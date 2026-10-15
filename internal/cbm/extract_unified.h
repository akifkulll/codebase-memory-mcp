#ifndef CBM_EXTRACT_UNIFIED_H
#define CBM_EXTRACT_UNIFIED_H

#include "cbm.h"
#include "lang_specs.h"
#include "extract_node_stack.h"

// Scope kinds for the walk state stack.
#define SCOPE_FUNC 1
#define SCOPE_CLASS 2
#define SCOPE_CALL 3
#define SCOPE_IMPORT 4
#define SCOPE_LOOP 5
#define SCOPE_BRANCH 6
#define SCOPE_PY_PARAMS 7 // Python def/lambda parameter frame; carries no walk flags

#define MAX_SCOPES 64
#define MAX_PY_PARAMS 256

// WalkState tracks scope context during the unified cursor walk.
// Replaces parent-chain walks for enclosing_func_qn, inside_call, etc.
typedef struct {
    const char *enclosing_func_qn;  // current function QN (module_qn at top level)
    const char *enclosing_class_qn; // current class QN (NULL outside class)
    bool inside_call;               // within a call_node_types subtree
    bool inside_import;             // within an import_node_types subtree
    int loop_depth;                 // count of enclosing loop scopes (for bottleneck metrics)
    int branch_depth;               // count of enclosing branch scopes

    struct {
        const char *qn;
        uint32_t depth;
        uint8_t kind;
        int py_param_base; // py_param_count on entry; popping unwinds back to it
    } scopes[MAX_SCOPES];
    int scope_top;

    /* Names bound as parameters by the Python defs/lambdas currently open on the
     * scope stack (upstream #1912). Bound when the frame is pushed and unwound
     * when it pops, so a bare-call guard never ascends the tree. Overflow only
     * loses names, so a lookup can miss (no suppression) but never invent one. */
    const char *py_params[MAX_PY_PARAMS];
    int py_param_count;

    /* Root-to-current path of the walk (the last item is the node being
     * handled), so a handler reads an ancestor in O(1). ts_node_parent() walks
     * down from the root, O(depth) per call, which is quadratic across deeply
     * nested code. path_complete is false only after an allocation failure. */
    TSNodeStack path;
    bool path_complete;
} WalkState;

// True when `name` is bound as a parameter of a Python def or lambda that
// encloses the current walk position.
bool cbm_walk_python_param_is_bound(const WalkState *state, const char *name);

// The ancestor `up` levels above the current walk node `node` (1 = parent), or
// a null node above the root.
TSNode cbm_walk_ancestor(const WalkState *state, TSNode node, uint32_t up);

// Per-node handler prototypes. Each is called once per node during the
// unified cursor walk, replacing the old recursive walk_* functions.
void handle_calls(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_usages(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_throws(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_readwrites(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_type_refs(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec, WalkState *state);
void handle_env_accesses(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec,
                         WalkState *state);
void handle_type_assigns(CBMExtractCtx *ctx, TSNode node, const CBMLangSpec *spec,
                         WalkState *state);

// Single-pass extraction using TSTreeCursor. Visits every node once,
// dispatching to all handlers per node. Replaces the 7 separate walk_*
// functions for calls/usages/throws/readwrites/type_refs/env_accesses/type_assigns.
// Definitions and imports stay as separate passes (different recursion patterns).
void cbm_extract_unified(CBMExtractCtx *ctx);

#endif // CBM_EXTRACT_UNIFIED_H
