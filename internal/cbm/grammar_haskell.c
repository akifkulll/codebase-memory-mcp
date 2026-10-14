// Vendored tree-sitter grammar: haskell
// Each grammar compiled as separate unit (conflicting static symbols).
// Preload the strict-aliasing-safe array helpers: the pinned Haskell scanner's
// own tree_sitter/array.h casts through (Array *), which optimized builds can
// miscompile across realloc (tree-sitter#5242). The shared include guard makes
// the scanner's copy a no-op.
#include "vendored/common/tree_sitter/array.h"
typedef void *(*cbm_haskell_array_grow_fn)(void *, uint32_t, uint32_t *, uint32_t, size_t);
_Static_assert(_Generic(&_array__grow, cbm_haskell_array_grow_fn: 1, default: 0),
               "Haskell scanner requires strict-aliasing-safe array helpers");
#include "vendored/grammars/haskell/parser.c"
#include "vendored/grammars/haskell/scanner.c"
