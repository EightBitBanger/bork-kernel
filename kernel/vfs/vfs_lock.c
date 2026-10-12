#ifndef _VFS_LOCK_H_
#define _VFS_LOCK_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/vfs/vfs_internal.h>

#include <kernel/mutex.h>
#include <kernel/scheduler/scheduler.h>

extern ThreadBlock* current_thread;

// Zero-initialized storage is a valid unlocked mutex (same state mutex_init
// produces), so the lock works from the very first VFS call, before or after
// scheduler_init().
static mutex_t vfs_mutex;

// Recursion bookkeeping. Only the owning thread writes these, and only while
// it holds vfs_mutex. It clears vfs_owner before releasing the mutex, so a
// thread that does not hold the lock can never read itself as the owner.
// vfs_depth is checked first so that, before the scheduler exists
// (current_thread == NULL == initial vfs_owner), an unheld lock is not
// mistaken for a recursive acquire.
static ThreadBlock* volatile vfs_owner = NULL;
static volatile uint32_t     vfs_depth = 0;

void vfs_lock(void) {
    if (vfs_depth != 0 && vfs_owner == current_thread) {
        vfs_depth++;
        return;
    }
    
    mutex_lock(&vfs_mutex);
    vfs_owner = current_thread;
    vfs_depth = 1;
}

void vfs_unlock(void) {
    if (vfs_depth == 0 || vfs_owner != current_thread)
        return;   // Not held by this thread
    
    if (--vfs_depth != 0)
        return;   // Still held by an outer vfs_lock()
    
    vfs_owner = NULL;
    mutex_unlock(&vfs_mutex);
}

#endif
