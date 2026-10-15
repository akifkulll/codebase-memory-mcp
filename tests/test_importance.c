/*
 * test_importance.c — index-time per-symbol importance score (pass_importance.c).
 * Ported from upstream e5f3aab0, adapted to the Zova-backed pipeline.
 *
 * Formula under test:
 *   importance = sqrt(num_refs) * priv * generic * distinct * test_penalty
 *
 * Three of these tests exist to catch failure modes that are SILENT — they
 * produce a wrong index with a fully green build:
 *
 *   - importance_pass_actually_runs_in_full_pipeline
 *       binds the predump registration. A hand-written PREDUMP_PASS_COUNT that
 *       lags the pass table skips the LAST-registered pass and nothing else
 *       notices. This test fails loudly if the score never lands.
 *   - importance_incremental_run_rescores_with_one_key
 *       binds the idempotent write-back and the incremental wiring. The
 *       incremental path exports nodes that already carry "importance"; a
 *       plain append yields {"importance":1.0,...,"importance":2.0} —
 *       property corruption, not a build failure.
 *   - importance_distinct_file_count_is_linear_in_group_size
 *       binds the cost shape: the distinct-file count must be computed once
 *       per distinct NAME, not once per node. The per-node shape is O(k^3) in
 *       a same-name group and is measurably disqualifying on real corpora.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "store/store.h"
#include "foundation/compat.h"
#include "foundation/log.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Shared helpers ───────────────────────────────────────────────────── */

/* Parse the numeric value of "importance" out of a properties_json blob.
 * Returns a sentinel (-999.0) when the key is absent — callers that care about
 * presence assert on imp_key_count() instead. */
static double imp_value(const char *json) {
    if (!json) {
        return -999.0;
    }
    const char *p = strstr(json, "\"importance\":");
    if (!p) {
        return -999.0;
    }
    return strtod(p + strlen("\"importance\":"), NULL);
}

/* How many times the key appears. The duplicate-key corruption this suite
 * guards against shows up here as 2. */
static int imp_key_count(const char *json) {
    if (!json) {
        return 0;
    }
    int n = 0;
    const char *p = json;
    while ((p = strstr(p, "\"importance\":")) != NULL) {
        n++;
        p += strlen("\"importance\":");
    }
    return n;
}

static void imp_run_pass(cbm_gbuf_t *gb) {
    atomic_int cancelled = 0;
    cbm_pipeline_ctx_t ctx = {
        .project_name = "test-proj",
        .repo_path = "/tmp/test",
        .gbuf = gb,
        .cancelled = &cancelled,
    };
    cbm_pipeline_pass_importance(&ctx);
}

/* Every stored node of the given label must carry exactly `expect` copies of
 * the key. Returns the number of nodes checked, or -1 on a store error. */
static int imp_check_label(cbm_store_t *s, const char *project, const char *label, int expect) {
    cbm_node_t *nodes = NULL;
    int count = 0;
    if (cbm_store_find_nodes_by_label(s, project, label, &nodes, &count) != CBM_STORE_OK) {
        return -1;
    }
    int bad = 0;
    for (int i = 0; i < count; i++) {
        if (imp_key_count(nodes[i].properties_json) != expect) {
            bad++;
            printf("    %s node '%s' props=%s\n", label, nodes[i].name ? nodes[i].name : "?",
                   nodes[i].properties_json ? nodes[i].properties_json : "(null)");
        }
    }
    cbm_store_free_nodes(nodes, count);
    return bad == 0 ? count : -1;
}

/* ── Registration: the pass must actually execute ─────────────────────── */

static int imp_write(const char *root, const char *rel, const char *content) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    return th_write_file(path, content);
}

/* A miscounted PREDUMP_PASS_COUNT silently drops the last-registered pass.
 * The score is the only observable, so assert on the score. */
TEST(importance_pass_actually_runs_in_full_pipeline) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char root[512];
    snprintf(root, sizeof(root), "%s/cbm_imp_run_XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    ASSERT_EQ(imp_write(root, "main.py",
                        "def helper():\n    return 1\n\n"
                        "def caller():\n    return helper() + helper()\n\n"
                        "class Widget:\n    def render(self):\n        return helper()\n"),
              0);

    cbm_pipeline_t *p = cbm_pipeline_new(root, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(cbm_pipeline_run(p), 0);
    char project[256];
    snprintf(project, sizeof(project), "%s", cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);

    cbm_store_t *s = th_open_repo_store(root);
    ASSERT_NOT_NULL(s);

    /* Non-vacuous: the corpus really produced symbols of each kind. */
    int fc = imp_check_label(s, project, "Function", 1);
    int mc = imp_check_label(s, project, "Method", 1);
    int cc = imp_check_label(s, project, "Class", 1);
    printf("    scored Function=%d Method=%d Class=%d\n", fc, mc, cc);
    ASSERT_GT(fc, 0);
    ASSERT_GT(mc, 0);
    ASSERT_GT(cc, 0);

    /* File nodes are deliberately excluded from scoring. */
    cbm_node_t *files = NULL;
    int filec = 0;
    ASSERT_EQ(cbm_store_find_nodes_by_label(s, project, "File", &files, &filec), CBM_STORE_OK);
    ASSERT_GT(filec, 0);
    for (int i = 0; i < filec; i++) {
        ASSERT_EQ(imp_key_count(files[i].properties_json), 0);
    }
    cbm_store_free_nodes(files, filec);

    cbm_store_close(s);
    th_rmtree(root);
    PASS();
#endif
}

/* ── Formula ──────────────────────────────────────────────────────────── */

TEST(importance_base_is_sqrt_of_incoming_refs) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);

    int64_t target =
        cbm_gbuf_upsert_node(gb, "Function", "target", "pkg.target", "pkg/main.go", 1, 1, "{}");
    int64_t c1 = cbm_gbuf_upsert_node(gb, "Function", "c1", "pkg.c1", "pkg/main.go", 2, 2, "{}");
    int64_t c2 = cbm_gbuf_upsert_node(gb, "Function", "c2", "pkg.c2", "pkg/main.go", 3, 3, "{}");
    int64_t c3 = cbm_gbuf_upsert_node(gb, "Function", "c3", "pkg.c3", "pkg/main.go", 4, 4, "{}");
    int64_t lonely =
        cbm_gbuf_upsert_node(gb, "Function", "lonely", "pkg.lonely", "pkg/main.go", 5, 5, "{}");
    ASSERT_GT(target, 0);
    ASSERT_GT(lonely, 0);

    cbm_gbuf_insert_edge(gb, c1, target, "CALLS", "{}");
    cbm_gbuf_insert_edge(gb, c2, target, "CALLS", "{}");
    cbm_gbuf_insert_edge(gb, c3, target, "USAGE", "{}"); /* spans BOTH edge types */

    imp_run_pass(gb);

    const cbm_gbuf_node_t *tn = cbm_gbuf_find_by_id(gb, target);
    ASSERT_NOT_NULL(tn);
    ASSERT_EQ(imp_key_count(tn->properties_json), 1);
    ASSERT_FLOAT_EQ(imp_value(tn->properties_json), sqrt(3.0), 1e-6);

    /* num_refs == 0 -> sqrt(0) == 0, still written. */
    const cbm_gbuf_node_t *ln = cbm_gbuf_find_by_id(gb, lonely);
    ASSERT_NOT_NULL(ln);
    ASSERT_EQ(imp_key_count(ln->properties_json), 1);
    ASSERT_FLOAT_EQ(imp_value(ln->properties_json), 0.0, 1e-9);

    cbm_gbuf_free(gb);
    PASS();
}

TEST(importance_private_names_are_demoted) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);

    int64_t priv =
        cbm_gbuf_upsert_node(gb, "Function", "_run", "pkg._run", "pkg/main.go", 1, 1, "{}");
    int64_t pub = cbm_gbuf_upsert_node(gb, "Function", "run", "pkg.run", "pkg/main.go", 2, 2, "{}");
    for (int i = 0; i < 4; i++) {
        char name[CBM_SZ_32];
        char qn[CBM_SZ_64];
        snprintf(name, sizeof(name), "p%d", i);
        snprintf(qn, sizeof(qn), "pkg.p%d", i);
        int64_t c =
            cbm_gbuf_upsert_node(gb, "Function", name, qn, "pkg/main.go", 10 + i, 10 + i, "{}");
        cbm_gbuf_insert_edge(gb, c, priv, "CALLS", "{}");
        snprintf(name, sizeof(name), "q%d", i);
        snprintf(qn, sizeof(qn), "pkg.q%d", i);
        c = cbm_gbuf_upsert_node(gb, "Function", name, qn, "pkg/main.go", 20 + i, 20 + i, "{}");
        cbm_gbuf_insert_edge(gb, c, pub, "CALLS", "{}");
    }

    imp_run_pass(gb);

    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, priv)->properties_json), sqrt(4.0) * 0.1,
                    1e-6);
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, pub)->properties_json), sqrt(4.0), 1e-6);

    cbm_gbuf_free(gb);
    PASS();
}

/* Generic = defined in >= 5 DISTINCT FILES. Two defs of one name in the same
 * file count once, so the boundary is tested on files, not on node count. */
TEST(importance_generic_names_are_demoted_on_distinct_files) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);

    /* "generic": 5 distinct files -> demoted. */
    int64_t generic = 0;
    for (int i = 0; i < 5; i++) {
        char qn[CBM_SZ_64];
        char file[CBM_SZ_64];
        snprintf(qn, sizeof(qn), "pkg%d.generic", i);
        snprintf(file, sizeof(file), "pkg%d/main.go", i);
        int64_t id = cbm_gbuf_upsert_node(gb, "Function", "generic", qn, file, 1, 1, "{}");
        if (i == 0) {
            generic = id;
        }
    }
    /* "narrow": 4 distinct files, but 6 nodes — two duplicates share a file,
     * so the DISTINCT count is 4 and it stays undemoted. */
    int64_t narrow = 0;
    for (int i = 0; i < 4; i++) {
        char qn[CBM_SZ_64];
        char file[CBM_SZ_64];
        snprintf(qn, sizeof(qn), "n%d.narrow", i);
        snprintf(file, sizeof(file), "n%d/main.go", i);
        int64_t id = cbm_gbuf_upsert_node(gb, "Function", "narrow", qn, file, 1, 1, "{}");
        if (i == 0) {
            narrow = id;
        }
    }
    cbm_gbuf_upsert_node(gb, "Function", "narrow", "n0.narrow_b", "n0/main.go", 9, 9, "{}");
    cbm_gbuf_upsert_node(gb, "Function", "narrow", "n1.narrow_b", "n1/main.go", 9, 9, "{}");

    for (int i = 0; i < 9; i++) {
        char name[CBM_SZ_32];
        char qn[CBM_SZ_64];
        snprintf(name, sizeof(name), "z%d", i);
        snprintf(qn, sizeof(qn), "z.z%d", i);
        int64_t c = cbm_gbuf_upsert_node(gb, "Function", name, qn, "z/main.go", i, i, "{}");
        cbm_gbuf_insert_edge(gb, c, i < 4 ? generic : narrow, "CALLS", "{}");
    }

    imp_run_pass(gb);

    /* generic: sqrt(4) * 0.1;  narrow: sqrt(5), undemoted. */
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, generic)->properties_json), sqrt(4.0) * 0.1,
                    1e-6);
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, narrow)->properties_json), sqrt(5.0), 1e-6);

    cbm_gbuf_free(gb);
    PASS();
}

TEST(importance_distinctive_names_are_promoted) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);

    /* snake_case >= 8, camelCase >= 8, and a plain long lowercase word that is
     * NOT distinctive (no '_', no hump). */
    int64_t snake = cbm_gbuf_upsert_node(gb, "Function", "load_user_profile",
                                         "pkg.load_user_profile", "pkg/a.go", 1, 1, "{}");
    int64_t camel = cbm_gbuf_upsert_node(gb, "Function", "loadUserProfile", "pkg.loadUserProfile",
                                         "pkg/b.go", 1, 1, "{}");
    int64_t plain =
        cbm_gbuf_upsert_node(gb, "Function", "aggregate", "pkg.aggregate", "pkg/c.go", 1, 1, "{}");
    int64_t shortnm =
        cbm_gbuf_upsert_node(gb, "Function", "get_x", "pkg.get_x", "pkg/d.go", 1, 1, "{}");

    const int64_t targets[] = {snake, camel, plain, shortnm};
    for (int t = 0; t < 4; t++) {
        char name[CBM_SZ_32];
        char qn[CBM_SZ_64];
        snprintf(name, sizeof(name), "cc%d", t);
        snprintf(qn, sizeof(qn), "z.cc%d", t);
        int64_t c = cbm_gbuf_upsert_node(gb, "Function", name, qn, "z/main.go", t, t, "{}");
        cbm_gbuf_insert_edge(gb, c, targets[t], "CALLS", "{}");
    }

    imp_run_pass(gb);

    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, snake)->properties_json), 1.0 * 10.0, 1e-6);
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, camel)->properties_json), 1.0 * 10.0, 1e-6);
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, plain)->properties_json), 1.0, 1e-6);
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, shortnm)->properties_json), 1.0, 1e-6);

    cbm_gbuf_free(gb);
    PASS();
}

/* Test scaffolding is demoted two ways: living in a test file (the graph's own
 * cbm_is_test_path classifier) or being the target of a TESTS edge. */
TEST(importance_test_scaffolding_is_demoted) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);

    int64_t in_test_file = cbm_gbuf_upsert_node(gb, "Function", "fixture", "t.fixture",
                                                "pkg/thing_test.go", 1, 1, "{}");
    int64_t tests_target =
        cbm_gbuf_upsert_node(gb, "Function", "helper", "pkg.helper", "pkg/util.go", 1, 1, "{}");
    int64_t plain =
        cbm_gbuf_upsert_node(gb, "Function", "worker", "pkg.worker", "pkg/util.go", 2, 2, "{}");
    /* A production file whose NAME merely contains "test" must NOT be demoted. */
    int64_t testutil = cbm_gbuf_upsert_node(gb, "Function", "spawn", "pkg.spawn",
                                            "pkg/testutil_helpers.go", 1, 1, "{}");

    const int64_t targets[] = {in_test_file, tests_target, plain, testutil};
    for (int t = 0; t < 4; t++) {
        char name[CBM_SZ_32];
        char qn[CBM_SZ_64];
        snprintf(name, sizeof(name), "d%d", t);
        snprintf(qn, sizeof(qn), "z.d%d", t);
        int64_t c = cbm_gbuf_upsert_node(gb, "Function", name, qn, "z/main.go", t, t, "{}");
        cbm_gbuf_insert_edge(gb, c, targets[t], "CALLS", "{}");
    }
    int64_t tfn = cbm_gbuf_upsert_node(gb, "Function", "TestHelper", "t.TestHelper",
                                       "pkg/util_test.go", 5, 5, "{}");
    cbm_gbuf_insert_edge(gb, tfn, tests_target, "TESTS", "{}");

    imp_run_pass(gb);

    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, in_test_file)->properties_json), 1.0 * 0.1,
                    1e-6);
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, tests_target)->properties_json), 1.0 * 0.1,
                    1e-6);
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, plain)->properties_json), 1.0, 1e-6);
    ASSERT_FLOAT_EQ(imp_value(cbm_gbuf_find_by_id(gb, testutil)->properties_json), 1.0, 1e-6);

    cbm_gbuf_free(gb);
    PASS();
}

/* ── Idempotent write-back ────────────────────────────────────────────── */

TEST(importance_append_prop_overwrites_instead_of_duplicating) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);
    int64_t id = cbm_gbuf_upsert_node(gb, "Function", "f", "p.f", "p/a.go", 1, 1,
                                      "{\"loop_depth\":2,\"recursive\":false}");
    cbm_gbuf_node_t *n = (cbm_gbuf_node_t *)cbm_gbuf_find_by_id(gb, id);
    ASSERT_NOT_NULL(n);

    cbm_pipeline_importance_append_prop(n, 1.5);
    ASSERT_EQ(imp_key_count(n->properties_json), 1);
    ASSERT_FLOAT_EQ(imp_value(n->properties_json), 1.5, 1e-6);

    cbm_pipeline_importance_append_prop(n, 42.25);
    printf("    after second write: %s\n", n->properties_json);
    ASSERT_EQ(imp_key_count(n->properties_json), 1);
    ASSERT_FLOAT_EQ(imp_value(n->properties_json), 42.25, 1e-6);

    /* Sibling keys survive the overwrite. */
    ASSERT_TRUE(strstr(n->properties_json, "\"loop_depth\":2") != NULL);
    ASSERT_TRUE(strstr(n->properties_json, "\"recursive\":false") != NULL);

    /* A key that is NOT last moves to the end, keeping the tail, so the
     * stored properties match a fresh full index whatever pass appended
     * after it (the incremental complexity re-append). */
    free(n->properties_json);
    n->properties_json = strdup("{\"importance\":0.000000,\"tail\":7}");
    ASSERT_NOT_NULL(n->properties_json);
    cbm_pipeline_importance_append_prop(n, 3.0);
    ASSERT_STR_EQ(n->properties_json, "{\"tail\":7,\"importance\":3.000000}");
    free(n->properties_json);
    n->properties_json = strdup("{\"a\":1,\"importance\":1.000000,\"recursive\":false}");
    ASSERT_NOT_NULL(n->properties_json);
    cbm_pipeline_importance_append_prop(n, 1.0);
    ASSERT_STR_EQ(n->properties_json, "{\"a\":1,\"recursive\":false,\"importance\":1.000000}");
    free(n->properties_json);
    n->properties_json = strdup("{\"importance\":2.000000}");
    ASSERT_NOT_NULL(n->properties_json);
    cbm_pipeline_importance_append_prop(n, 4.0);
    ASSERT_STR_EQ(n->properties_json, "{\"importance\":4.000000}");

    /* A string VALUE that merely contains the key text is not a key. */
    free(n->properties_json);
    n->properties_json = strdup("{\"doc\":\"see \\\"importance\\\": below\"}");
    ASSERT_NOT_NULL(n->properties_json);
    cbm_pipeline_importance_append_prop(n, 2.0);
    ASSERT_FLOAT_EQ(imp_value(n->properties_json), 2.0, 1e-6);
    ASSERT_TRUE(strstr(n->properties_json, "\"doc\":") != NULL);

    /* Empty object and a non-object blob. */
    free(n->properties_json);
    n->properties_json = strdup("{}");
    ASSERT_NOT_NULL(n->properties_json);
    cbm_pipeline_importance_append_prop(n, 0.5);
    ASSERT_STR_EQ(n->properties_json, "{\"importance\":0.500000}");

    free(n->properties_json);
    n->properties_json = strdup("not json");
    ASSERT_NOT_NULL(n->properties_json);
    cbm_pipeline_importance_append_prop(n, 9.0);
    ASSERT_STR_EQ(n->properties_json, "not json");

    cbm_gbuf_free(gb);
    PASS();
}

/* Running the pass twice over the same graph — the shape the incremental path
 * produces when it re-scores an already-scored rehydrated buffer. */
TEST(importance_pass_is_idempotent_across_reruns) {
    cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
    ASSERT_NOT_NULL(gb);
    int64_t t =
        cbm_gbuf_upsert_node(gb, "Function", "target", "pkg.target", "pkg/main.go", 1, 1, "{}");
    int64_t c =
        cbm_gbuf_upsert_node(gb, "Function", "caller", "pkg.caller", "pkg/main.go", 2, 2, "{}");
    cbm_gbuf_insert_edge(gb, c, t, "CALLS", "{}");

    imp_run_pass(gb);
    char first[CBM_SZ_512];
    snprintf(first, sizeof(first), "%s", cbm_gbuf_find_by_id(gb, t)->properties_json);
    imp_run_pass(gb);
    const char *second = cbm_gbuf_find_by_id(gb, t)->properties_json;

    printf("    run1=%s run2=%s\n", first, second);
    ASSERT_EQ(imp_key_count(second), 1);
    ASSERT_STR_EQ(first, second); /* re-scoring is a fixed point */

    cbm_gbuf_free(gb);
    PASS();
}

/* Importance of the single stored Function named `name`, or -999 when absent.
 * *refs receives its stored incoming CALLS + USAGE edge count. */
static double imp_stored_function(cbm_store_t *s, const char *project, const char *name,
                                  int *refs) {
    cbm_node_t *nodes = NULL;
    int count = 0;
    double value = -999.0;
    *refs = -1;
    if (cbm_store_find_nodes_by_label(s, project, "Function", &nodes, &count) != CBM_STORE_OK) {
        return value;
    }
    for (int i = 0; i < count; i++) {
        if (nodes[i].name && strcmp(nodes[i].name, name) == 0) {
            value = imp_value(nodes[i].properties_json);
            cbm_edge_t *edges = NULL;
            int ne = 0;
            if (cbm_store_find_edges_by_target(s, nodes[i].id, &edges, &ne) == CBM_STORE_OK) {
                *refs = 0;
                for (int k = 0; k < ne; k++) {
                    if (strcmp(edges[k].type, "CALLS") == 0 ||
                        strcmp(edges[k].type, "USAGE") == 0) {
                        (*refs)++;
                    }
                }
                cbm_store_free_edges(edges, ne);
            }
        }
    }
    cbm_store_free_nodes(nodes, count);
    return value;
}

/* End-to-end on the incremental route. Index, add a second caller of
 * `helper` in the changed file, index again. `helper` lives in the UNCHANGED
 * file, so its new score proves the incremental post-passes rescored over the
 * whole exported graph with the relinked inbound edges, and every stored
 * symbol must still carry exactly one key. */
TEST(importance_incremental_run_rescores_with_one_key) {
#if !CBM_WITH_ZOVA
    PASS();
#else
    char root[512];
    snprintf(root, sizeof(root), "%s/cbm_imp_incr_XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    ASSERT_EQ(imp_write(root, "a.py", "def helper():\n    return 1\n"), 0);
    ASSERT_EQ(imp_write(root, "b.py",
                        "from a import helper\n\ndef caller():\n    return helper()\n"),
              0);

    cbm_pipeline_t *p1 = cbm_pipeline_new(root, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p1);
    ASSERT_EQ(cbm_pipeline_run(p1), 0);
    ASSERT_EQ(cbm_pipeline_get_last_route(p1), CBM_PIPELINE_ROUTE_FULL);
    char project[256];
    snprintf(project, sizeof(project), "%s", cbm_pipeline_project_name(p1));
    cbm_pipeline_free(p1);

    cbm_store_t *s = th_open_repo_store(root);
    ASSERT_NOT_NULL(s);
    /* The fork also stores a module-level USAGE edge for the import, so the
     * score is derived from the stored CALLS + USAGE count, not hard-coded. */
    int refs_before = 0;
    double before = imp_stored_function(s, project, "helper", &refs_before);
    cbm_store_close(s);
    ASSERT_GT(refs_before, 0);
    ASSERT_FLOAT_EQ(before, sqrt((double)refs_before), 1e-6);

    ASSERT_EQ(imp_write(root, "b.py",
                        "from a import helper\n\ndef caller():\n    return helper()\n\n"
                        "def other_caller():\n    return helper()\n"),
              0);
    cbm_pipeline_t *p2 = cbm_pipeline_new(root, NULL, CBM_MODE_FULL);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ(cbm_pipeline_run(p2), 0);
    /* Route proof: a silent fall-back to a full reindex would leave the
     * incremental wiring untested. */
    ASSERT_EQ(cbm_pipeline_get_last_route(p2), CBM_PIPELINE_ROUTE_INCREMENTAL);
    cbm_pipeline_free(p2);

    s = th_open_repo_store(root);
    ASSERT_NOT_NULL(s);
    int refs_after = 0;
    double after = imp_stored_function(s, project, "helper", &refs_after);
    printf("    helper importance: full=%f incremental=%f\n", before, after);
    ASSERT_EQ(refs_after, refs_before + 1);
    ASSERT_FLOAT_EQ(after, sqrt((double)refs_after), 1e-6);
    int fc = imp_check_label(s, project, "Function", 1);
    ASSERT_GT(fc, 0);
    cbm_store_close(s);

    th_rmtree(root);
    PASS();
#endif
}

/* ── Cost shape ───────────────────────────────────────────────────────── */

/* The distinct-file count must be computed once per distinct NAME. Computing
 * it per NODE makes a same-name group of size k cost O(k^3) — measurably
 * disqualifying on real corpora. Assert on the pass's own work counter, not
 * on wall time: doubling a same-name group must roughly double the work. */
TEST(importance_distinct_file_count_is_linear_in_group_size) {
    enum { K = 200 };

    uint64_t visits[2];
    for (int leg = 0; leg < 2; leg++) {
        int k = (leg == 0) ? K : 2 * K;
        cbm_gbuf_t *gb = cbm_gbuf_new("test-proj", "/tmp/test");
        ASSERT_NOT_NULL(gb);
        for (int i = 0; i < k; i++) {
            char qn[CBM_SZ_64];
            char file[CBM_SZ_64];
            snprintf(qn, sizeof(qn), "pkg%d.toString", i);
            snprintf(file, sizeof(file), "pkg%d/a.java", i);
            ASSERT_GT(cbm_gbuf_upsert_node(gb, "Method", "toString", qn, file, 1, 1, "{}"), 0);
        }
        uint64_t v0 = atomic_load_explicit(&g_importance_name_visits, memory_order_relaxed);
        imp_run_pass(gb);
        visits[leg] = atomic_load_explicit(&g_importance_name_visits, memory_order_relaxed) - v0;
        cbm_gbuf_free(gb);
    }

    double ratio = (visits[0] > 0) ? (double)visits[1] / (double)visits[0] : 0.0;
    printf("    name visits k=%d -> %llu, k=%d -> %llu (ratio %.2f; linear ~2, per-node ~4)\n", K,
           (unsigned long long)visits[0], 2 * K, (unsigned long long)visits[1], ratio);

    /* Non-vacuous: the group really was scanned. */
    ASSERT_GT((long long)visits[0], 0);
    /* Memoized: one scan of the whole group, so visits == k exactly. */
    ASSERT_EQ((long long)visits[0], (long long)K);
    ASSERT_EQ((long long)visits[1], (long long)(2 * K));
    ASSERT_TRUE(ratio <= 2.75);
    PASS();
}

/* ── Suite ────────────────────────────────────────────────────────────── */

SUITE(importance) {
    RUN_TEST(importance_pass_actually_runs_in_full_pipeline);
    RUN_TEST(importance_base_is_sqrt_of_incoming_refs);
    RUN_TEST(importance_private_names_are_demoted);
    RUN_TEST(importance_generic_names_are_demoted_on_distinct_files);
    RUN_TEST(importance_distinctive_names_are_promoted);
    RUN_TEST(importance_test_scaffolding_is_demoted);
    RUN_TEST(importance_append_prop_overwrites_instead_of_duplicating);
    RUN_TEST(importance_pass_is_idempotent_across_reruns);
    RUN_TEST(importance_incremental_run_rescores_with_one_key);
    RUN_TEST(importance_distinct_file_count_is_linear_in_group_size);
}
