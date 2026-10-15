/*
 * test_call_reference.c — Production-graph contract for callable values.
 *
 * CALLS is invocation-only. A function or method named as a value (passed as an
 * argument, aliased, or written with explicit reference syntax such as Java
 * `A::m` or Kotlin `::f`) becomes CALL_REFERENCE only when the language LSP
 * proves one exact callable at that occurrence. Anything ambiguous stays USAGE.
 * Every case indexes through the MCP index_repository flow, so the edges are
 * read back from the published store.
 */
#include "test_framework.h"
#include "repro_harness.h"

#include <store/store.h>

#include <string.h>

static cbm_store_t *crf_index(RProj *lp, const RFile *files, int nfiles) {
    memset(lp, 0, sizeof(*lp));
    const char *tmp = getenv("TMPDIR");
    snprintf(lp->tmpdir, sizeof(lp->tmpdir), "%s/cbm_callref_XXXXXX",
             tmp && tmp[0] ? tmp : "/tmp");
    if (!cbm_mkdtemp(lp->tmpdir))
        return NULL;
    rh_to_fwd_slashes(lp->tmpdir);
    for (int i = 0; i < nfiles; i++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", lp->tmpdir, files[i].name);
        char *slash = strrchr(path, '/');
        if (slash && slash > path + strlen(lp->tmpdir)) {
            *slash = '\0';
            cbm_mkdir_p(path, 0755);
            *slash = '/';
        }
        FILE *f = fopen(path, "wb");
        if (!f)
            return NULL;
        fputs(files[i].content, f);
        fclose(f);
    }
    return rh_open_indexed(lp);
}

static cbm_store_t *crf_index1(RProj *lp, const char *name, const char *content) {
    RFile f = {name, content};
    return crf_index(lp, &f, 1);
}

/* Edges of `type` from a node named `source` to a node named `target`. */
static int crf_count(cbm_store_t *store, const char *project, const char *type,
                     const char *source, const char *target) {
    cbm_edge_t *edges = NULL;
    int n = 0;
    if (cbm_store_find_edges_by_type(store, project, type, &edges, &n) != CBM_STORE_OK)
        return -1;
    int hits = 0;
    for (int i = 0; i < n; i++) {
        cbm_node_t s = {0}, t = {0};
        if (cbm_store_find_node_by_id(store, edges[i].source_id, &s) == CBM_STORE_OK &&
            cbm_store_find_node_by_id(store, edges[i].target_id, &t) == CBM_STORE_OK && s.name &&
            t.name && strcmp(s.name, source) == 0 && strcmp(t.name, target) == 0)
            hits++;
        cbm_node_free_fields(&s);
        cbm_node_free_fields(&t);
    }
    if (edges)
        cbm_store_free_edges(edges, n);
    return hits;
}

typedef struct {
    int ref, call, usage;
} CrfEdges;

static CrfEdges crf_edges(cbm_store_t *store, const char *project, const char *source,
                          const char *target) {
    CrfEdges e = {crf_count(store, project, "CALL_REFERENCE", source, target),
                  crf_count(store, project, "CALLS", source, target),
                  crf_count(store, project, "USAGE", source, target)};
    fprintf(stderr, "  [callref] %s -> %s: ref=%d calls=%d usage=%d\n", source, target, e.ref,
            e.call, e.usage);
    return e;
}

/* Index one file, read the edges source->target, clean up. */
static CrfEdges crf_run1(const char *name, const char *content, const char *source,
                         const char *target) {
    RProj lp;
    cbm_store_t *store = crf_index1(&lp, name, content);
    CrfEdges e = {-1, -1, -1};
    if (store)
        e = crf_edges(store, lp.project, source, target);
    rh_cleanup(&lp, store);
    return e;
}

#define ASSERT_EXACT_REF(e)                                                                        \
    do {                                                                                           \
        ASSERT_EQ((e).ref, 1);                                                                     \
        ASSERT_EQ((e).call, 0);                                                                    \
        ASSERT_EQ((e).usage, 0);                                                                   \
    } while (0)

/* ── TypeScript / JavaScript ───────────────────────────────────── */

TEST(callref_ts_direct_argument) {
    CrfEdges e = crf_run1("main.ts",
                          "function handler(): void {}\n"
                          "function register(cb: () => void): void {}\n"
                          "function bootstrap(): void { register(handler); }\n",
                          "bootstrap", "handler");
    ASSERT_EXACT_REF(e);
    PASS();
}

TEST(callref_ts_invocation_stays_calls) {
    CrfEdges e = crf_run1("main.ts",
                          "function handler(): void {}\n"
                          "function bootstrap(): void { handler(); }\n",
                          "bootstrap", "handler");
    ASSERT_EQ(e.call, 1);
    ASSERT_EQ(e.ref, 0);
    PASS();
}

TEST(callref_ts_local_shadow_is_not_global) {
    CrfEdges e = crf_run1("main.ts",
                          "function handler(): void {}\n"
                          "function register(cb: () => void): void {}\n"
                          "function bootstrap(): void {\n"
                          "  const handler = (): void => {};\n"
                          "  register(handler);\n"
                          "}\n",
                          "bootstrap", "handler");
    ASSERT_EQ(e.ref, 0);
    ASSERT_EQ(e.call, 0);
    PASS();
}

/* ── Python ────────────────────────────────────────────────────── */

TEST(callref_python_direct_argument) {
    CrfEdges e = crf_run1("app.py",
                          "def handler():\n    pass\n"
                          "def accept(cb):\n    pass\n"
                          "def caller():\n    accept(handler)\n",
                          "caller", "handler");
    ASSERT_EXACT_REF(e);
    PASS();
}

TEST(callref_python_keyword_argument) {
    CrfEdges e = crf_run1("app.py",
                          "def handler():\n    pass\n"
                          "def accept(cb=None):\n    pass\n"
                          "def caller():\n    accept(cb=handler)\n",
                          "caller", "handler");
    ASSERT_EXACT_REF(e);
    PASS();
}

TEST(callref_python_assignment_rhs) {
    CrfEdges e = crf_run1("app.py",
                          "def handler():\n    pass\n"
                          "def caller():\n    cb = handler\n    return cb\n",
                          "caller", "handler");
    ASSERT_EXACT_REF(e);
    PASS();
}

TEST(callref_python_conditional_value_stays_usage) {
    CrfEdges e = crf_run1("app.py",
                          "def handler():\n    pass\n"
                          "def other():\n    pass\n"
                          "def accept(cb):\n    pass\n"
                          "def caller(flag):\n    accept(handler if flag else other)\n",
                          "caller", "handler");
    ASSERT_EQ(e.ref, 0);
    ASSERT_EQ(e.call, 0);
    PASS();
}

TEST(callref_python_local_shadow_is_not_global) {
    CrfEdges e = crf_run1("app.py",
                          "def handler():\n    pass\n"
                          "def accept(cb):\n    pass\n"
                          "def caller():\n    handler = 1\n    accept(handler)\n",
                          "caller", "handler");
    ASSERT_EQ(e.ref, 0);
    ASSERT_EQ(e.call, 0);
    PASS();
}

/* ── Go ────────────────────────────────────────────────────────── */

TEST(callref_go_direct_argument) {
    CrfEdges e = crf_run1("main.go",
                          "package main\n\n"
                          "func handler() {}\n"
                          "func accept(cb func()) {}\n"
                          "func caller() { accept(handler) }\n",
                          "caller", "handler");
    ASSERT_EXACT_REF(e);
    PASS();
}

/* ── C ─────────────────────────────────────────────────────────── */

TEST(callref_c_function_pointer_argument) {
    CrfEdges e = crf_run1("main.c",
                          "static int cmp(const void *a, const void *b) { return 0; }\n"
                          "void sort_with(int (*f)(const void *, const void *)) {}\n"
                          "void caller(void) { sort_with(cmp); }\n",
                          "caller", "cmp");
    ASSERT_EXACT_REF(e);
    PASS();
}

/* ── Java / Kotlin explicit reference syntax ───────────────────── */

TEST(callref_java_method_reference_leaves_calls) {
    CrfEdges e = crf_run1("App.java",
                          "import java.util.List;\n"
                          "class App {\n"
                          "  static void handle(String s) {}\n"
                          "  void run(List<String> xs) { xs.forEach(App::handle); }\n"
                          "}\n",
                          "run", "handle");
    ASSERT_EXACT_REF(e);
    PASS();
}

TEST(callref_kotlin_callable_reference_leaves_calls) {
    CrfEdges e = crf_run1("App.kt",
                          "fun handler(): Unit {}\n"
                          "fun accept(cb: () -> Unit): Unit {}\n"
                          "fun caller(): Unit { accept(::handler) }\n",
                          "caller", "handler");
    ASSERT_EXACT_REF(e);
    PASS();
}

/* A branch picks one of two references, so neither is proven: USAGE only. */
TEST(callref_kotlin_branch_reference_stays_usage) {
    CrfEdges e = crf_run1("App.kt",
                          "fun handler(): Unit {}\n"
                          "fun other(): Unit {}\n"
                          "fun accept(cb: () -> Unit): Unit {}\n"
                          "fun caller(f: Boolean): Unit { accept(if (f) ::handler else ::other) }\n",
                          "caller", "handler");
    ASSERT_EQ(e.ref, 0);
    ASSERT_EQ(e.call, 0);
    PASS();
}

/* ── C# / Rust / PHP ───────────────────────────────────────────── */

TEST(callref_csharp_method_group_argument) {
    CrfEdges e = crf_run1("App.cs",
                          "using System;\n"
                          "class App {\n"
                          "  static void Handle(string s) {}\n"
                          "  static void Accept(Action<string> cb) {}\n"
                          "  void Run() { Accept(Handle); }\n"
                          "}\n",
                          "Run", "Handle");
    ASSERT_EXACT_REF(e);
    PASS();
}

TEST(callref_rust_function_argument) {
    CrfEdges e = crf_run1("main.rs",
                          "fn handler(x: i32) -> i32 { x }\n"
                          "fn accept(f: fn(i32) -> i32) {}\n"
                          "fn caller() { accept(handler); }\n",
                          "caller", "handler");
    ASSERT_EXACT_REF(e);
    PASS();
}

TEST(callref_rust_local_shadow_is_not_global) {
    CrfEdges e = crf_run1("main.rs",
                          "fn handler(x: i32) -> i32 { x }\n"
                          "fn accept(f: i32) {}\n"
                          "fn caller() { let handler = 1; accept(handler); }\n",
                          "caller", "handler");
    ASSERT_EQ(e.ref, 0);
    ASSERT_EQ(e.call, 0);
    PASS();
}

TEST(callref_php_first_class_callable) {
    CrfEdges e = crf_run1("app.php",
                          "<?php\n"
                          "function handler(string $s): void {}\n"
                          "function caller(): void { $f = handler(...); }\n",
                          "caller", "handler");
    ASSERT_EXACT_REF(e);
    PASS();
}

/* ── Provenance and incremental replacement ───────────────────── */

/* Write `content` to lp's file `name`, re-run index_repository on the same
 * server (incremental route), and reopen the store. */
static cbm_store_t *crf_reindex(RProj *lp, cbm_store_t *store, const char *name,
                                const char *content) {
    if (store)
        cbm_store_close(store);
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", lp->tmpdir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    fputs(content, f);
    fclose(f);
    char args[700];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", lp->tmpdir);
    char *resp = cbm_mcp_handle_tool(lp->srv, "index_repository", args);
    free(resp);
    return th_open_project_store(lp->project);
}

static const RFile CRF_TWO_FILES[] = {
    {"lib.ts", "export function handler(): void {}\n"},
    {"app.ts", "import { handler } from './lib';\n"
               "function accept(cb: () => void): void {}\n"
               "export function caller(): void { accept(handler); }\n"},
};

/* The referenced function becomes a plain value in a changed file; the
 * unchanged referencing file must not keep a CALL_REFERENCE to it. */
TEST(callref_incremental_drops_reference_to_noncallable) {
    RProj lp;
    cbm_store_t *store = crf_index(&lp, CRF_TWO_FILES, 2);
    ASSERT_NOT_NULL(store);
    CrfEdges before = crf_edges(store, lp.project, "caller", "handler");
    store = crf_reindex(&lp, store, "lib.ts", "export const handler = 3;\n");
    CrfEdges after = {-1, -1, -1};
    if (store)
        after = crf_edges(store, lp.project, "caller", "handler");
    rh_cleanup(&lp, store);
    ASSERT_EXACT_REF(before);
    ASSERT_EQ(after.ref, 0);
    ASSERT_EQ(after.call, 0);
    PASS();
}

/* Positive control: an unrelated edit to the target file keeps the edge. */
TEST(callref_incremental_keeps_reference_on_unrelated_edit) {
    RProj lp;
    cbm_store_t *store = crf_index(&lp, CRF_TWO_FILES, 2);
    ASSERT_NOT_NULL(store);
    store = crf_reindex(&lp, store, "lib.ts",
                        "export function handler(): void {}\n"
                        "export function other(): void {}\n");
    CrfEdges after = {-1, -1, -1};
    if (store)
        after = crf_edges(store, lp.project, "caller", "handler");
    rh_cleanup(&lp, store);
    ASSERT_EXACT_REF(after);
    PASS();
}

/* The edge records its LSP provenance, and trace_path exposes it as a closed
 * class only when include_evidence is set. */
TEST(callref_trace_path_include_evidence) {
    RProj lp;
    cbm_store_t *store = crf_index1(&lp, "app.py",
                                    "def handler():\n    pass\n"
                                    "def accept(cb):\n    pass\n"
                                    "def caller():\n    accept(handler)\n");
    ASSERT_NOT_NULL(store);
    cbm_edge_t *edges = NULL;
    int n = 0;
    bool props_ok = cbm_store_find_edges_by_type(store, lp.project, "CALL_REFERENCE", &edges,
                                                 &n) == CBM_STORE_OK &&
                    n == 1 && edges[0].properties_json &&
                    strstr(edges[0].properties_json, "\"strategy\":\"lsp_") &&
                    strstr(edges[0].properties_json, "\"confidence\":");
    if (edges)
        cbm_store_free_edges(edges, n);

    char args[700];
    snprintf(args, sizeof(args),
             "{\"function_name\":\"caller\",\"project\":\"%s\",\"direction\":\"outbound\","
             "\"edge_types\":[\"CALL_REFERENCE\"]}",
             lp.project);
    char *plain = cbm_mcp_handle_tool(lp.srv, "trace_path", args);
    snprintf(args, sizeof(args),
             "{\"function_name\":\"caller\",\"project\":\"%s\",\"direction\":\"outbound\","
             "\"edge_types\":[\"CALL_REFERENCE\"],\"include_evidence\":true}",
             lp.project);
    char *evidence = cbm_mcp_handle_tool(lp.srv, "trace_path", args);
    bool plain_ok = plain && strstr(plain, "handler") && !strstr(plain, "strategy");
    bool evidence_ok = evidence && strstr(evidence, "handler") &&
                       strstr(evidence, "\\\"strategy\\\":\\\"lsp\\\"") &&
                       strstr(evidence, "\\\"confidence\\\":") &&
                       !strstr(evidence, "value_reference");
    if (!evidence_ok)
        fprintf(stderr, "  [callref] evidence response: %s\n", evidence ? evidence : "(null)");
    free(plain);
    free(evidence);
    rh_cleanup(&lp, store);
    ASSERT_TRUE(props_ok);
    ASSERT_TRUE(plain_ok);
    ASSERT_TRUE(evidence_ok);
    PASS();
}

/* A CALLS-only trace takes the native workspace walk, which has no edges;
 * include_evidence must still report the CALLS edge's provenance. */
TEST(callref_trace_calls_only_include_evidence) {
    RProj lp;
    cbm_store_t *store = crf_index1(&lp, "main.ts",
                                    "function helper(): void {}\n"
                                    "function caller(): void { helper(); }\n");
    ASSERT_NOT_NULL(store);
    char args[700];
    snprintf(args, sizeof(args),
             "{\"function_name\":\"caller\",\"project\":\"%s\",\"direction\":\"outbound\","
             "\"edge_types\":[\"CALLS\"],\"include_evidence\":true}",
             lp.project);
    char *resp = cbm_mcp_handle_tool(lp.srv, "trace_path", args);
    bool ok = resp && strstr(resp, "helper") && strstr(resp, "\\\"strategy\\\":\\\"lsp\\\"") &&
              strstr(resp, "\\\"confidence\\\":");
    if (!ok)
        fprintf(stderr, "  [callref] calls evidence response: %s\n", resp ? resp : "(null)");
    free(resp);
    rh_cleanup(&lp, store);
    ASSERT_TRUE(ok);
    PASS();
}

/* Copy every `type` edge of the published graph, with its endpoints, into a
 * SQLite store. ids[] maps published node ids to SQLite ids. */
static void crf_copy_edges(cbm_store_t *from, const char *project, const char *type,
                           cbm_store_t *to, int64_t ids[][2], int *nids) {
    cbm_edge_t *edges = NULL;
    int n = 0;
    if (cbm_store_find_edges_by_type(from, project, type, &edges, &n) != CBM_STORE_OK)
        return;
    for (int i = 0; i < n; i++) {
        int64_t end[2] = {edges[i].source_id, edges[i].target_id};
        int64_t mapped[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            for (int j = 0; j < *nids; j++)
                if (ids[j][0] == end[k])
                    mapped[k] = ids[j][1];
            cbm_node_t node = {0};
            if (!mapped[k] && cbm_store_find_node_by_id(from, end[k], &node) == CBM_STORE_OK) {
                mapped[k] = cbm_store_upsert_node(to, &node);
                ids[*nids][0] = end[k];
                ids[(*nids)++][1] = mapped[k];
            }
            cbm_node_free_fields(&node);
        }
        cbm_edge_t copy = {.project = edges[i].project, .source_id = mapped[0],
                           .target_id = mapped[1], .type = edges[i].type,
                           .properties_json = edges[i].properties_json};
        cbm_store_insert_edge(to, &copy);
    }
    cbm_store_free_edges(edges, n);
}

/* "name@hop" per visited node in BFS order, then "from>to:provenance" per edge. */
static void crf_trace_sig(cbm_store_t *store, int64_t start, char *buf, size_t size) {
    const char *types[] = {"CALL_REFERENCE"};
    cbm_traverse_result_t tr = {0};
    int len = snprintf(buf, size, "rc=%d", cbm_store_bfs(store, start, "outbound", types, 1, 3,
                                                         100, &tr));
    for (int i = 0; i < tr.visited_count; i++)
        len += snprintf(buf + len, size - (size_t)len, " %s@%d", tr.visited[i].node.name,
                        tr.visited[i].hop);
    len += snprintf(buf + len, size - (size_t)len, " edges=%d", tr.edge_count);
    /* Edge order is unspecified on both routes; compare them sorted. */
    char edges[8][128];
    int n = tr.edge_count < 8 ? tr.edge_count : 8;
    for (int i = 0; i < n; i++)
        snprintf(edges[i], sizeof(edges[i]), "%s>%s:%s", tr.edges[i].from_name,
                 tr.edges[i].to_name,
                 tr.edges[i].properties_json &&
                         strstr(tr.edges[i].properties_json, "\"strategy\":\"lsp_")
                     ? "lsp"
                     : "none");
    qsort(edges, (size_t)n, sizeof(edges[0]), (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i < n; i++)
        len += snprintf(buf + len, size - (size_t)len, " %s", edges[i]);
    cbm_store_traverse_free(&tr);
}

/* The Zova workspace walk and the SQL walk return the same CALL_REFERENCE
 * trace (nodes, hops, edges, provenance) over the same graph, and neither
 * follows the CALLS edges beside it. */
TEST(callref_bfs_workspace_matches_sqlite) {
    RProj lp;
    cbm_store_t *store = crf_index1(&lp, "main.ts",
                                    "function leaf(): void {}\n"
                                    "function register(cb: () => void): void {}\n"
                                    "function mid(): void { register(leaf); }\n"
                                    "function helper(): void {}\n"
                                    "function caller(): void { register(mid); helper(); }\n");
    ASSERT_NOT_NULL(store);
    cbm_store_t *sql = cbm_store_open_memory();
    cbm_store_upsert_project(sql, lp.project, lp.tmpdir);
    int64_t ids[64][2];
    int nids = 0;
    crf_copy_edges(store, lp.project, "CALL_REFERENCE", sql, ids, &nids);
    crf_copy_edges(store, lp.project, "CALLS", sql, ids, &nids);

    int64_t start_ws = 0, start_sql = 0;
    for (int j = 0; j < nids; j++) {
        cbm_node_t node = {0};
        if (cbm_store_find_node_by_id(store, ids[j][0], &node) == CBM_STORE_OK && node.name &&
            strcmp(node.name, "caller") == 0) {
            start_ws = ids[j][0];
            start_sql = ids[j][1];
        }
        cbm_node_free_fields(&node);
    }
    char ws[1024], lite[1024];
    crf_trace_sig(store, start_ws, ws, sizeof(ws));
    crf_trace_sig(sql, start_sql, lite, sizeof(lite));
    fprintf(stderr, "  [callref] workspace: %s\n  [callref] sqlite:    %s\n", ws, lite);
    cbm_store_close(sql);
    rh_cleanup(&lp, store);
    ASSERT_STR_EQ(ws, "rc=0 mid@1 leaf@2 edges=2 caller>mid:lsp mid>leaf:lsp");
    ASSERT_STR_EQ(lite, ws);
    PASS();
}

/* Every strategy a CALL_REFERENCE row can carry maps to a known class. */
TEST(callref_strategy_class_vocabulary) {
    static const char *const lsp[] = {
        "lsp_func_value_reference",      "lsp_callable_value_reference",
        "lsp_method_group_reference",    "lsp_method_ref",
        "lsp_method_ref_ctor",           "lsp_method_ref_ctor_synth",
        "lsp_kt_callable_ref",           "lsp_ts_import_value_reference",
        "lsp_ts_local_value_reference",  "lsp_direct",
    };
    for (size_t i = 0; i < sizeof(lsp) / sizeof(lsp[0]); i++) {
        ASSERT_STR_EQ(cbm_mcp_edge_strategy_class(lsp[i]), "lsp");
    }
    ASSERT_STR_EQ(cbm_mcp_edge_strategy_class("lsp_unresolved"), "unresolved");
    ASSERT_STR_EQ(cbm_mcp_edge_strategy_class("php_self_static"), "language_rule");
    ASSERT_STR_EQ(cbm_mcp_edge_strategy_class("unique_name"), "heuristic");
    ASSERT_NULL(cbm_mcp_edge_strategy_class(""));
    ASSERT_NULL(cbm_mcp_edge_strategy_class(NULL));
    PASS();
}

SUITE(call_reference) {
    RUN_TEST(callref_ts_direct_argument);
    RUN_TEST(callref_ts_invocation_stays_calls);
    RUN_TEST(callref_ts_local_shadow_is_not_global);
    RUN_TEST(callref_python_direct_argument);
    RUN_TEST(callref_python_keyword_argument);
    RUN_TEST(callref_python_assignment_rhs);
    RUN_TEST(callref_python_conditional_value_stays_usage);
    RUN_TEST(callref_python_local_shadow_is_not_global);
    RUN_TEST(callref_go_direct_argument);
    RUN_TEST(callref_c_function_pointer_argument);
    RUN_TEST(callref_java_method_reference_leaves_calls);
    RUN_TEST(callref_kotlin_callable_reference_leaves_calls);
    RUN_TEST(callref_kotlin_branch_reference_stays_usage);
    RUN_TEST(callref_csharp_method_group_argument);
    RUN_TEST(callref_rust_function_argument);
    RUN_TEST(callref_rust_local_shadow_is_not_global);
    RUN_TEST(callref_php_first_class_callable);
    RUN_TEST(callref_incremental_drops_reference_to_noncallable);
    RUN_TEST(callref_incremental_keeps_reference_on_unrelated_edit);
    RUN_TEST(callref_trace_path_include_evidence);
    RUN_TEST(callref_trace_calls_only_include_evidence);
    RUN_TEST(callref_bfs_workspace_matches_sqlite);
    RUN_TEST(callref_strategy_class_vocabulary);
}
