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
}
