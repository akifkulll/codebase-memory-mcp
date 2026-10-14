/*
 * compat_thread.h — Portable threading: pthreads on POSIX, Win32 threads on Windows.
 *
 * Provides: thread create/join, mutex, aligned allocation.
 * All have zero overhead on POSIX (thin inlines or macros).
 */
#ifndef CBM_COMPAT_THREAD_H
#define CBM_COMPAT_THREAD_H

#include <stddef.h>

/* ── Thread ───────────────────────────────────────────────────── */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct {
    HANDLE handle;
} cbm_thread_t;

#else /* POSIX */

#include <pthread.h>

typedef struct {
    pthread_t handle;
} cbm_thread_t;

#endif

/* Create a thread with the given stack size (0 = OS default).
 * fn receives arg. Returns 0 on success. */
int cbm_thread_create(cbm_thread_t *t, size_t stack_size, void *(*fn)(void *), void *arg);

/* Wait for thread to finish. Returns 0 on success. */
int cbm_thread_join(cbm_thread_t *t);

/* Detach thread so resources are freed on exit. Returns 0 on success. */
int cbm_thread_detach(cbm_thread_t *t);

/* ── Mutex ────────────────────────────────────────────────────── */

#ifdef _WIN32

typedef struct {
    CRITICAL_SECTION cs;
} cbm_mutex_t;

#else

typedef struct {
    pthread_mutex_t mtx;
} cbm_mutex_t;

#endif

void cbm_mutex_init(cbm_mutex_t *m);
void cbm_mutex_lock(cbm_mutex_t *m);
void cbm_mutex_unlock(cbm_mutex_t *m);
void cbm_mutex_destroy(cbm_mutex_t *m);

/* ── One-time initialization ──────────────────────────────────── */

#ifdef _WIN32
typedef INIT_ONCE cbm_once_t;
#define CBM_ONCE_INIT INIT_ONCE_STATIC_INIT
#else
typedef pthread_once_t cbm_once_t;
#define CBM_ONCE_INIT PTHREAD_ONCE_INIT
#endif

/* Run fn exactly once per `once` across all threads. Callers that arrive while
 * it runs wait for it to finish. */
void cbm_once(cbm_once_t *once, void (*fn)(void));

/* ── Aligned allocation ───────────────────────────────────────── */

/* Allocate size bytes aligned to alignment boundary.
 * Returns 0 on success, non-zero on failure. *ptr receives the allocation. */
int cbm_aligned_alloc(void **ptr, size_t alignment, size_t size);

/* Free memory from cbm_aligned_alloc. */
void cbm_aligned_free(void *ptr);

#endif /* CBM_COMPAT_THREAD_H */
