#include "zova/cbm_zova_writer_gate.h"

#include "foundation/constants.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/file_lock.h"
#include "foundation/platform.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static cbm_mutex_t gate_mutex;
static _Atomic uint64_t gate_waiters;

#ifdef _WIN32
static INIT_ONCE gate_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_gate(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    cbm_mutex_init(&gate_mutex);
    return TRUE;
}
static void ensure_gate_initialized(void) {
    InitOnceExecuteOnce(&gate_once, init_gate, NULL, NULL);
}
#else
static pthread_once_t gate_once = PTHREAD_ONCE_INIT;
static void init_gate(void) {
    cbm_mutex_init(&gate_mutex);
}
static void ensure_gate_initialized(void) {
    (void)pthread_once(&gate_once, init_gate);
}
#endif

static int acquire_process_lock(const char *zova_path,
                                cbm_zova_writer_guard_t *guard,
                                bool wait) {
    int n = snprintf(guard->lock_path, sizeof(guard->lock_path),
                     "%s.writer.lock", zova_path);
    if (n <= 0 || (size_t)n >= sizeof(guard->lock_path)) return -1;
    cbm_file_lock_t lock;
    if (cbm_file_lock_acquire(guard->lock_path, CBM_FILE_LOCK_EXCLUSIVE, wait, &lock) != 0)
        return -1;
    guard->handle = lock.handle;
    return 0;
}

int cbm_zova_writer_guard_acquire(const char *zova_path,
                                  cbm_zova_writer_guard_t *guard) {
    if (!zova_path || !zova_path[0] || !guard) return -1;
    memset(guard, 0, sizeof(*guard));
    guard->handle = -1;
    ensure_gate_initialized();
    atomic_fetch_add_explicit(&gate_waiters, 1, memory_order_relaxed);
    cbm_mutex_lock(&gate_mutex);
    atomic_fetch_sub_explicit(&gate_waiters, 1, memory_order_relaxed);
    guard->mutex_held = true;
    if (acquire_process_lock(zova_path, guard, true) != 0) {
        guard->mutex_held = false;
        cbm_mutex_unlock(&gate_mutex);
        return -1;
    }
    return 0;
}

int cbm_zova_writer_guard_try_acquire(const char *zova_path,
                                      cbm_zova_writer_guard_t *guard) {
    if (!zova_path || !zova_path[0] || !guard) return -1;
    memset(guard, 0, sizeof(*guard));
    guard->handle = -1;
    ensure_gate_initialized();
#ifdef _WIN32
    if (!TryEnterCriticalSection(&gate_mutex.cs)) return -1;
#else
    if (pthread_mutex_trylock(&gate_mutex.mtx) != 0) return -1;
#endif
    guard->mutex_held = true;
    if (acquire_process_lock(zova_path, guard, false) != 0) {
        guard->mutex_held = false;
        cbm_mutex_unlock(&gate_mutex);
        return -1;
    }
    return 0;
}

void cbm_zova_writer_guard_release(cbm_zova_writer_guard_t *guard) {
    if (!guard) return;
    cbm_file_lock_t lock = {.handle = guard->handle};
    cbm_file_lock_release(&lock);
    guard->handle = -1;
    if (guard->mutex_held) {
        guard->mutex_held = false;
        cbm_mutex_unlock(&gate_mutex);
    }
}

uint64_t cbm_zova_writer_gate_waiter_count(void) {
    return atomic_load_explicit(&gate_waiters, memory_order_relaxed);
}
