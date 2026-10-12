#ifndef _KERNEL_LOCK_H_
#define _KERNEL_LOCK_H_

// Kernel event lock.
//
// Replaces the old kernel_big_lock. It only guards the kernel event system
// (kernel_event_send / kernel_event_update and whatever kernel_event_update
// touches). It does NOT guard the DWM: the DWM has its own lock (dwm_lock)
// and its own thread, see dwm/dwm_thread.c.
//
// Lock order (always take left to right, never the reverse):
//
//     kernel_lock  ->  dwm_lock  ->  filecopy lock  ->  VFS lock
//                                ->  clipboard lock
//
// In particular the DWM thread never takes kernel_lock while it holds
// dwm_lock. DWM code that wants to send a kernel event queues it with
// dwm_kernel_event_send(); the DWM thread delivers the queue after it has
// released dwm_lock at the end of each frame.

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
