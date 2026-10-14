#include "zova/cbm_zova_coordination.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "zova/cbm_zova.h"
#include "zova/cbm_zova_route.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

enum { COORD_POLL_MS = 20, COORD_DEFAULT_WAIT_S = 300 };

/* In-process accounting. A file lock alone cannot exclude threads of one
 * process, and a second descriptor on the same file would drop the process's
 * lock when closed. So each lock file is held at most once per process, and
 * this table records who in the process holds it. */
typedef struct coord_entry {
    struct coord_entry *next;
    char path[4096];
    int shared; /* in-process shared holders */
    bool exclusive;
    cbm_file_lock_t lock;
} coord_entry_t;

static coord_entry_t *coord_entries;
static cbm_mutex_t coord_mutex;
static atomic_bool coord_cancelled;

#ifdef _WIN32
static INIT_ONCE coord_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK coord_init(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    cbm_mutex_init(&coord_mutex);
    return TRUE;
}
static void coord_ensure_initialized(void) {
    InitOnceExecuteOnce(&coord_once, coord_init, NULL, NULL);
}
#else
static pthread_once_t coord_once = PTHREAD_ONCE_INIT;
static void coord_init(void) {
    cbm_mutex_init(&coord_mutex);
}
static void coord_ensure_initialized(void) {
    (void)pthread_once(&coord_once, coord_init);
}
#endif

static coord_entry_t **coord_find(const char *path) {
    for (coord_entry_t **e = &coord_entries; *e; e = &(*e)->next) {
        if (strcmp((*e)->path, path) == 0) return e;
    }
    return NULL;
}

/* The cache directory may not exist before the first index run. */
static bool coord_ensure_parent_dir(const char *path) {
    char dir[4096];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) return false;
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *backslash = strrchr(dir, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    if (!slash || slash == dir) return true;
    *slash = '\0';
    return cbm_mkdir_p(dir, 0755);
}

/* One non-blocking attempt, in-process and cross-process. */
static cbm_zova_coord_status_t coord_try(const char *path, bool exclusive) {
    cbm_mutex_lock(&coord_mutex);
    coord_entry_t **found = coord_find(path);
    if (found) {
        coord_entry_t *e = *found;
        cbm_zova_coord_status_t status = CBM_ZOVA_COORD_BUSY;
        if (!exclusive && !e->exclusive) {
            e->shared++;
            status = CBM_ZOVA_COORD_OK;
        }
        cbm_mutex_unlock(&coord_mutex);
        return status;
    }
    coord_entry_t *e = calloc(1, sizeof(*e));
    if (!e) {
        cbm_mutex_unlock(&coord_mutex);
        return CBM_ZOVA_COORD_ERROR;
    }
    snprintf(e->path, sizeof(e->path), "%s", path);
    int rc = cbm_file_lock_acquire(
        path, exclusive ? CBM_FILE_LOCK_EXCLUSIVE : CBM_FILE_LOCK_SHARED, false, &e->lock);
    if (rc != 0) {
        free(e);
        cbm_mutex_unlock(&coord_mutex);
        return rc == CBM_FILE_LOCK_BUSY ? CBM_ZOVA_COORD_BUSY : CBM_ZOVA_COORD_ERROR;
    }
    e->exclusive = exclusive;
    e->shared = exclusive ? 0 : 1;
    e->next = coord_entries;
    coord_entries = e;
    cbm_mutex_unlock(&coord_mutex);
    return CBM_ZOVA_COORD_OK;
}

static int64_t coord_now_ms(void) {
    struct timespec ts;
    cbm_clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Poll instead of blocking in the kernel so a wait stays bounded and returns
 * promptly on shutdown. */
static cbm_zova_coord_status_t coord_acquire(const char *path, bool exclusive, int64_t wait_ms,
                                             int64_t deadline_ms, cbm_zova_coord_hold_t *out) {
    out->held = false;
    if (snprintf(out->path, sizeof(out->path), "%s", path) >= (int)sizeof(out->path) ||
        !coord_ensure_parent_dir(path))
        return CBM_ZOVA_COORD_ERROR;
    coord_ensure_initialized();
    for (;;) {
        if (atomic_load(&coord_cancelled)) return CBM_ZOVA_COORD_CANCELLED;
        cbm_zova_coord_status_t status = coord_try(path, exclusive);
        if (status == CBM_ZOVA_COORD_OK) {
            out->held = true;
            return status;
        }
        if (status != CBM_ZOVA_COORD_BUSY || wait_ms <= 0 || coord_now_ms() >= deadline_ms)
            return status;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = COORD_POLL_MS * 1000000L};
        cbm_nanosleep(&pause, NULL);
    }
}

int64_t cbm_zova_coord_default_wait_ms(void) {
    const char *raw = getenv("CBM_INDEX_LEASE_TIMEOUT_S");
    if (raw && raw[0]) {
        char *end = NULL;
        long long seconds = strtoll(raw, &end, 10);
        if (end && *end == '\0' && seconds >= 0) return (int64_t)seconds * 1000;
    }
    return (int64_t)COORD_DEFAULT_WAIT_S * 1000;
}

static int coord_lease_path(const char *zova_path, const char *workspace_id, char *out,
                            size_t out_size) {
    if (!zova_path || !zova_path[0] || cbm_zova_workspace_id_validate(workspace_id) != 0)
        return -1;
    int n = snprintf(out, out_size, "%s.ws-%s.lock", zova_path, workspace_id);
    return n > 0 && (size_t)n < out_size ? 0 : -1;
}

static int coord_barrier_path(const char *zova_path, char *out, size_t out_size) {
    if (!zova_path || !zova_path[0]) return -1;
    int n = snprintf(out, out_size, "%s.prepare.lock", zova_path);
    return n > 0 && (size_t)n < out_size ? 0 : -1;
}

cbm_zova_coord_status_t cbm_zova_workspace_lease_acquire(const char *zova_path,
                                                         const char *workspace_id,
                                                         int64_t wait_ms,
                                                         cbm_zova_coord_hold_t *out) {
    if (!out) return CBM_ZOVA_COORD_ERROR;
    out->held = false;
    char path[4096];
    if (coord_lease_path(zova_path, workspace_id, path, sizeof(path)) != 0)
        return CBM_ZOVA_COORD_ERROR;
    return coord_acquire(path, true, wait_ms, coord_now_ms() + wait_ms, out);
}

cbm_zova_coord_status_t cbm_zova_prepare_barrier_acquire(const char *zova_path, bool exclusive,
                                                         int64_t wait_ms,
                                                         cbm_zova_coord_hold_t *out) {
    if (!out) return CBM_ZOVA_COORD_ERROR;
    out->held = false;
    char path[4096];
    if (coord_barrier_path(zova_path, path, sizeof(path)) != 0) return CBM_ZOVA_COORD_ERROR;
    return coord_acquire(path, exclusive, wait_ms, coord_now_ms() + wait_ms, out);
}

void cbm_zova_coord_release(cbm_zova_coord_hold_t *hold) {
    if (!hold || !hold->held) return;
    hold->held = false;
    coord_ensure_initialized();
    cbm_mutex_lock(&coord_mutex);
    coord_entry_t **found = coord_find(hold->path);
    if (found) {
        coord_entry_t *e = *found;
        if (e->exclusive || --e->shared <= 0) {
            cbm_file_lock_release(&e->lock);
            *found = e->next;
            free(e);
        }
    }
    cbm_mutex_unlock(&coord_mutex);
}

cbm_zova_coord_status_t cbm_zova_index_hold_acquire(const char *root_path, int64_t wait_ms,
                                                    cbm_zova_index_hold_t *out,
                                                    char *out_workspace_id,
                                                    size_t out_workspace_id_size) {
    if (!out) return CBM_ZOVA_COORD_ERROR;
    memset(out, 0, sizeof(*out));
    char zova_path[4096];
    char workspace_id[CBM_ZOVA_WORKSPACE_ID_MAX];
    if (cbm_zova_user_database_path(zova_path, sizeof(zova_path)) != 0 ||
        cbm_zova_workspace_id_for_root(root_path, workspace_id, sizeof(workspace_id)) != 0)
        return CBM_ZOVA_COORD_ERROR;
    if (out_workspace_id && out_workspace_id_size > 0)
        snprintf(out_workspace_id, out_workspace_id_size, "%s", workspace_id);
    int64_t deadline_ms = coord_now_ms() + wait_ms;
    char path[4096];
    if (coord_barrier_path(zova_path, path, sizeof(path)) != 0) return CBM_ZOVA_COORD_ERROR;
    cbm_zova_coord_status_t status = coord_acquire(path, false, wait_ms, deadline_ms, &out->barrier);
    if (status != CBM_ZOVA_COORD_OK) {
        out->barrier_busy = status == CBM_ZOVA_COORD_BUSY;
        return status;
    }
    if (coord_lease_path(zova_path, workspace_id, path, sizeof(path)) != 0) {
        cbm_zova_coord_release(&out->barrier);
        return CBM_ZOVA_COORD_ERROR;
    }
    status = coord_acquire(path, true, wait_ms, deadline_ms, &out->lease);
    if (status != CBM_ZOVA_COORD_OK) cbm_zova_coord_release(&out->barrier);
    return status;
}

void cbm_zova_index_hold_release(cbm_zova_index_hold_t *hold) {
    if (!hold) return;
    cbm_zova_coord_release(&hold->lease);
    cbm_zova_coord_release(&hold->barrier);
}

void cbm_zova_coord_set_cancelled(bool cancelled) {
    atomic_store(&coord_cancelled, cancelled);
}
