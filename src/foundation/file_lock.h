/*
 * file_lock.h — Cross-process advisory lock on a lock file.
 *
 * fcntl record locks on POSIX, LockFileEx on Windows. The OS releases the lock
 * when the holder dies, so a crashed process never leaves it stuck.
 *
 * POSIX record locks belong to the process, not the descriptor: two threads of
 * one process never conflict, and closing any descriptor of the file drops the
 * process's lock. Callers that need exclusion between threads, or more than
 * one hold of the same file, must add their own in-process accounting.
 */
#ifndef CBM_FILE_LOCK_H
#define CBM_FILE_LOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CBM_FILE_LOCK_SHARED = 0,
    CBM_FILE_LOCK_EXCLUSIVE = 1,
} cbm_file_lock_mode_t;

enum { CBM_FILE_LOCK_BUSY = 1 };

typedef struct {
    intptr_t handle; /* -1 when not held */
} cbm_file_lock_t;

/* Lock `path`, creating the file (mode 0600) when missing. With wait=false a
 * lock held by another process returns CBM_FILE_LOCK_BUSY at once; with
 * wait=true the call blocks. Returns 0 when held, -1 when the file cannot be
 * opened or locked. */
int cbm_file_lock_acquire(const char *path, cbm_file_lock_mode_t mode, bool wait,
                          cbm_file_lock_t *out);

/* Release and close. NULL-safe and idempotent. */
void cbm_file_lock_release(cbm_file_lock_t *lock);

#endif /* CBM_FILE_LOCK_H */
