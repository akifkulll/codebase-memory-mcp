/*
 * test_zig_lsp.c — Tests for Zig type-aware call resolution.
 *
 * Categories:
 *   - Container methods through typed locals, `try`, `@This()` aliases
 *   - Same-named calls in one function keep their own call site
 *   - Field chains, slices + for captures, if captures, `.?`, `&x`
 *   - Bare and container-qualified function calls
 *   - Fail-closed: std, anytype and unknown receivers emit nothing
 *   - anytype: argument types collected at resolved calls, then the callee
 *     walked again with each bound (cbm_run_zig_lsp_cross_ex)
 *   - Cross-file: cbm_run_zig_lsp_cross resolves through imports, return types
 *     (`!Self`, `!Statement`) and recorded field types
 */
#include "test_framework.h"
#include "cbm.h"
#include "lsp/zig_lsp.h"

static CBMFileResult *extract_zig(const char *source) {
    return cbm_extract_file(source, (int)strlen(source), CBM_LANG_ZIG, "test", "src/main.zig", 0,
                            NULL, NULL);
}

static void dump_resolved(const CBMResolvedCallArray *arr) {
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        printf("    %s -> %s [%s %.2f @%u]\n", rc->caller_qn ? rc->caller_qn : "(null)",
               rc->callee_qn ? rc->callee_qn : "(null)", rc->strategy ? rc->strategy : "(null)",
               rc->confidence, rc->site_start_byte);
    }
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = s ? strlen(s) : 0;
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Rows whose caller and callee QNs end with the given suffixes. */
static int count_rows(const CBMResolvedCallArray *arr, const char *caller, const char *callee) {
    int n = 0;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (ends_with(rc->caller_qn, caller) && ends_with(rc->callee_qn, callee)) {
            n++;
        }
    }
    return n;
}

static int require_rows(const CBMResolvedCallArray *arr, const char *caller, const char *callee,
                        int expected) {
    int n = count_rows(arr, caller, callee);
    if (n != expected) {
        printf("  %s -> %s: expected %d rows, have %d\n", caller, callee, expected, n);
        dump_resolved(arr);
    }
    return n;
}

/* Source text of a resolved row's call site. */
static char *site_text(const char *source, const CBMResolvedCall *rc, char *buf, size_t cap) {
    size_t len = rc->site_end_byte > rc->site_start_byte
                     ? (size_t)(rc->site_end_byte - rc->site_start_byte)
                     : 0;
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(buf, source + rc->site_start_byte, len);
    buf[len] = '\0';
    return buf;
}

static const char *DB_SRC = "const std = @import(\"std\");\n"
                            "\n"
                            "const Publication = struct {\n"
                            "    fn cleanup(self: *Publication) void {\n"
                            "        _ = self;\n"
                            "    }\n"
                            "};\n"
                            "\n"
                            "const Statement = struct {\n"
                            "    db: *Database,\n"
                            "    pub fn step(self: *Statement) bool {\n"
                            "        _ = self;\n"
                            "        return false;\n"
                            "    }\n"
                            "    pub fn deinit(self: *Statement) void {\n"
                            "        _ = self;\n"
                            "    }\n"
                            "};\n"
                            "\n"
                            "pub const Database = struct {\n"
                            "    const Self = @This();\n"
                            "    stmts: []Statement,\n"
                            "    pub fn open() !Self {\n"
                            "        return Self{ .stmts = &.{} };\n"
                            "    }\n"
                            "    pub fn prepare(self: *Self) !Statement {\n"
                            "        return Statement{ .db = self };\n"
                            "    }\n"
                            "    pub fn first(self: *Self) ?*Statement {\n"
                            "        _ = self;\n"
                            "        return null;\n"
                            "    }\n"
                            "    pub fn deinit(self: *Self) void {\n"
                            "        _ = self;\n"
                            "    }\n"
                            "    fn reset(self: *Self) void {\n"
                            "        self.deinit();\n"
                            "    }\n"
                            "};\n"
                            "\n"
                            "fn helper() void {}\n"
                            "\n"
                            "pub fn run() !void {\n"
                            "    var db = try Database.open();\n"
                            "    defer db.deinit();\n"
                            "    var st = try db.prepare();\n"
                            "    st.deinit();\n"
                            "    for (db.stmts) |*s| {\n"
                            "        _ = s.step();\n"
                            "    }\n"
                            "    if (db.first()) |s| {\n"
                            "        s.deinit();\n"
                            "    }\n"
                            "    db.first().?.deinit();\n"
                            "    st.db.deinit();\n"
                            "    const p = &st;\n"
                            "    _ = p.step();\n"
                            "    helper();\n"
                            "}\n"
                            "\n"
                            "fn generic(x: anytype) void {\n"
                            "    x.cleanup();\n"
                            "}\n"
                            "\n"
                            "test \"tmp dir\" {\n"
                            "    var tmp = std.testing.tmpDir(.{});\n"
                            "    defer tmp.cleanup();\n"
                            "}\n";

TEST(zig_lsp_methods_through_typed_locals) {
    CBMFileResult *r = extract_zig(DB_SRC);
    ASSERT_NOT_NULL(r);
    const CBMResolvedCallArray *a = &r->resolved_calls;
    ASSERT_EQ(require_rows(a, "src.main.run", "src.main.Database.open", 1), 1);
    ASSERT_EQ(require_rows(a, "src.main.run", "src.main.Database.prepare", 1), 1);
    ASSERT_EQ(require_rows(a, "src.main.run", "src.main.Database.first", 2), 2);
    /* db.deinit() and st.db.deinit() (field *Database). */
    ASSERT_EQ(require_rows(a, "src.main.run", "src.main.Database.deinit", 2), 2);
    /* st.deinit(), s.deinit() in the if capture, db.first().?.deinit(). */
    ASSERT_EQ(require_rows(a, "src.main.run", "src.main.Statement.deinit", 3), 3);
    /* s.step() in the for capture over []Statement, p.step() through &st. */
    ASSERT_EQ(require_rows(a, "src.main.run", "src.main.Statement.step", 2), 2);
    ASSERT_EQ(require_rows(a, "src.main.run", "src.main.helper", 1), 1);
    /* self.deinit() through `const Self = @This();`. */
    ASSERT_EQ(require_rows(a, "src.main.Database.reset", "src.main.Database.deinit", 1), 1);
    cbm_free_result(r);
    PASS();
}

TEST(zig_lsp_rows_carry_their_call_site) {
    CBMFileResult *r = extract_zig(DB_SRC);
    ASSERT_NOT_NULL(r);
    char buf[128];
    int checked = 0;
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[i];
        if (!ends_with(rc->caller_qn, "src.main.run") || !ends_with(rc->callee_qn, ".deinit")) {
            continue;
        }
        ASSERT_EQ(rc->kind, CBM_RESOLVED_INVOCATION);
        ASSERT_TRUE(rc->site_end_byte > rc->site_start_byte);
        const char *text = site_text(DB_SRC, rc, buf, sizeof(buf));
        bool db_site = strcmp(text, "db.deinit()") == 0 || strcmp(text, "st.db.deinit()") == 0;
        if (ends_with(rc->callee_qn, "Database.deinit")) {
            ASSERT_TRUE(db_site);
        } else {
            ASSERT_FALSE(db_site);
        }
        checked++;
    }
    ASSERT_EQ(checked, 5);

    /* Each extracted call names the site the matching row carries. */
    int joined = 0;
    for (int i = 0; i < r->calls.count; i++) {
        const CBMCall *c = &r->calls.items[i];
        if (!c->callee_name || !ends_with(c->callee_name, "deinit") ||
            !ends_with(c->enclosing_func_qn, "src.main.run")) {
            continue;
        }
        bool found = false;
        for (int j = 0; j < r->resolved_calls.count; j++) {
            if (r->resolved_calls.items[j].site_start_byte == c->site_start_byte) {
                found = true;
            }
        }
        ASSERT_TRUE(found);
        joined++;
    }
    ASSERT_EQ(joined, 5);
    cbm_free_result(r);
    PASS();
}

TEST(zig_lsp_unknown_receivers_fail_closed) {
    CBMFileResult *r = extract_zig(DB_SRC);
    ASSERT_NOT_NULL(r);
    /* tmp is a std value, x is anytype: neither may bind to Publication.cleanup. */
    ASSERT_EQ(require_rows(&r->resolved_calls, "", "Publication.cleanup", 0), 0);
    ASSERT_EQ(require_rows(&r->resolved_calls, "src.main.generic", "", 0), 0);
    cbm_free_result(r);
    PASS();
}

TEST(zig_lsp_nested_containers_and_qualified_calls) {
    const char *src = "const Outer = struct {\n"
                      "    pub const Inner = struct {\n"
                      "        pub fn make() Inner {\n"
                      "            return .{};\n"
                      "        }\n"
                      "        pub fn poke(self: Inner) void {\n"
                      "            _ = self;\n"
                      "        }\n"
                      "    };\n"
                      "    pub fn helper2() void {}\n"
                      "    fn go() void {\n"
                      "        helper2();\n"
                      "        const i = Inner.make();\n"
                      "        i.poke();\n"
                      "    }\n"
                      "};\n"
                      "\n"
                      "fn top() void {\n"
                      "    Outer.helper2();\n"
                      "    const I = Outer.Inner;\n"
                      "    var v: I = I.make();\n"
                      "    v.poke();\n"
                      "    const w = Outer.Inner{};\n"
                      "    w.poke();\n"
                      "}\n";
    CBMFileResult *r = extract_zig(src);
    ASSERT_NOT_NULL(r);
    const CBMResolvedCallArray *a = &r->resolved_calls;
    ASSERT_EQ(require_rows(a, "src.main.Outer.go", "src.main.Outer.helper2", 1), 1);
    ASSERT_EQ(require_rows(a, "src.main.Outer.go", "src.main.Outer.Inner.make", 1), 1);
    ASSERT_EQ(require_rows(a, "src.main.Outer.go", "src.main.Outer.Inner.poke", 1), 1);
    ASSERT_EQ(require_rows(a, "src.main.top", "src.main.Outer.helper2", 1), 1);
    ASSERT_EQ(require_rows(a, "src.main.top", "src.main.Outer.Inner.make", 1), 1);
    ASSERT_EQ(require_rows(a, "src.main.top", "src.main.Outer.Inner.poke", 2), 2);
    cbm_free_result(r);
    PASS();
}

/* std is an opaque namespace with no graph nodes, but its containers carry
 * element types on to project code: list items, map values, iterators,
 * getOrPut results and Allocator.create. */
static const char *STD_SRC = "const std = @import(\"std\");\n"
                             "const Allocator = std.mem.Allocator;\n"
                             "const List = std.ArrayList;\n"
                             "\n"
                             "pub const Row = struct {\n"
                             "    pub fn deinit(self: *Row) void {\n"
                             "        _ = self;\n"
                             "    }\n"
                             "    pub fn close(self: *Row) void {\n"
                             "        _ = self;\n"
                             "    }\n"
                             "};\n"
                             "\n"
                             "pub const Store = struct {\n"
                             "    rows: std.ArrayList(Row),\n"
                             "    by_name: std.StringHashMap(*Row),\n"
                             "    by_id: std.AutoHashMap(u32, Row),\n"
                             "\n"
                             "    pub fn run(self: *Store, alloc: Allocator) !void {\n"
                             "        for (self.rows.items) |*r| r.deinit();\n"
                             "        if (self.by_name.get(\"x\")) |r| r.close();\n"
                             "        var it = self.by_name.iterator();\n"
                             "        while (it.next()) |e| e.value_ptr.*.deinit();\n"
                             "        var vals = self.by_id.valueIterator();\n"
                             "        while (vals.next()) |v| v.close();\n"
                             "        const one = try alloc.create(Row);\n"
                             "        one.close();\n"
                             "        var list = List(Row).init(alloc);\n"
                             "        list.items[0].close();\n"
                             "        if (list.pop()) |last| last.deinit();\n"
                             "        const gop = try self.by_id.getOrPut(1);\n"
                             "        gop.value_ptr.deinit();\n"
                             "        var empty: std.ArrayList(Row) = .empty;\n"
                             "        empty.items[0].deinit();\n"
                             "        std.debug.print(\"x\", .{});\n"
                             "        list.deinit();\n"
                             "    }\n"
                             "};\n";

TEST(zig_lsp_std_containers_carry_element_types) {
    CBMFileResult *r = extract_zig(STD_SRC);
    ASSERT_NOT_NULL(r);
    const CBMResolvedCallArray *a = &r->resolved_calls;
    const char *run = "src.main.Store.run";
    /* items capture, iterator value_ptr.*, pop() capture, getOrPut, `.empty`. */
    ASSERT_EQ(require_rows(a, run, "src.main.Row.deinit", 5), 5);
    /* get() capture, valueIterator, Allocator.create, items[0]. */
    ASSERT_EQ(require_rows(a, run, "src.main.Row.close", 4), 4);
    /* Calls into std itself (print, list.deinit) bind to nothing. */
    ASSERT_EQ(require_rows(a, run, "", 9), 9);
    cbm_free_result(r);
    PASS();
}

TEST(zig_lsp_cross_file_std_field_and_return_types) {
    const char *src = "const store = @import(\"store.zig\");\n"
                      "\n"
                      "pub fn run(s: *store.Store) !void {\n"
                      "    for (s.rows.items) |r| r.deinit();\n"
                      "    if (s.cache.get(\"k\")) |r| r.close();\n"
                      "    var l = try s.list();\n"
                      "    l.items[0].close();\n"
                      "}\n";
    CBMLSPDef defs[] = {
        {.qualified_name = "test.src.main.run", .short_name = "run", .label = "Function",
         .def_module_qn = "test.src.main", .return_types = "!void", .lang = CBM_LANG_ZIG},
        /* `const std = @import("std");` in store.zig. */
        {.qualified_name = "test.src.store.std", .short_name = "std", .label = "Variable",
         .def_module_qn = "test.src.store", .return_types = "std", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.store.Row", .short_name = "Row", .label = "Struct",
         .def_module_qn = "test.src.store", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.store.Row.deinit", .short_name = "deinit", .label = "Method",
         .receiver_type = "test.src.store.Row", .def_module_qn = "test.src.store",
         .return_types = "void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.store.Row.close", .short_name = "close", .label = "Method",
         .receiver_type = "test.src.store.Row", .def_module_qn = "test.src.store",
         .return_types = "void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.store.Store", .short_name = "Store", .label = "Struct",
         .def_module_qn = "test.src.store",
         .field_defs = "rows:std.ArrayList(Row)|cache:std.StringHashMap(*Row)",
         .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.store.Store.list", .short_name = "list", .label = "Method",
         .receiver_type = "test.src.store.Store", .def_module_qn = "test.src.store",
         .return_types = "!std.ArrayList(Row)", .lang = CBM_LANG_ZIG},
    };
    const char *imp_names[] = {"store"};
    const char *imp_qns[] = {"test.src.store"};
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));
    cbm_run_zig_lsp_cross(&arena, src, (int)strlen(src), "test.src.main", defs,
                          (int)(sizeof(defs) / sizeof(defs[0])), imp_names, imp_qns, 1, NULL, &out);
    const char *run = "test.src.main.run";
    ASSERT_EQ(require_rows(&out, run, "test.src.store.Row.deinit", 1), 1);
    ASSERT_EQ(require_rows(&out, run, "test.src.store.Row.close", 2), 2);
    ASSERT_EQ(require_rows(&out, run, "test.src.store.Store.list", 1), 1);
    cbm_arena_destroy(&arena);
    PASS();
}

/* Project generics: a type function names the struct it returns (members
 * are `<fn>.<member>`), and `Pool(Row)` binds `T` while its members are
 * evaluated. A switch of containers shares the function's QN. */
static const char *GEN_SRC = "pub const Row = struct {\n"
                             "    pub fn close(self: *Row) void {\n"
                             "        _ = self;\n"
                             "    }\n"
                             "};\n"
                             "pub fn Pool(comptime T: type) type {\n"
                             "    return struct {\n"
                             "        const Self = @This();\n"
                             "        items: []T,\n"
                             "        pub fn init() Self {\n"
                             "            return .{ .items = &.{} };\n"
                             "        }\n"
                             "        pub fn first(self: *Self) *T {\n"
                             "            return &self.items[0];\n"
                             "        }\n"
                             "        pub fn deinit(self: *Self) void {\n"
                             "            _ = self;\n"
                             "        }\n"
                             "    };\n"
                             "}\n"
                             "pub fn Pair(comptime K: type, comptime V: type) type {\n"
                             "    return struct { key: K, value: V };\n"
                             "}\n"
                             "const RowPool = Pool(Row);\n"
                             "fn StoreOps(comptime kind: u8) type {\n"
                             "    return switch (kind) {\n"
                             "        0 => struct {\n"
                             "            fn copy() void {}\n"
                             "        },\n"
                             "        else => struct {\n"
                             "            fn copy() void {}\n"
                             "        },\n"
                             "    };\n"
                             "}\n"
                             "pub fn use(p: *RowPool, q: Pool(Row), pair: Pair(u32, Row)) void {\n"
                             "    p.first().close();\n"
                             "    p.deinit();\n"
                             "    var local = Pool(Row).init();\n"
                             "    local.first().close();\n"
                             "    for (q.items) |*r| r.close();\n"
                             "    pair.value.close();\n"
                             "    StoreOps(0).copy();\n"
                             "}\n";

TEST(zig_lsp_generic_type_functions_bind_their_parameters) {
    CBMFileResult *r = extract_zig(GEN_SRC);
    ASSERT_NOT_NULL(r);
    const CBMResolvedCallArray *a = &r->resolved_calls;
    const char *use = "src.main.use";
    /* first() returns *T; items is []T; Pair's value is V. */
    ASSERT_EQ(require_rows(a, use, "src.main.Row.close", 4), 4);
    ASSERT_EQ(require_rows(a, use, "src.main.Pool.first", 2), 2);
    ASSERT_EQ(require_rows(a, use, "src.main.Pool.deinit", 1), 1);
    ASSERT_EQ(require_rows(a, use, "src.main.Pool.init", 1), 1);
    ASSERT_EQ(require_rows(a, use, "src.main.StoreOps.copy", 1), 1);
    /* The type function calls themselves: Pool(Row), StoreOps(0). */
    ASSERT_EQ(require_rows(a, use, "src.main.Pool", 1), 1);
    ASSERT_EQ(require_rows(a, use, "src.main.StoreOps", 1), 1);
    ASSERT_EQ(require_rows(a, use, "", 11), 11);
    cbm_free_result(r);
    PASS();
}

TEST(zig_lsp_cross_file_generic_type_functions) {
    const char *src = "const pool = @import(\"pool.zig\");\n"
                      "\n"
                      "pub fn run(p: *pool.Pool(pool.Row)) void {\n"
                      "    p.first().close();\n"
                      "    for (p.items) |r| r.close();\n"
                      "    var q = pool.Pool(pool.Row).init();\n"
                      "    q.first().close();\n"
                      "}\n";
    CBMLSPDef defs[] = {
        {.qualified_name = "test.src.main.run", .short_name = "run", .label = "Function",
         .def_module_qn = "test.src.main", .return_types = "void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.pool.Row", .short_name = "Row", .label = "Struct",
         .def_module_qn = "test.src.pool", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.pool.Row.close", .short_name = "close", .label = "Method",
         .receiver_type = "test.src.pool.Row", .def_module_qn = "test.src.pool",
         .return_types = "void", .lang = CBM_LANG_ZIG},
        /* `pub fn Pool(comptime T: type) type`: the Function and, from
         * pass_lsp_cross, the type it names. */
        {.qualified_name = "test.src.pool.Pool", .short_name = "Pool", .label = "Function",
         .def_module_qn = "test.src.pool", .return_types = "type", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.pool.Pool", .short_name = "Pool", .label = "Struct",
         .def_module_qn = "test.src.pool", .field_defs = "items:[]T", .type_params = "T",
         .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.pool.Pool.init", .short_name = "init", .label = "Method",
         .receiver_type = "test.src.pool.Pool", .def_module_qn = "test.src.pool",
         .return_types = "Self", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.pool.Pool.first", .short_name = "first", .label = "Method",
         .receiver_type = "test.src.pool.Pool", .def_module_qn = "test.src.pool",
         .return_types = "*T", .lang = CBM_LANG_ZIG},
    };
    const char *imp_names[] = {"pool"};
    const char *imp_qns[] = {"test.src.pool"};
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));
    cbm_run_zig_lsp_cross(&arena, src, (int)strlen(src), "test.src.main", defs,
                          (int)(sizeof(defs) / sizeof(defs[0])), imp_names, imp_qns, 1, NULL, &out);
    const char *run = "test.src.main.run";
    ASSERT_EQ(require_rows(&out, run, "test.src.pool.Row.close", 3), 3);
    ASSERT_EQ(require_rows(&out, run, "test.src.pool.Pool.first", 2), 2);
    ASSERT_EQ(require_rows(&out, run, "test.src.pool.Pool.init", 1), 1);
    cbm_arena_destroy(&arena);
    PASS();
}

/* db.zig as cross-file defs, plus an unrelated Other with a deinit too. */
static const CBMLSPDef ANY_DB_DEFS[] = {
    {.qualified_name = "test.src.db.Database", .short_name = "Database", .label = "Struct",
     .def_module_qn = "test.src.db", .lang = CBM_LANG_ZIG},
    {.qualified_name = "test.src.db.Database.prepare", .short_name = "prepare", .label = "Method",
     .receiver_type = "test.src.db.Database", .def_module_qn = "test.src.db",
     .return_types = "!Statement", .lang = CBM_LANG_ZIG},
    {.qualified_name = "test.src.db.Database.deinit", .short_name = "deinit", .label = "Method",
     .receiver_type = "test.src.db.Database", .def_module_qn = "test.src.db",
     .return_types = "void", .lang = CBM_LANG_ZIG},
    {.qualified_name = "test.src.db.Statement", .short_name = "Statement", .label = "Struct",
     .def_module_qn = "test.src.db", .lang = CBM_LANG_ZIG},
    {.qualified_name = "test.src.db.Statement.deinit", .short_name = "deinit", .label = "Method",
     .receiver_type = "test.src.db.Statement", .def_module_qn = "test.src.db",
     .return_types = "void", .lang = CBM_LANG_ZIG},
    {.qualified_name = "test.src.other.Other", .short_name = "Other", .label = "Struct",
     .def_module_qn = "test.src.other", .lang = CBM_LANG_ZIG},
    {.qualified_name = "test.src.other.Other.deinit", .short_name = "deinit", .label = "Method",
     .receiver_type = "test.src.other.Other", .def_module_qn = "test.src.other",
     .return_types = "void", .lang = CBM_LANG_ZIG},
    /* support.zig: `seed(conn: anytype, n: usize)`, `each(all: anytype)`. */
    {.qualified_name = "test.src.support.seed", .short_name = "seed", .label = "Function",
     .def_module_qn = "test.src.support", .return_types = "void", .type_params = "*,_",
     .lang = CBM_LANG_ZIG},
    {.qualified_name = "test.src.support.each", .short_name = "each", .label = "Function",
     .def_module_qn = "test.src.support", .return_types = "void", .type_params = "*",
     .lang = CBM_LANG_ZIG},
    {.qualified_name = "test.src.support.helper", .short_name = "helper", .label = "Function",
     .def_module_qn = "test.src.support", .return_types = "void", .lang = CBM_LANG_ZIG},
};

static bool has_arg(const CBMZigArgTypeArray *a, const char *callee, int param, const char *type) {
    for (int i = 0; i < a->count; i++) {
        if (strcmp(a->items[i].callee_qn, callee) == 0 && a->items[i].param == param &&
            strcmp(a->items[i].type, type) == 0) {
            return true;
        }
    }
    return false;
}

TEST(zig_lsp_param_kinds_mark_type_and_anytype_parameters) {
    CBMArena arena;
    cbm_arena_init(&arena);
    ASSERT_STR_EQ(cbm_zig_param_kinds(&arena, "(comptime T: type, x: anytype, n: usize)"),
                  "T,*,_");
    ASSERT_STR_EQ(cbm_zig_param_kinds(&arena, "(self: *Self, f: fn (a: u8, b: anytype) void)"),
                  "_,_");
    ASSERT_STR_EQ(cbm_zig_param_kinds(&arena, "()"), "");
    ASSERT_NULL(cbm_zig_param_kinds(&arena, NULL));
    cbm_arena_destroy(&arena);
    PASS();
}

TEST(zig_lsp_cross_file_anytype_collects_argument_types) {
    const char *src = "const db = @import(\"db.zig\");\n"
                      "const support = @import(\"support.zig\");\n"
                      "\n"
                      "const S = struct {\n"
                      "    pub fn use(self: S, x: anytype) void {\n"
                      "        _ = self;\n"
                      "        _ = x;\n"
                      "    }\n"
                      "};\n"
                      "\n"
                      "pub fn run(conn: *db.Database, all: []db.Statement) void {\n"
                      "    support.seed(conn, 3);\n"
                      "    support.each(all);\n"
                      "    const s = S{};\n"
                      "    s.use(conn);\n"
                      "    support.seed(mystery, 1);\n"
                      "    support.helper();\n"
                      "}\n";
    int n = (int)(sizeof(ANY_DB_DEFS) / sizeof(ANY_DB_DEFS[0]));
    CBMLSPDef defs[16];
    memcpy(defs, ANY_DB_DEFS, sizeof(ANY_DB_DEFS));
    defs[n++] = (CBMLSPDef){.qualified_name = "test.src.main.run", .short_name = "run",
                            .label = "Function", .def_module_qn = "test.src.main",
                            .return_types = "void", .lang = CBM_LANG_ZIG};
    defs[n++] = (CBMLSPDef){.qualified_name = "test.src.main.S", .short_name = "S",
                            .label = "Struct", .def_module_qn = "test.src.main",
                            .lang = CBM_LANG_ZIG};
    defs[n++] = (CBMLSPDef){.qualified_name = "test.src.main.S.use", .short_name = "use",
                            .label = "Method", .receiver_type = "test.src.main.S",
                            .def_module_qn = "test.src.main", .return_types = "void",
                            .type_params = "_,*", .lang = CBM_LANG_ZIG};
    const char *imp_names[] = {"db", "support"};
    const char *imp_qns[] = {"test.src.db", "test.src.support"};
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));
    CBMZigArgTypeArray collect = {NULL, 0, 0, &arena};
    cbm_run_zig_lsp_cross_ex(&arena, src, (int)strlen(src), "test.src.main", defs, n, imp_names,
                             imp_qns, 2, NULL, &out, NULL, 0, &collect);
    ASSERT_TRUE(has_arg(&collect, "test.src.support.seed", 0, "test.src.db.Database"));
    ASSERT_TRUE(has_arg(&collect, "test.src.support.each", 0, "[]test.src.db.Statement"));
    /* A value-receiver call passes the receiver as parameter 0. */
    ASSERT_TRUE(has_arg(&collect, "test.src.main.S.use", 1, "test.src.db.Database"));
    /* `mystery` has no type, and `3` is not an anytype argument's container. */
    ASSERT_EQ(collect.count, 3);
    free(collect.items);
    cbm_arena_destroy(&arena);
    PASS();
}

TEST(zig_lsp_cross_file_anytype_walks_bound_parameters) {
    const char *src = "const db = @import(\"db.zig\");\n"
                      "\n"
                      "pub fn seed(conn: anytype, n: usize) void {\n"
                      "    _ = n;\n"
                      "    conn.deinit();\n"
                      "    const st = conn.prepare() catch return;\n"
                      "    st.deinit();\n"
                      "    helper();\n"
                      "}\n"
                      "\n"
                      "fn helper() void {}\n"
                      "\n"
                      "pub fn each(all: anytype) void {\n"
                      "    for (all) |s| s.deinit();\n"
                      "}\n"
                      "\n"
                      "pub fn unused(conn: anytype) void {\n"
                      "    conn.deinit();\n"
                      "}\n";
    /* Sorted by callee, then parameter, then type. */
    const CBMZigArgType args[] = {
        {"test.src.support.each", 0, "[]test.src.db.Statement"},
        {"test.src.support.seed", 0, "test.src.db.Database"},
        {"test.src.support.seed", 0, "test.src.other.Other"},
    };
    const char *imp_names[] = {"db"};
    const char *imp_qns[] = {"test.src.db"};
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));
    cbm_run_zig_lsp_cross_ex(&arena, src, (int)strlen(src), "test.src.support",
                             (CBMLSPDef *)ANY_DB_DEFS,
                             (int)(sizeof(ANY_DB_DEFS) / sizeof(ANY_DB_DEFS[0])), imp_names,
                             imp_qns, 1, NULL, &out, args, 3, NULL);
    const char *seed = "test.src.support.seed";
    /* One row per call site: the first type bound wins. */
    ASSERT_EQ(require_rows(&out, seed, "test.src.db.Database.deinit", 1), 1);
    ASSERT_EQ(require_rows(&out, seed, "test.src.other.Other.deinit", 0), 0);
    ASSERT_EQ(require_rows(&out, seed, "test.src.db.Database.prepare", 1), 1);
    ASSERT_EQ(require_rows(&out, seed, "test.src.db.Statement.deinit", 1), 1);
    ASSERT_EQ(require_rows(&out, seed, "test.src.support.helper", 1), 1);
    ASSERT_EQ(require_rows(&out, "test.src.support.each", "test.src.db.Statement.deinit", 1), 1);
    ASSERT_EQ(require_rows(&out, "test.src.support.unused", "deinit", 0), 0);
    int anytype_rows = 0;
    for (int i = 0; i < out.count; i++) {
        anytype_rows += strcmp(out.items[i].strategy, "lsp_zig_anytype") == 0;
    }
    ASSERT_EQ(anytype_rows, 4);
    cbm_arena_destroy(&arena);
    PASS();
}

TEST(zig_lsp_cross_file_imports_returns_and_fields) {
    const char *src = "const db = @import(\"db.zig\");\n"
                      "const Database = db.Database;\n"
                      "\n"
                      "pub fn run() !void {\n"
                      "    var conn = try db.Database.open();\n"
                      "    defer conn.deinit();\n"
                      "    var st = try conn.prepare();\n"
                      "    defer st.deinit();\n"
                      "    _ = st.step();\n"
                      "    st.conn.close();\n"
                      "    var other = try Database.open();\n"
                      "    other.deinit();\n"
                      "}\n"
                      "\n"
                      "pub fn rows(stmt: *db.Statement, maybe: ?*const db.Statement, all: []db.Statement) void {\n"
                      "    _ = stmt.step();\n"
                      "    if (maybe) |m| m.deinit();\n"
                      "    all[0].deinit();\n"
                      "    all.deinit();\n"
                      "}\n";
    CBMLSPDef defs[] = {
        {.qualified_name = "test.src.main.run", .short_name = "run", .label = "Function",
         .def_module_qn = "test.src.main", .return_types = "!void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.main.rows", .short_name = "rows", .label = "Function",
         .def_module_qn = "test.src.main", .return_types = "void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Database", .short_name = "Database", .label = "Struct",
         .def_module_qn = "test.src.db", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Database.open", .short_name = "open", .label = "Method",
         .receiver_type = "test.src.db.Database", .def_module_qn = "test.src.db",
         .return_types = "!Self", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Database.prepare", .short_name = "prepare",
         .label = "Method", .receiver_type = "test.src.db.Database", .def_module_qn = "test.src.db",
         .return_types = "!Statement", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Database.deinit", .short_name = "deinit", .label = "Method",
         .receiver_type = "test.src.db.Database", .def_module_qn = "test.src.db",
         .return_types = "void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Statement", .short_name = "Statement", .label = "Struct",
         .def_module_qn = "test.src.db", .field_defs = "conn:*Conn", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Statement.step", .short_name = "step", .label = "Method",
         .receiver_type = "test.src.db.Statement", .def_module_qn = "test.src.db",
         .return_types = "bool", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Statement.deinit", .short_name = "deinit",
         .label = "Method", .receiver_type = "test.src.db.Statement", .def_module_qn = "test.src.db",
         .return_types = "void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Conn", .short_name = "Conn", .label = "Struct",
         .def_module_qn = "test.src.db", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Conn.close", .short_name = "close", .label = "Method",
         .receiver_type = "test.src.db.Conn", .def_module_qn = "test.src.db",
         .return_types = "void", .lang = CBM_LANG_ZIG},
        /* Decoys: same short names in a module the file does not import. */
        {.qualified_name = "test.src.other.Statement", .short_name = "Statement", .label = "Struct",
         .def_module_qn = "test.src.other", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.other.Statement.step", .short_name = "step", .label = "Method",
         .receiver_type = "test.src.other.Statement", .def_module_qn = "test.src.other",
         .return_types = "bool", .lang = CBM_LANG_ZIG},
    };
    const char *imp_names[] = {"db"};
    const char *imp_qns[] = {"test.src.db"};
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));
    cbm_run_zig_lsp_cross(&arena, src, (int)strlen(src), "test.src.main", defs,
                          (int)(sizeof(defs) / sizeof(defs[0])), imp_names, imp_qns, 1, NULL, &out);
    const char *run = "test.src.main.run";
    ASSERT_EQ(require_rows(&out, run, "test.src.db.Database.open", 2), 2);
    ASSERT_EQ(require_rows(&out, run, "test.src.db.Database.prepare", 1), 1);
    ASSERT_EQ(require_rows(&out, run, "test.src.db.Database.deinit", 2), 2);
    ASSERT_EQ(require_rows(&out, run, "test.src.db.Statement.deinit", 1), 1);
    ASSERT_EQ(require_rows(&out, run, "test.src.db.Statement.step", 1), 1);
    ASSERT_EQ(require_rows(&out, run, "test.src.db.Conn.close", 1), 1);
    /* `*db.Statement` parses with the prefix on `db`; the slice itself has no methods. */
    const char *rows = "test.src.main.rows";
    ASSERT_EQ(require_rows(&out, rows, "test.src.db.Statement.step", 1), 1);
    ASSERT_EQ(require_rows(&out, rows, "test.src.db.Statement.deinit", 2), 2);
    ASSERT_EQ(require_rows(&out, "", "test.src.other.Statement.step", 0), 0);
    cbm_arena_destroy(&arena);
    PASS();
}

TEST(zig_lsp_cross_file_follows_aliases_and_reexports) {
    /* A CLI reaches the library through a build module whose root re-exports
     * it (`pub const Database = db.Database;`); the library spells a return
     * type through its own import (`!sqlite.Statement`). */
    const char *src = "const zova = @import(\"zova\");\n"
                      "\n"
                      "pub fn cli(db: *zova.Database) !void {\n"
                      "    var st = try db.prepare();\n"
                      "    defer st.deinit();\n"
                      "    _ = st.step();\n"
                      "    _ = zova.probe();\n"
                      "    var raw: zova.sqlite.Statement = undefined;\n"
                      "    raw.reset();\n"
                      "}\n";
    CBMLSPDef defs[] = {
        {.qualified_name = "test.src.cli.cli", .short_name = "cli", .label = "Function",
         .def_module_qn = "test.src.cli", .return_types = "!void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.root.Database", .short_name = "Database", .label = "Variable",
         .def_module_qn = "test.src.root", .return_types = "test.src.db.Database",
         .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.root.sqlite", .short_name = "sqlite", .label = "Variable",
         .def_module_qn = "test.src.root", .return_types = "test.src.sqlite", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.root.probe", .short_name = "probe", .label = "Variable",
         .def_module_qn = "test.src.root", .return_types = "test.src.db.probe",
         .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.sqlite", .short_name = "sqlite", .label = "Variable",
         .def_module_qn = "test.src.db", .return_types = "test.src.sqlite", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.probe", .short_name = "probe", .label = "Function",
         .def_module_qn = "test.src.db", .return_types = "bool", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Database", .short_name = "Database", .label = "Struct",
         .def_module_qn = "test.src.db", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.db.Database.prepare", .short_name = "prepare",
         .label = "Method", .receiver_type = "test.src.db.Database", .def_module_qn = "test.src.db",
         .return_types = "!sqlite.Statement", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.sqlite.Statement", .short_name = "Statement", .label = "Struct",
         .def_module_qn = "test.src.sqlite", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.sqlite.Statement.step", .short_name = "step", .label = "Method",
         .receiver_type = "test.src.sqlite.Statement", .def_module_qn = "test.src.sqlite",
         .return_types = "bool", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.sqlite.Statement.deinit", .short_name = "deinit",
         .label = "Method", .receiver_type = "test.src.sqlite.Statement",
         .def_module_qn = "test.src.sqlite", .return_types = "void", .lang = CBM_LANG_ZIG},
        {.qualified_name = "test.src.sqlite.Statement.reset", .short_name = "reset",
         .label = "Method", .receiver_type = "test.src.sqlite.Statement",
         .def_module_qn = "test.src.sqlite", .return_types = "void", .lang = CBM_LANG_ZIG},
    };
    const char *imp_names[] = {"zova"};
    const char *imp_qns[] = {"test.src.root"};
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));
    cbm_run_zig_lsp_cross(&arena, src, (int)strlen(src), "test.src.cli", defs,
                          (int)(sizeof(defs) / sizeof(defs[0])), imp_names, imp_qns, 1, NULL, &out);
    const char *cli = "test.src.cli.cli";
    ASSERT_EQ(require_rows(&out, cli, "test.src.db.Database.prepare", 1), 1);
    ASSERT_EQ(require_rows(&out, cli, "test.src.sqlite.Statement.deinit", 1), 1);
    ASSERT_EQ(require_rows(&out, cli, "test.src.sqlite.Statement.step", 1), 1);
    ASSERT_EQ(require_rows(&out, cli, "test.src.db.probe", 1), 1);
    ASSERT_EQ(require_rows(&out, cli, "test.src.sqlite.Statement.reset", 1), 1);
    cbm_arena_destroy(&arena);
    PASS();
}

TEST(zig_lsp_cross_file_unmapped_import_fails_closed) {
    const char *src = "const zova = @import(\"zova\");\n"
                      "\n"
                      "pub fn run() void {\n"
                      "    var db = zova.Database.open();\n"
                      "    db.deinit();\n"
                      "}\n";
    CBMLSPDef defs[] = {
        {.qualified_name = "test.src.db.Database.deinit", .short_name = "deinit", .label = "Method",
         .receiver_type = "test.src.db.Database", .def_module_qn = "test.src.db",
         .return_types = "void", .lang = CBM_LANG_ZIG},
    };
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));
    cbm_run_zig_lsp_cross(&arena, src, (int)strlen(src), "test.src.main", defs, 1, NULL, NULL, 0,
                          NULL, &out);
    ASSERT_EQ(out.count, 0);
    cbm_arena_destroy(&arena);
    PASS();
}

SUITE(zig_lsp) {
    RUN_TEST(zig_lsp_methods_through_typed_locals);
    RUN_TEST(zig_lsp_rows_carry_their_call_site);
    RUN_TEST(zig_lsp_unknown_receivers_fail_closed);
    RUN_TEST(zig_lsp_nested_containers_and_qualified_calls);
    RUN_TEST(zig_lsp_cross_file_imports_returns_and_fields);
    RUN_TEST(zig_lsp_cross_file_follows_aliases_and_reexports);
    RUN_TEST(zig_lsp_cross_file_unmapped_import_fails_closed);
    RUN_TEST(zig_lsp_std_containers_carry_element_types);
    RUN_TEST(zig_lsp_cross_file_std_field_and_return_types);
    RUN_TEST(zig_lsp_generic_type_functions_bind_their_parameters);
    RUN_TEST(zig_lsp_cross_file_generic_type_functions);
    RUN_TEST(zig_lsp_param_kinds_mark_type_and_anytype_parameters);
    RUN_TEST(zig_lsp_cross_file_anytype_collects_argument_types);
    RUN_TEST(zig_lsp_cross_file_anytype_walks_bound_parameters);
}
