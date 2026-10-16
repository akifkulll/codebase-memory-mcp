/* zig_lsp.h — Type-aware call resolution for Zig source files.
 *
 * Zig has no inheritance, no overloading and no implicit conversions that change
 * which declaration a member call names, so a call `x.m()` resolves to exactly
 * one function once the container type of `x` is known. The resolver infers
 * that type from what Zig makes explicit: declaration and parameter type
 * annotations, `T{...}` initializers, `@This()` / `@import(...)` / `@as(T, ..)`,
 * the declared return type of a resolved callee, struct field types, and the
 * transparent wrappers (`try`, `catch`, `orelse`, `.?`, `.*`, `&x`, indexing and
 * loop/if/while captures). Declarations are looked up lexically through the
 * enclosing containers, so aliases such as `const Self = @This();` and
 * `const Db = @import("db.zig").Database;` resolve the same way the compiler
 * does.
 *
 * std is an opaque namespace (no graph nodes) whose generic containers carry
 * element types (ArrayList, the HashMap family, Allocator.create, ...). A type
 * function (`fn List(comptime T: type) type`) names the struct it returns, and
 * `List(Row)` binds `T` while its members are evaluated. An `anytype`
 * parameter takes the types the project passes to it (cbm_run_zig_lsp_cross_ex).
 * A receiver whose type is still not proven emits nothing: the extractor flags
 * such calls is_method and the pipeline suppresses the weak short-name
 * fallback for them, so an unknown receiver yields no edge rather than a
 * guessed one.
 *
 * Every resolved row carries its call site, so two same-named calls in one
 * function (`db.deinit(); st.deinit();`) each join their own target.
 */
#ifndef CBM_LSP_ZIG_LSP_H
#define CBM_LSP_ZIG_LSP_H

#include "go_lsp.h" /* CBMLSPDef */
#include "../cbm.h"

/* Single-file resolution, invoked from cbm_extract_file() after definitions and
 * calls are extracted. Resolves against the file's own definitions. */
void cbm_run_zig_lsp(CBMArena *arena, CBMFileResult *result, const char *source, int source_len,
                     TSNode root);

/* Cross-file resolution: defs holds the file's own and its imported modules'
 * definitions; import_names/import_qns map each import's local name (see
 * cbm_zig_import_local_name) to the imported module QN. Results are appended to
 * out (arena-allocated). */
void cbm_run_zig_lsp_cross(CBMArena *arena, const char *source, int source_len,
                           const char *module_qn, CBMLSPDef *defs, int def_count,
                           const char **import_names, const char **import_qns, int import_count,
                           TSTree *cached_tree, CBMResolvedCallArray *out);

/* `anytype` parameters: the type an argument passed to one, at a resolved call
 * anywhere in the project. callee_qn's parameter `param` (0-based, a method's
 * receiver included) received a value of type `type` (a type string: `[]` per
 * slice layer, then a QN or generic instance). */
typedef struct {
    const char *callee_qn;
    int param;
    const char *type;
} CBMZigArgType;

typedef struct {
    CBMZigArgType *items; /* malloc'd; the caller frees it */
    int count;
    int cap;
    CBMArena *arena; /* owns the entries' strings */
} CBMZigArgTypeArray;

/* Per-parameter kinds of a Zig parameter list's text (`(comptime T: type, x:
 * anytype, n: usize)`): ","-joined, the parameter's name for a `type`
 * parameter, "*" for an `anytype` one, "_" otherwise ("T,*,_"). NULL for NULL. */
const char *cbm_zig_param_kinds(CBMArena *arena, const char *signature);

/* cbm_run_zig_lsp_cross, plus: when collect is non-NULL, the argument types of
 * resolved calls into `anytype` parameters are appended to it (in arena); when
 * args (sorted by callee_qn, then param, then type) holds entries for one of
 * this file's functions, that function is walked again once per entry with the
 * parameter bound to the type, and calls it resolves only then are emitted with
 * strategy "lsp_zig_anytype" (one row per call site: the first type wins). */
void cbm_run_zig_lsp_cross_ex(CBMArena *arena, const char *source, int source_len,
                              const char *module_qn, CBMLSPDef *defs, int def_count,
                              const char **import_names, const char **import_qns,
                              int import_count, TSTree *cached_tree, CBMResolvedCallArray *out,
                              const CBMZigArgType *args, int arg_count,
                              CBMZigArgTypeArray *collect);

#endif /* CBM_LSP_ZIG_LSP_H */
