#ifndef _KERNEL_LOCK_H_
#define _KERNEL_LOCK_H_

// Kernel event lock.
//
// It only guards the kernel event system
// (kernel_event_send / kernel_event_update and whatever kernel_event_update
// touches), and serializes direct presents to the display (SYS_GFX_PRESENT).
//
// Lock order (always take left to right, never the reverse):
//
//     kernel_lock  ->  VFS lock  ->  driver locks
//
// Per-process syscall tables (files, gfx) are taken before kernel_lock.

#include <stdbool.h>
#include <kernel/mutex.h>

void kernel_lock_init(void);
void kernel_lock_acquire(void);
void kernel_lock_release(void);

// True when the calling thread holds kernel_lock (for asserts / debugging)
bool kernel_lock_held(void);

// The mutex itself, for watchdog registration only
mutex_t* kernel_lock_mutex(void);

#endif

