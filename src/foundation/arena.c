/*
 * arena.c — Bump allocator implementation.
 *
 * Restructured from internal/cbm/arena.c with additions:
 *   - cbm_arena_init_sized() for custom block sizes
 *   - cbm_arena_calloc() for zero-initialized allocations
 *   - cbm_arena_reset() for reuse without full destroy
 *   - cbm_arena_total() for allocation tracking
 *
 * Arena blocks use malloc/free (= mimalloc in production builds).
 */
#include "arena.h"
#include "foundation/constants.h"

enum { ARENA_ALIGN = 7, ARENA_GROW_OK = 1 };
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <xlocale.h>
#endif
#include <stdio.h>

void cbm_arena_init(CBMArena *a) {
    cbm_arena_init_sized(a, CBM_ARENA_DEFAULT_BLOCK_SIZE);
}

void cbm_arena_init_sized(CBMArena *a, size_t block_size) {
    memset(a, 0, sizeof(*a));
    if (block_size < CBM_SZ_64) {
        block_size = CBM_SZ_64; /* minimum sanity */
    }
    a->block_size = block_size;
    a->blocks[0] = (char *)malloc(block_size);
    if (a->blocks[0]) {
        a->block_sizes[0] = block_size;
        a->nblocks = SKIP_ONE;
    }
}

static int arena_grow(CBMArena *a, size_t min_size) {
    if (a->nblocks >= CBM_ARENA_MAX_BLOCKS) {
        return 0;
    }
    size_t new_size = a->block_size * PAIR_LEN;
    if (new_size < min_size) {
        new_size = min_size;
    }
    char *block = (char *)malloc(new_size);
    if (!block) {
        return 0;
    }
    a->blocks[a->nblocks] = block;
    a->block_sizes[a->nblocks] = new_size;
    a->nblocks++;
    a->block_size = new_size;
    a->used = 0;
    return ARENA_GROW_OK;
}

void *cbm_arena_alloc(CBMArena *a, size_t n) {
    if (!a || n == 0) {
        return NULL;
    }
    /* 8-byte alignment */
    n = (n + ARENA_ALIGN) & ~(size_t)ARENA_ALIGN;
    if (a->nblocks == 0) {
        return NULL;
    }
    if (a->used + n > a->block_size) {
        if (!arena_grow(a, n)) {
            return NULL;
        }
    }
    char *ptr = a->blocks[a->nblocks - SKIP_ONE] + a->used;
    a->used += n;
    a->total_alloc += n;
    return ptr;
}

void *cbm_arena_calloc(CBMArena *a, size_t n) {
    void *p = cbm_arena_alloc(a, n);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

char *cbm_arena_strdup(CBMArena *a, const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *dst = (char *)cbm_arena_alloc(a, len + SKIP_ONE);
    if (dst) {
        memcpy(dst, s, len + SKIP_ONE);
    }
    return dst;
}

char *cbm_arena_strndup(CBMArena *a, const char *s, size_t len) {
    if (!s) {
        return NULL;
    }
    char *dst = (char *)cbm_arena_alloc(a, len + SKIP_ONE);
    if (dst) {
        memcpy(dst, s, len);
        dst[len] = '\0';
    }
    return dst;
}

/* On macOS every vsnprintf consults the process locale under an unfair lock
 * (localeconv_l inside __vfprintf), so parallel workers formatting type names
 * serialize on it (upstream 7b77fd48: 10 MB C# files took 68 s instead of under
 * a second). Each thread formats with a C locale object of its own, freed by a
 * pthread key destructor at thread exit, and the common short string is
 * formatted once into a stack buffer. */
#if defined(__APPLE__)
static _Thread_local locale_t tl_c_locale;
static pthread_key_t tl_c_locale_key;
static pthread_once_t tl_c_locale_once = PTHREAD_ONCE_INIT;
static void arena_c_locale_free(void *loc) {
    if (loc) {
        freelocale((locale_t)loc);
    }
}
static void arena_c_locale_key_init(void) {
    (void)pthread_key_create(&tl_c_locale_key, arena_c_locale_free);
}
static locale_t arena_c_locale(void) {
    if (!tl_c_locale) {
        tl_c_locale = newlocale(LC_ALL_MASK, "C", NULL);
        if (tl_c_locale) {
            pthread_once(&tl_c_locale_once, arena_c_locale_key_init);
            (void)pthread_setspecific(tl_c_locale_key, tl_c_locale);
        }
    }
    return tl_c_locale; /* NULL = the global locale */
}
#define ARENA_VSNPRINTF(buf, n, fmt, ap) vsnprintf_l((buf), (n), arena_c_locale(), (fmt), (ap))
#else
#define ARENA_VSNPRINTF(buf, n, fmt, ap) vsnprintf((buf), (n), (fmt), (ap))
#endif

enum { ARENA_SPRINTF_LOCAL = 512 };

char *cbm_arena_sprintf(CBMArena *a, const char *fmt, ...) {
    char local[ARENA_SPRINTF_LOCAL];
    va_list args;
    va_start(args, fmt);
    int needed = ARENA_VSNPRINTF(local, sizeof(local), fmt, args);
    va_end(args);
    if (needed < 0) {
        return NULL;
    }

    char *dst = (char *)cbm_arena_alloc(a, (size_t)needed + SKIP_ONE);
    if (!dst) {
        return NULL;
    }
    if ((size_t)needed < sizeof(local)) {
        memcpy(dst, local, (size_t)needed + SKIP_ONE);
        return dst;
    }

    va_start(args, fmt);
    ARENA_VSNPRINTF(dst, (size_t)needed + SKIP_ONE, fmt, args);
    va_end(args);
    return dst;
}

void cbm_arena_reset(CBMArena *a) {
    /* Keep first block, free the rest */
    for (int i = SKIP_ONE; i < a->nblocks; i++) {
        free(a->blocks[i]);
        a->blocks[i] = NULL;
        a->block_sizes[i] = 0;
    }
    if (a->nblocks > SKIP_ONE) {
        a->nblocks = SKIP_ONE;
    }
    a->used = 0;
    a->total_alloc = 0;
    /* Reset block_size to match surviving block — prevents overflow if
     * block_size grew during previous allocations (e.g., CBM_SZ_128 → CBM_SZ_256). */
    if (a->nblocks == SKIP_ONE) {
        a->block_size = a->block_sizes[0];
    }
}

void cbm_arena_destroy(CBMArena *a) {
    for (int i = 0; i < a->nblocks; i++) {
        free(a->blocks[i]);
    }
    memset(a, 0, sizeof(*a));
}

size_t cbm_arena_total(const CBMArena *a) {
    return a->total_alloc;
}
