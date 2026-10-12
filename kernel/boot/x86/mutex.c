#include <stddef.h>

#include <kernel/mutex.h>
#include <kernel/malloc.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/scheduler/watchdog.h>
#include <kernel/util/timer.h>

// Longest owner -> wait_mutex -> owner chain followed when checking for a
// deadlock (a longer chain than this is itself a sign of trouble)
#define MUTEX_CHAIN_MAX  64

// Reference the current running thread defined in scheduler.c
extern ThreadBlock* current_thread;

void mutex_init(mutex_t* mutex) {
    if (!mutex) return;

    uint32_t flags = irq_save();
    mutex->locked = false;
    mutex->owner = NULL;
    mutex->wait_head = NULL;
    mutex->wait_tail = NULL;
    irq_restore(flags);
}

void mutex_lock(mutex_t* mutex) {
    if (!mutex) return;

    uint32_t caller = (uint32_t)__builtin_return_address(0);
    uint32_t flags = irq_save();

    // Locking a mutex we already own would block on ourselves forever with
    // nothing in the report but a BLOCKED thread. Stop here and say so.
    if (mutex->locked && current_thread != NULL && mutex->owner == current_thread) {
        wd_note_self_lock(mutex, caller);   // Never returns
    }

    // Fast path: lock is unowned
    if (!mutex->locked) {
        mutex->locked = true;
        mutex->owner = current_thread;
        irq_restore(flags);
        return;
    }

    // Deadlock check before blocking: follow the owner, the mutex it waits
    // on, that mutex's owner, and so on. If the chain leads back to us, our
    // wait would close a cycle that can never break. The wait graph only
    // changes with interrupts off on this single CPU, so the check is exact:
    // no false positives, and every mutex deadlock is caught as it forms.
    if (current_thread != NULL) {
        ThreadBlock* t = mutex->owner;
        for (int depth = 0; t != NULL && depth < MUTEX_CHAIN_MAX; depth++) {
            if (t == current_thread)
                wd_note_deadlock(mutex, caller);    // Never returns
            if (t->state != THREAD_BLOCKED || t->wait_mutex == NULL)
                break;
            t = t->wait_mutex->owner;
        }
    }

    // Slow path: lock is held. Queue current thread using local stack frame node.
    // (Safe because the thread stack remains allocated while blocked)
    MutexWaiter waiter;
    waiter.thread = current_thread;
    waiter.next = NULL;

    if (mutex->wait_tail != NULL) {
        mutex->wait_tail->next = &waiter;
    } else {
        mutex->wait_head = &waiter;
    }
    mutex->wait_tail = &waiter;

    // Record the wait for deadlock detection and the watchdog
    current_thread->wait_mutex = mutex;
    current_thread->wait_mutex_ms = timer_get_ms();

    // Mark current thread as BLOCKED and yield CPU.
    // THREAD_BLOCKED is only ever cleared by mutex_unlock (sleepers use
    // THREAD_SLEEPING), so when we resume, ownership has been handed to us.
    current_thread->state = THREAD_BLOCKED;

    // Yield with interrupts still disabled so nothing can run between
    // queueing ourselves and switching away. int $0x80 works with IF=0 and
    // execution resumes here (IF=0) once mutex_unlock readies us.
    thread_yield();

    current_thread->wait_mutex = NULL;     // Already cleared by the handoff
    irq_restore(flags);
}

bool mutex_trylock(mutex_t* mutex) {
    if (!mutex) return false;

    uint32_t flags = irq_save();

    if (!mutex->locked) {
        mutex->locked = true;
        mutex->owner = current_thread;
        irq_restore(flags);
        return true;
    }

    irq_restore(flags);
    return false;
}

void mutex_unlock(mutex_t* mutex) {
    if (!mutex) return;

    uint32_t flags = irq_save();

    // Ensure only the thread holding the lock can release it
    if (mutex->owner != current_thread) {
        irq_restore(flags);
        return;
    }

    // Check if threads are waiting in the queue
    if (mutex->wait_head != NULL) {
        // Dequeue next thread
        MutexWaiter* next_waiter = mutex->wait_head;
        mutex->wait_head = next_waiter->next;
        if (mutex->wait_head == NULL) {
            mutex->wait_tail = NULL;
        }

        // Direct handoff: pass ownership directly to dequeued thread. Its wait
        // ends here, in the same critical section, so the wait graph never
        // shows a thread waiting on a lock it already owns.
        mutex->owner = next_waiter->thread;
        next_waiter->thread->wait_mutex = NULL;
        next_waiter->thread->state = THREAD_READY;
    } else {
        // Queue is empty: unlock completely
        mutex->locked = false;
        mutex->owner = NULL;
    }

    irq_restore(flags);
}

void mutex_cancel_wait(ThreadBlock* thread) {
    if (!thread) return;

    uint32_t flags = irq_save();

    mutex_t* mutex = thread->wait_mutex;
    if (mutex != NULL) {
        MutexWaiter* prev = NULL;
        for (MutexWaiter* w = mutex->wait_head; w != NULL; prev = w, w = w->next) {
            if (w->thread != thread) continue;

            if (prev != NULL) prev->next = w->next;
            else              mutex->wait_head = w->next;
            if (mutex->wait_tail == w) mutex->wait_tail = prev;
            break;
        }
        thread->wait_mutex = NULL;
    }

    irq_restore(flags);
}
