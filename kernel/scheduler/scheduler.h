#ifndef _KERNEL_SCHEDULER_H_
#define _KERNEL_SCHEDULER_H_

#include <stdint.h>
#include <stdbool.h>

#define EVENT_INITIALIZER { false, NULL }

typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,            // Waiting for wake_tick (thread_sleep)
    THREAD_BLOCKED,             // Waiting for an explicit wake (mutex, events)
    THREAD_DEAD
} ThreadState;

typedef enum {
    PRIORITY_IDLE       = 1,  // 1 tick   1ms
    PRIORITY_LOW        = 2,  // 2 ticks  2ms
    PRIORITY_NORMAL     = 3,  // 3 ticks  3ms
    PRIORITY_HIGH       = 4,  // 4 ticks  4ms
    PRIORITY_REALTIME   = 5   // 5 ticks  5ms
} ThreadPriority;

struct Mutex;

typedef struct ThreadBlockType {
    uint32_t esp;                       // Current stack pointer
    uint32_t cr3;                       // Page directory (loaded by the scheduler on every switch)
    ThreadState state;                  // Thread state
    void* stack_base;                   // Base of the stack allocation (incl. guard page), NULL for the boot thread
    uint8_t* fpu_state;                 // 512-byte, 16-byte aligned FXSAVE area
    
    ThreadPriority priority;            // Thread base priority (sets the quantum and the recovery ceiling)
    uint32_t ticks_remaining;           // Ticks left in current quantum
    uint64_t wake_tick;                 // Millisecond timestamp when a SLEEPING thread becomes READY
    
    // Dynamic priority
    ThreadPriority dyn_priority;        // Effective priority after degradation / recovery
    bool boosted;                       // Starvation boost: runs at REALTIME for one quantum
    uint32_t cpu_used_ms;               // Recent CPU use, decays while off the CPU
    uint64_t recover_mark_ms;           // Last time it ran (or recovered a level)
    uint64_t wait_mark_ms;              // Last time it was not READY (start of the current wait)
    
    struct ThreadBlockType* next;       // Run queue link (circular)
    struct ThreadBlockType* reap_next;  // Reaper list link
    
    struct Event* wait_event;           // Event this thread is queued on, NULL if none
    struct ThreadBlockType* wait_next;  // Event wait list link
    bool wait_signaled;                 // Set by event_signal when it wakes this thread
    
    // Mutex wait (set by mutex_lock, cleared on handoff). The watchdog follows
    // these to find deadlocks and long waits on any mutex.
    struct Mutex* wait_mutex;           // Mutex this thread is blocked on, NULL if none
    uint64_t wait_mutex_ms;             // When that wait started
    
    // User mode. Appended so fpu_state stays at offset 16 for the asm stubs.
    uint32_t kernel_stack_top;          // TSS esp0 while this thread runs (0: boot thread)
    int32_t process_id;                 // Owning ProcessId, 0 for a kernel thread
    bool user_mode;                     // Started in ring 3

    // Inside the kernel on the process's behalf (a syscall, or ending after
    // a fault). DestroyProcess() from another thread never kills a thread
    // in this state, which may hold kernel locks; it marks the process and
    // the thread ends itself on the way out (see syscall_dispatch).
    volatile bool in_syscall;
} ThreadBlock;

// Auto-reset event: event_signal() wakes every thread currently waiting. If nobody is waiting,
// the event stays signaled and the next event_wait() returns immediately
// (consuming it), so a wakeup that races ahead of the waiter is not lost.
// event_signal() is safe to call from interrupt handlers.
typedef struct Event {
    volatile bool signaled;
    ThreadBlock* waiters;
} Event;

void event_init(Event* event);
void event_signal(Event* event);

// Block until the event is signaled or timeout_ms elapses (0 = no timeout).
// Returns true if woken by a signal, false on timeout.
bool event_wait(Event* event, uint32_t timeout_ms);

// Wakes the kernel event thread early (defined in boot.c)
extern Event kernel_wakeup_event;

void scheduler_init(void);

uint32_t thread_get_count(void);

ThreadBlock* thread_create(void (*entry_point)(void), ThreadPriority priority);

// Kernel threads with an argument
//
// kthread_create() starts fn(arg) on a new kernel thread. The thread is READY
// as soon as this returns (it may already be running), and it ends when fn
// returns or calls kthread_exit(); its stack and ThreadBlock are freed by the
// scheduler afterwards, so don't keep the returned pointer past that.
//
// name (optional, may be NULL) labels the thread in watchdog reports. Only the
// pointer is stored, so pass a string literal or other storage that outlives
// the thread.
//
// arg is the caller's to manage: if it points at memory the caller frees, the
// thread must be done with it first (or own it and free it itself).
typedef void (*kthread_fn)(void* arg);

ThreadBlock* kthread_create(kthread_fn fn, void* arg, ThreadPriority priority, const char* name);

// User-mode threads
//
// thread_create_user() builds a thread whose first instruction is `entry` in
// ring 3, with ESP = user_esp, inside the address space whose page directory
// is cr3 (physical). Both addresses must be in user space and already mapped
// in that space. The thread still owns a normal guarded kernel stack: every
// interrupt, fault and syscall taken in ring 3 lands on it (the scheduler
// points the TSS at it on each switch).
//
// process_id (non-zero) ties the thread to its process for syscalls and
// fault handling. Usually called by process.c, not directly.
ThreadBlock* thread_create_user(uint32_t entry, uint32_t user_esp, ThreadPriority priority,
                                uint32_t cr3, int32_t process_id, const char* name);

// End the calling thread now. Never returns.
void kthread_exit(void) __attribute__((noreturn));

// Priority the scheduler is currently using for this thread
// (degraded, recovered or starvation-boosted). For task lists / debugging.
ThreadPriority thread_get_effective_priority(const ThreadBlock* thread);

void thread_yield(void);
void thread_sleep(uint32_t ms);

uint32_t thread_handler_c(uint32_t current_esp);

#endif
