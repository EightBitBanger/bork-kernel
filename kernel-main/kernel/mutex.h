#ifndef _KERNEL_MUTEX_H_
#define _KERNEL_MUTEX_H_

#include <stdint.h>
#include <stdbool.h>
#include <kernel/scheduler/scheduler.h>

typedef struct MutexWaiter {
    ThreadBlock* thread;
    struct MutexWaiter* next;
} MutexWaiter;

typedef struct Mutex {
    bool locked;
    ThreadBlock* owner;         // Thread currently holding the lock
    MutexWaiter* wait_head;     // Head of the queue waiting for lock
    MutexWaiter* wait_tail;     // Tail of the queue waiting for lock
} mutex_t;

void mutex_init(mutex_t* mutex);
void mutex_lock(mutex_t* mutex);
bool mutex_trylock(mutex_t* mutex);
void mutex_unlock(mutex_t* mutex);

// Remove a thread from the wait queue of the mutex it is blocked on (if any).
// For killing a BLOCKED thread: its MutexWaiter node lives on its stack.
void mutex_cancel_wait(ThreadBlock* thread);

#endif

