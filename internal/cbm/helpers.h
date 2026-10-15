#ifndef CBM_HELPERS_H
#define CBM_HELPERS_H

#include "cbm.h"

// Portable memmem: find first occurrence of `needle` (needle_len bytes) within
// `haystack` (haystack_len bytes). Returns a pointer into haystack, or NULL.
// Hand-rolled so it compiles identically on all platforms (GNU/BSD-only
// memmem is unavailable under msys2-clang on Windows).
void *cbm_memmem(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len);

// Extract text of a node from source. Returns arena-allocated string.
char *cbm_node_text(CBMArena *a, TSNode node, const char *source);

// Check if a string is a language keyword (should be skipped as callee/usage).
bool cbm_is_keyword(const char *name, CBMLanguage lang);

// Check if a name is a builtin we mint a real graph node for, so a CALL to it
// must NOT be keyword-filtered out of call extraction (the LSP resolves it to
// the injected builtin node and forms a CALLS edge). Narrower than
// cbm_is_keyword: it only covers builtins with a target node, so un-filtering
// them cannot produce a node-less / Module-sourced edge. The Python set MUST
// stay in sync with kPyBuiltinNodes in internal/cbm/lsp/py_builtins.c.
bool cbm_is_resolvable_builtin(const char *name, CBMLanguage lang);

// Classify a string literal as URL, config, or neither.
// Returns CBM_STRREF_URL (0), CBM_STRREF_CONFIG (1), or -1 for neither.
int cbm_classify_string(const char *str, int len);

// Check if a name is exported per language convention.
bool cbm_is_exported(const char *name, CBMLanguage lang);

// Check if a file is a test file based on path and language.
bool cbm_is_test_file(const char *rel_path, CBMLanguage lang);

// Find the innermost enclosing function node by walking parent chain.
// Returns a null node if none found.
TSNode cbm_find_enclosing_func(TSNode node, CBMLanguage lang);

// Get the QN of an enclosing function, or module_qn if none.
const char *cbm_enclosing_func_qn(CBMArena *a, TSNode node, CBMLanguage lang, const char *source,
                                  const char *project, const char *rel_path, const char *module_qn);

// Cached version: uses ctx->ef_cache to avoid repeated parent-chain walks.
const char *cbm_enclosing_func_qn_cached(CBMExtractCtx *ctx, TSNode node);

// Max declarator-chain descent depth for C/C++/CUDA/GLSL function-name
// resolution. Single source of truth — extract_defs.c's DECLARATOR_DEPTH_LIMIT
// is derived from this so the three extractors cannot drift.
#define CBM_DECLARATOR_DEPTH_LIMIT 8

// Resolve the function-name node for a C/C++/CUDA/GLSL `function_definition`.
// Such nodes have no `name` field — the name is nested in the declarator chain
// (pointer/function/parenthesized/array declarators wrap it; out-of-line method
// definitions name it with a qualified_identifier). Descends the `declarator`
// field to the innermost name node and returns it, or a null node if none is
// found. Shared by the defs, calls, and unified extractors so all three agree on
// enclosing-function attribution — drift between private copies caused #438.
TSNode cbm_resolve_c_declarator_name_node(TSNode func_node);

// tree-sitter error recovery around an unknown leading macro: `API RetT name(...)`
// (`XXH_PUBLIC_API XXH64_hash_t XXH64(...)`, `JEMALLOC_ALWAYS_INLINE T *f(...)`)
// carries one identifier more than the declaration grammar admits, so the parser
// recovers in one of two shapes, and neither names the function:
//   - C++ inserts a zero-width MISSING "::" and reads `RetT name` as the
//     out-of-line name `RetT::name`; with a pointer return, the name side is a
//     pointer_type_declarator that wraps the function_declarator;
//   - C and C++ keep RetT as the declarator identifier and put the real name in
//     an ERROR node directly before the parameter list.
// cbm_c_qualifier_is_recovered: true when `qid` (a qualified_identifier or
// scoped_identifier) is the first shape, i.e. its "::" token is MISSING.
// cbm_c_recovered_func_name: the real name node of the second shape for a
// function_declarator, or a null node when the declarator is not that shape.
// Shared by every C-family declarator walk (defs, call scope, params, C LSP) so
// the def QN and every caller QN agree on the recovered name.
bool cbm_c_qualifier_is_recovered(TSNode qid);
TSNode cbm_c_recovered_func_name(TSNode func_declarator);

// True for a C keyword that error recovery can surface as a function "name"
// (`else if (a) (b) {` split by #ifdef reads as a definition named `if`).
// cbm_func_name_node_text returns NULL for these in the C-declarator grammars,
// so neither a def nor a call scope is minted; the C LSP applies the same test.
bool cbm_c_reserved_func_name(const char *name);

// True for the languages whose `#define`s become Macro nodes: C, C++, CUDA,
// GLSL, Objective-C and ISPC. The one predicate for what follows from the C
// preprocessor: the `#macro` QN namespace (CBM_MACRO_QN_SUFFIX) and the
// `variants` property (CBMDefinition.variants), because #if branches are what
// lets one of these files define a name more than once.
bool cbm_is_c_preprocessor_lang(CBMLanguage lang);

// Convert a resolved function/method name node to its name string, normalizing a
// C++ conversion-operator's `operator_cast` node (which spans the full
// "operator bool() const") down to "operator bool". Shared by the defs and
// unified extractors so the def name and call-scope QN agree.
char *cbm_func_name_node_text(CBMArena *a, TSNode name_node, const char *source, CBMLanguage lang);

// Resolve a function/method definition node's NAME node across all ~130 grammars
// (generic `name` field, arrow→declarator, C/C++ declarator chain, plus the many
// per-language quirks: Fortran subroutine, SCSS mixin, SQL create_function, R,
// PowerShell, Ada, the Lisp/FP family, etc.). Defined in extract_defs.c. Shared by
// the defs, calls, and unified extractors so all three agree on enclosing-function
// naming — drift between private copies caused the Module-mis-attribution of
// gap #3 (and #438 for the C-declarator case).
TSNode cbm_resolve_func_name(TSNode node, CBMLanguage lang);

// C++/CUDA out-of-line method definition (`void Foo::bar() {...}`): return the
// immediate enclosing class name ("Foo") from the qualified declarator, or NULL
// for a plain free function. Defined in extract_defs.c. Shared so the unified
// (call-scope) extractor computes the SAME class-qualified enclosing QN as the
// def extractor — drift dropped the class qualifier from in-body calls (#554/#621).
char *cbm_cpp_out_of_line_parent_class(CBMArena *a, TSNode node, const char *source);

// Find a child node by kind string.
TSNode cbm_find_child_by_kind(TSNode parent, const char *kind);

// Find every child node matching `kind` (unlike cbm_find_child_by_kind,
// which stops at the first match). Writes up to `max` nodes into `out`;
// returns how many were found. Repeated sibling nodes of the same kind are
// common in some grammars (e.g. C#/PHP stack each `[Attr]` as its own
// attribute_list child) — see #1692.
int cbm_find_children_by_kind(TSNode parent, const char *kind, TSNode *out, int max);

// Check if node kind matches a set of types (NULL-terminated array of strings).
bool cbm_kind_in_set(TSNode node, const char **types);

// PHP first-class callable syntax (`strlen(...)`, `$obj->m(...)`, `Foo::m(...)`):
// a call node whose argument list is the `...` placeholder. It creates a
// Closure for the target; it does not invoke it.
bool cbm_php_is_first_class_callable(TSNode node);

// Free the calling thread's cbm_kind_in_set bitset cache (call at thread/process
// teardown so the thread-local cache is not reported as a leak).
void cbm_kind_in_set_free_cache(void);

// Check if node has an ancestor of the given kind, within max_depth levels.
bool cbm_has_ancestor_kind(TSNode node, const char *kind, int max_depth);

// Count nodes of given kinds in subtree (for complexity metric).
int cbm_count_branching(TSNode node, const char **branching_types);

// Per-function structural complexity, computed in a single AST walk.
typedef struct {
    int cyclomatic;       // branching-node count (matches def.complexity)
    int cognitive;        // nesting-weighted flow-break count (Campbell-style approximation)
    int loop_count;       // total loop constructs in the body
    int loop_depth;       // maximum nested-loop depth — structural bottleneck proxy
    int max_access_depth; // deepest chained member/subscript access (a.b.c.d → 4) — structure smell
} cbm_complexity_t;

// Compute the metrics above in one traversal of `node`'s subtree.
// `branching_types` is the language's branching node-type set.
void cbm_compute_complexity(TSNode node, const char **branching_types, cbm_complexity_t *out);

// Is `kind` a loop construct node type? Language-agnostic curated set (for/while/
// do/foreach/repeat/loop variants). Exposed so the unified walk can track loop
// nesting at call sites without re-deriving the set.
bool cbm_is_loop_node_type(const char *kind);

// Is this a module-level node? (not nested inside function/class body)
bool cbm_is_module_level(TSNode node, CBMLanguage lang);

// Same check, but the node's PARENT is supplied directly — avoids the
// O(n) ts_node_parent rescan. Use at call sites iterating a known
// parent's children (the common case). `parent` is the parent of the
// node being classified.
bool cbm_is_module_level_p(TSNode parent, CBMLanguage lang);

// --- FQN computation ---

// Compute qualified name: project.rel_path_parts.name
char *cbm_fqn_compute(CBMArena *a, const char *project, const char *rel_path, const char *name);

// Module QN (file without name): project.rel_path_parts
char *cbm_fqn_module(CBMArena *a, const char *project, const char *rel_path);

// Language-aware module QN. For directory-module languages (Java package, Go
// package) the module is derived from the CONTAINING DIRECTORY (the filename
// stem is NOT baked in): `Outer.java` at root -> "proj", `myapp/db/conn.go` ->
// "proj.myapp.db". For every OTHER language this returns exactly what
// cbm_fqn_module returns (no behavior change).
char *cbm_fqn_module_source_lang(CBMArena *a, const char *project, const char *rel_path,
                                 CBMLanguage lang);

// Language-aware symbol QN. For directory-module languages this is the
// directory-based module + "." + name (so a top-level class `Outer` in
// `Outer.java` is "proj.Outer", not "proj.Outer.Outer"). For every other
// language this is exactly cbm_fqn_compute (no behavior change).
char *cbm_fqn_compute_source_lang(CBMArena *a, const char *project, const char *rel_path,
                                  const char *name, CBMLanguage lang);

// Folder QN: project.dir_parts
char *cbm_fqn_folder(CBMArena *a, const char *project, const char *rel_dir);

/* Flatten a JS/TS `template_string` node into plain text: string fragments are
 * kept verbatim and each ${...} substitution becomes the "{}" placeholder, so
 * client-side URLs built from template literals share the canonical parameter
 * shape that server-side route paths already use. NULL when empty/oversized. */
const char *cbm_template_string_text(CBMArena *a, TSNode node, const char *source);

#endif // CBM_HELPERS_H
