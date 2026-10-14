/*
 * cbm_zova_coordination.h — Indexing coordination around one shared cbm.zova
 * (ADR-0001).
 *
 * Workspace lease: one exclusive hold per workspace for a whole index run, so
 * two processes (or two threads) never index the same workspace at once.
 *
 * Preparation barrier: index runs hold it shared for their whole run; restore,
 * import and repack hold it exclusive while they replace the database file, so
 * no run publishes state prepared against the file that was replaced. A
 * waiting replacement has preference: a turnstile lock keeps new index runs
 * from starting while it waits.
 *
 * Lock order: barrier, then lease, then the writer gate (cbm_zova_writer_gate.h).
 * Both locks are file locks next to cbm.zova that the OS releases when the
 * holder dies. Lock files are created on demand and never removed.
 *
 * A supervised index worker never acquires either: the parent process that
 * received the request holds them for the worker's whole life.
 */
#ifndef CBM_ZOVA_COORDINATION_H
#define CBM_ZOVA_COORDINATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "foundation/file_lock.h"

typedef enum {
    CBM_ZOVA_COORD_OK = 0,
    CBM_ZOVA_COORD_BUSY = 1,      /* held elsewhere and the wait ran out */
    CBM_ZOVA_COORD_CANCELLED = 2, /* the process is shutting down */
    CBM_ZOVA_COORD_ERROR = -1,    /* lock file unusable; never proceed uncoordinated */
} cbm_zova_coord_status_t;

typedef struct {
    char path[4096];
    bool held;
} cbm_zova_coord_hold_t;

/* Wait budget for explicit requests: CBM_INDEX_LEASE_TIMEOUT_S seconds, default
 * 300. A wait of 0 tries once. */
int64_t cbm_zova_coord_default_wait_ms(void);

cbm_zova_coord_status_t cbm_zova_workspace_lease_acquire(const char *zova_path,
                                                         const char *workspace_id,
                                                         int64_t wait_ms,
                                                         cbm_zova_coord_hold_t *out);

cbm_zova_coord_status_t cbm_zova_prepare_barrier_acquire(const char *zova_path, bool exclusive,
                                                         int64_t wait_ms,
                                                         cbm_zova_coord_hold_t *out);

/* Release a lease or barrier hold. NULL-safe and idempotent. */
void cbm_zova_coord_release(cbm_zova_coord_hold_t *hold);

/* Index-run hold: barrier shared, then the lease for root_path's workspace in
 * the user cache database. Both share one wait budget. On BUSY or ERROR nothing
 * is held. out_workspace_id (optional, >= 64 bytes) receives the workspace id. */
typedef struct {
    cbm_zova_coord_hold_t barrier;
    cbm_zova_coord_hold_t lease;
    bool barrier_busy; /* BUSY came from the barrier (a database replacement) */
} cbm_zova_index_hold_t;

cbm_zova_coord_status_t cbm_zova_index_hold_acquire(const char *root_path, int64_t wait_ms,
                                                    cbm_zova_index_hold_t *out,
                                                    char *out_workspace_id,
                                                    size_t out_workspace_id_size);
void cbm_zova_index_hold_release(cbm_zova_index_hold_t *hold);

/* Make pending and future waits return CANCELLED (async-signal-safe), or clear
 * that state again. */
void cbm_zova_coord_set_cancelled(bool cancelled);

#endif /* CBM_ZOVA_COORDINATION_H */
