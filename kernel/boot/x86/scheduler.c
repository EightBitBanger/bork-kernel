#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/malloc.h>
#include <kernel/scheduler/configuration.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/watchdog.h>

#include <kernel/util/string.h>
#include <kernel/util/timer.h>

// Stack layout for every thread except the boot thread:
//
//   region + 0                     guard page (unmapped -> overflow page-faults)
//   region + PAGE_SIZE             usable stack, grows down
//   top - FPU_STATE_SIZE           FXSAVE area (16-byte aligned)
//   top = region + THREAD_ALLOC_PAGES * PAGE_SIZE
#define THREAD_STACK_PAGES   4      // 16 KB usable stack
#define THREAD_GUARD_PAGES   1
#define THREAD_ALLOC_PAGES   (THREAD_STACK_PAGES + THREAD_GUARD_PAGES)
#define FPU_STATE_SIZE       512

#define SCHED_TICK_MS        1      // Timer period (quantum ticks are this long)

#include <stddef.h>

// scheduler_context.asm / scheduler_yield_context.asm read fpu_state at this offset
_Static_assert(offsetof(ThreadBlock, fpu_state) == 16, "update TB_FPU_STATE in the scheduler stubs");

struct ThreadInterruptFrame {
    uint32_t gs, fs, es, ds;                               // Pushed by ISR
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; // Pushed by pushal
    uint32_t eip, cs, eflags;                              // Pushed by CPU on interrupt
} __attribute__((packed));

extern uint32_t page_directory[];

ThreadBlock* current_thread = NULL;
ThreadBlock* thread_queue = NULL;

static ThreadBlock* idle_thread = NULL;
static ThreadBlock* reap_list = NULL;

// Timestamp of the previous scheduler entry (for CPU accounting)
static uint64_t sched_last_ms = 0;

static uint8_t main_fpu_state[FPU_STATE_SIZE]    __attribute__((aligned(16)));
static uint8_t default_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

// A clean FXSAVE image: x87 default control word, all exceptions masked
// in MXCSR, empty register stack.
static void fpu_build_default(void) {
    memset(default_fpu_state, 0, FPU_STATE_SIZE);
    *(uint16_t*)&default_fpu_state[0]  = 0x037F;   // FCW
    *(uint32_t*)&default_fpu_state[24] = 0x1F80;   // MXCSR
}

//
// Dynamic priority
//

static inline int thread_effective(const ThreadBlock* t) {
    return t->boosted ? (int)PRIORITY_REALTIME : (int)t->dyn_priority;
}

ThreadPriority thread_get_effective_priority(const ThreadBlock* thread) {
    if (!thread) return PRIORITY_IDLE;
    return (ThreadPriority)thread_effective(thread);
}

static void thread_init_dynamic(ThreadBlock* t, ThreadPriority priority, uint64_t now) {
    t->dyn_priority    = priority;
    t->boosted         = false;
    t->cpu_used_ms     = 0;
    t->recover_mark_ms = now;
    t->wait_mark_ms    = now;
}

// Charge CPU time to the thread that just had the CPU and drop it one level
// for every full allotment it has used.
static void thread_charge_cpu(ThreadBlock* t, uint32_t ms) {
#if !SCHED_DEGRADE_REALTIME
    if (t->priority == PRIORITY_REALTIME) {
        t->cpu_used_ms = 0;
        return;
    }
#endif
    uint32_t allotment = (uint32_t)t->priority * SCHED_DEGRADE_QUANTA * SCHED_TICK_MS;
    
    t->cpu_used_ms += ms;
    while (t->cpu_used_ms >= allotment) {
        if (t->dyn_priority <= PRIORITY_IDLE) {
            t->cpu_used_ms = 0;     // At the floor: nothing left to charge for
            break;
        }
        t->dyn_priority = (ThreadPriority)(t->dyn_priority - 1);
        t->cpu_used_ms -= allotment;
    }
}

// One step back toward the base priority, and forget half the recent usage
static void thread_recover(ThreadBlock* t) {
    t->cpu_used_ms /= 2;
    if (t->dyn_priority < t->priority)
        t->dyn_priority = (ThreadPriority)(t->dyn_priority + 1);
}

// Per-tick aging for every live thread in the queue
static void thread_age(ThreadBlock* t, uint64_t now) {
    if (t->state == THREAD_RUNNING) {
        t->recover_mark_ms = now;
        t->wait_mark_ms = now;
        return;
    }
    
    // Off the CPU (READY, SLEEPING or BLOCKED): slowly earn levels back
    if (now - t->recover_mark_ms >= SCHED_RECOVER_MS) {
        t->recover_mark_ms = now;
        thread_recover(t);
    }
    
    // Starvation clock only runs while the thread actually wants the CPU
    if (t->state != THREAD_READY) {
        t->wait_mark_ms = now;
        return;
    }
    if (!t->boosted && now - t->wait_mark_ms >= SCHED_STARVATION_MS)
        t->boosted = true;
}

// Threads

static void idle_thread_entry(void) {
    while (1) {
        __asm__ volatile("hlt");
    }
}

// Where every thread lands when its entry function returns (it is the fake
// return address on the initial stack), and what kthread_exit() calls.
//
// Marks the thread DEAD and switches away at once instead of halting until
// the next timer tick. The scheduler never saves a DEAD thread's frame and
// the reaper frees the stack on a later pass, after we are off it, so the
// int $0x80 below never returns. The loop is only a backstop.
static __attribute__((noreturn)) void thread_exit(void) {
    __asm__ volatile("cli");
    current_thread->state = THREAD_DEAD;

    while (1) {
        __asm__ volatile("int $0x80" : : : "memory");
    }
}

void kthread_exit(void) {
    thread_exit();
}

// Allocate a guarded stack + FPU area and build the initial interrupt frame.
//
// Initial stack, top down (T = fpu_state, 16-byte aligned):
//
//   T - 16   arg                 first cdecl argument, 16-byte aligned
//   T - 20   thread_exit         return address for the entry function
//   below    ThreadInterruptFrame (popped by the context switch, then iret)
//
// After the iret ESP = T - 20, so ESP % 16 == 12 at the entry point: exactly
// what the i386 SysV ABI promises right after a CALL. A void(void) entry
// simply ignores the argument slot (cdecl: the caller owns it).
static bool thread_setup_stack(ThreadBlock* thread, uint32_t entry_point, void* arg) {
    uint8_t* region = (uint8_t*)vmm_alloc_pages(THREAD_ALLOC_PAGES);
    if (!region) return false;
    
    // Turn the lowest page into a guard page: give its frame back and unmap it.
    // vmm_free_pages() skips non-present pages, so freeing the whole region later is safe.
    uint32_t guard_phys = vmm_get_phys_addr(region);
    vmm_unmap_page((uint32_t)region);
    if (guard_phys != 0) pmm_free_frame(guard_phys);
    
    uint8_t* top = region + (THREAD_ALLOC_PAGES * PAGE_SIZE);
    
    thread->stack_base = region;
    thread->fpu_state  = top - FPU_STATE_SIZE;
    memcpy(thread->fpu_state, default_fpu_state, FPU_STATE_SIZE);
    
    // Stack begins just below the FPU area (16-byte aligned)
    uint32_t stack_top = (uint32_t)thread->fpu_state;
    
    // Argument slot (16-byte aligned) plus 12 bytes of padding above it
    stack_top -= 16;
    *(uint32_t*)stack_top = (uint32_t)arg;

    // Return address for entry_point
    stack_top -= sizeof(uint32_t);
    *(uint32_t*)stack_top = (uint32_t)thread_exit;

    stack_top -= sizeof(struct ThreadInterruptFrame);
    struct ThreadInterruptFrame* frame = (struct ThreadInterruptFrame*)stack_top;
    memset(frame, 0, sizeof(struct ThreadInterruptFrame));

    frame->eip = entry_point;
    frame->cs = 0x08;
    frame->eflags = 0x202;
    frame->ds = 0x10;
    frame->es = 0x10;
    frame->fs = 0x10;
    frame->gs = 0x10;
    
    thread->esp = stack_top;
    return true;
}

static void unlink_thread(ThreadBlock* dead) {
    if (!thread_queue || !dead) return;
    
    if (dead->next == dead) {
        thread_queue = NULL;
        return;
    }
    
    ThreadBlock* prev = dead;
    while (prev->next != dead) {
        prev = prev->next;
    }
    
    prev->next = dead->next;
    
    if (thread_queue == dead) {
        thread_queue = dead->next;
    }
}

// Free every thread that was unlinked on a previous scheduler pass.
// Those threads are never current, so their stacks are not in use.
static void reap_threads(void) {
    while (reap_list != NULL) {
        ThreadBlock* thread = reap_list;
        reap_list = thread->reap_next;
        
        // Drop watchdog references before the block is freed
        wd_forget_thread(thread);
        
        if (thread->stack_base) {
            vmm_free_pages(thread->stack_base, THREAD_ALLOC_PAGES);
        }
        free(thread);
    }
}

void thread_yield(void) {
    if (current_thread)
        current_thread->ticks_remaining = 0;
    
    __asm__ volatile("int $0x80" : : : "memory");
}

void thread_sleep(uint32_t ms) {
    if (ms == 0) {
        thread_yield();
        return;
    }
    
    uint32_t flags = irq_save();
    current_thread->wake_tick = timer_get_ms() + ms;
    current_thread->state = THREAD_SLEEPING;
    
    // int $0x80 works with IF=0; we resume here (still IF=0) once woken
    thread_yield();
    irq_restore(flags);
}

//
// Events
//

void event_init(Event* event) {
    if (!event) return;
    uint32_t flags = irq_save();
    event->signaled = false;
    event->waiters = NULL;
    irq_restore(flags);
}

// Must be called with interrupts disabled
static void event_remove_waiter(Event* event, ThreadBlock* thread) {
    ThreadBlock** link = &event->waiters;
    while (*link) {
        if (*link == thread) {
            *link = thread->wait_next;
            break;
        }
        link = &(*link)->wait_next;
    }
    thread->wait_next = NULL;
    thread->wait_event = NULL;
}

void event_signal(Event* event) {
    if (!event) return;
    uint32_t flags = irq_save();
    
    if (event->waiters == NULL) {
        // Nobody waiting: remember the signal for the next waiter
        event->signaled = true;
    } else {
        ThreadBlock* t = event->waiters;
        event->waiters = NULL;
        while (t) {
            ThreadBlock* following = t->wait_next;
            t->wait_next = NULL;
            t->wait_event = NULL;
            t->wait_signaled = true;
            // A waiter whose timeout already made it READY just keeps running
            if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) {
                t->state = THREAD_READY;
            }
            t = following;
        }
    }

    irq_restore(flags);
}

bool event_wait(Event* event, uint32_t timeout_ms) {
    if (!event) return false;
    
    uint32_t flags = irq_save();
    
    // Before the scheduler exists there is nothing to switch to
    if (!current_thread) {
        bool was = event->signaled;
        event->signaled = false;
        irq_restore(flags);
        return was;
    }
    
    if (event->signaled) {
        event->signaled = false;
        irq_restore(flags);
        return true;
    }
    
    ThreadBlock* self = current_thread;
    self->wait_signaled = false;
    self->wait_event = event;
    self->wait_next = event->waiters;
    event->waiters = self;
    
    if (timeout_ms == 0) {
        self->state = THREAD_BLOCKED;
    } else {
        self->wake_tick = timer_get_ms() + timeout_ms;
        self->state = THREAD_SLEEPING;
    }
    
    // Interrupts stay disabled until we are switched away, so a signal cannot
    // slip in between queueing and sleeping. We resume here with IF=0.
    thread_yield();
    
    // Woken by the timeout: we may still be on the wait list
    if (self->wait_event == event) {
        event_remove_waiter(event, self);
    }
    
    bool signaled = self->wait_signaled;
    self->wait_signaled = false;
    
    irq_restore(flags);
    return signaled;
}

void scheduler_init(void) {
    fpu_build_default();
    
    uint64_t now = timer_get_ms();
    
    // Main (boot) kernel thread: already running on the boot stack
    ThreadBlock* main_thread = malloc(sizeof(ThreadBlock));
    if (!main_thread) return;
    memset(main_thread, 0, sizeof(ThreadBlock));
    
    main_thread->cr3 = (uint32_t)page_directory;
    main_thread->state = THREAD_RUNNING;
    main_thread->priority = PRIORITY_NORMAL;
    main_thread->ticks_remaining = PRIORITY_NORMAL;
    main_thread->stack_base = NULL;
    main_thread->fpu_state = main_fpu_state;
    main_thread->next = main_thread;
    thread_init_dynamic(main_thread, PRIORITY_NORMAL, now);
    
    // Idle thread (not part of the run queue, never aged or degraded)
    ThreadBlock* idle = malloc(sizeof(ThreadBlock));
    if (!idle) return;
    memset(idle, 0, sizeof(ThreadBlock));
    
    if (!thread_setup_stack(idle, (uint32_t)idle_thread_entry, NULL)) {
        free(idle);
        return;
    }
    
    idle->cr3 = (uint32_t)page_directory;
    idle->state = THREAD_READY;
    idle->priority = PRIORITY_IDLE;
    idle->ticks_remaining = PRIORITY_IDLE;
    idle->next = NULL;
    thread_init_dynamic(idle, PRIORITY_IDLE, now);
    
    // Publish last: the timer IRQ is already live and starts scheduling
    // as soon as thread_queue is non-NULL.
    uint32_t flags = irq_save();
    sched_last_ms  = timer_get_ms();
    idle_thread    = idle;
    current_thread = main_thread;
    thread_queue   = main_thread;
    irq_restore(flags);
}

// Build a thread and put it on the run queue. Everything the caller asked
// for (name, cr3) is in place before the enqueue, so the thread can never
// run, or show up in a watchdog report, half configured.
static ThreadBlock* thread_spawn(uint32_t entry_point, void* arg, ThreadPriority priority,
                                 const char* name, uint32_t cr3) {
    if (entry_point == 0) return NULL;
    if (priority < PRIORITY_IDLE || priority > PRIORITY_REALTIME) return NULL;

    ThreadBlock* thread = malloc(sizeof(ThreadBlock));
    if (!thread) return NULL;
    memset(thread, 0, sizeof(ThreadBlock));

    if (!thread_setup_stack(thread, entry_point, arg)) {
        free(thread);
        return NULL;
    }

    thread->cr3 = cr3 ? cr3 : (uint32_t)page_directory;
    thread->state = THREAD_READY;
    thread->priority = priority;
    thread->ticks_remaining = (uint32_t)priority;
    thread->next = NULL;
    thread_init_dynamic(thread, priority, timer_get_ms());

    // Not reachable from the queue yet, so nothing can race this
    if (name != NULL)
        wd_name_thread(thread, name);

    uint32_t flags = irq_save();
    if (!thread_queue) {
        thread_queue = thread;
        thread->next = thread;
    } else {
        ThreadBlock* temp = thread_queue;
        while (temp->next != thread_queue) {
            temp = temp->next;
        }
        temp->next = thread;
        thread->next = thread_queue;
    }
    irq_restore(flags);

    return thread;
}

ThreadBlock* thread_create(void (*entry_point)(void), ThreadPriority priority) {
    return thread_spawn((uint32_t)entry_point, NULL, priority, NULL, 0);
}

ThreadBlock* kthread_create(kthread_fn fn, void* arg, ThreadPriority priority, const char* name) {
    return thread_spawn((uint32_t)fn, arg, priority, name, 0);
}

uint32_t thread_get_count(void) {
    uint32_t flags = irq_save();
    uint32_t count = 0;
    
    if (thread_queue) {
        ThreadBlock* t = thread_queue;
        do {
            if (t->state != THREAD_DEAD) count++;
            t = t->next;
        } while (t != thread_queue);
    }
    
    irq_restore(flags);
    return count;
}

// Called from the timer IRQ and int $0x80 with interrupts disabled.
//
// The entry stubs save the outgoing thread's FPU/SSE state before calling
// this and restore the incoming thread's after, so any C code may run here.
uint32_t thread_handler_c(uint32_t current_esp) {
    if (!thread_queue || !current_thread)
        return current_esp;
    
    ThreadBlock* prev = current_thread;
    
    // Always record where the outgoing thread's frame lives
    if (prev->state != THREAD_DEAD) {
        prev->esp = current_esp;
        wd_tick(timer_get_ms());
    }
    
    reap_threads();
    
    uint64_t now = timer_get_ms();
    uint32_t elapsed = (uint32_t)(now - sched_last_ms);
    sched_last_ms = now;
    
    // Everything since the last scheduler entry was spent running prev
    if (prev != idle_thread && prev->state != THREAD_DEAD) {
        thread_charge_cpu(prev, elapsed);
        
        // Gave up the CPU voluntarily (sleep, event, mutex): win a level back.
        // Usage is only halved, so a short sleep does not erase a CPU hog's history.
        if (prev->state == THREAD_SLEEPING || prev->state == THREAD_BLOCKED)
            thread_recover(prev);
    }
    
    // Pass 1: wake sleepers, unlink dead threads, age everyone else.
    // Count first so removal during iteration cannot shorten/lengthen the walk.
    uint32_t count = 0;
    ThreadBlock* t = thread_queue;
    do {
        count++;
        t = t->next;
    } while (t != thread_queue);
    
    t = thread_queue;
    for (uint32_t i = 0; i < count && thread_queue != NULL; i++) {
        ThreadBlock* following = t->next;
        
        if (t->state == THREAD_SLEEPING && now >= t->wake_tick) {
            t->state = THREAD_READY;
        }
        
        if (t->state == THREAD_DEAD) {
            unlink_thread(t);
            t->reap_next = reap_list;
            reap_list = t;
        } else {
            thread_age(t, now);
        }
        
        t = following;
    }
    
    // A starvation boost lasts exactly one quantum: drop it as soon as the
    // boosted thread leaves the CPU for any reason
    bool prev_continues = prev != idle_thread &&
                        prev->state == THREAD_RUNNING &&
                        prev->ticks_remaining > 1;
    if (prev != idle_thread && !prev_continues)
        prev->boosted = false;
    
    // Pass 2: highest runnable effective priority
    int max_priority = -1;
    if (thread_queue) {
        t = thread_queue;
        do {
            if ((t->state == THREAD_READY || t->state == THREAD_RUNNING) &&
                thread_effective(t) > max_priority) {
                max_priority = thread_effective(t);
            }
            t = t->next;
        } while (t != thread_queue);
    }
    
    ThreadBlock* next;
    
    if (max_priority == -1) {
        // Nothing runnable: fall back to idle
        if (!idle_thread)
            return current_esp;
        next = idle_thread;
    }
    else if (prev_continues && thread_effective(prev) == max_priority) {
        // Keep running the current thread for the rest of its quantum
        prev->ticks_remaining--;
        return current_esp;
    }
    else {
        // Round-robin among threads at max_priority, starting after prev
        ThreadBlock* search_start = (prev == idle_thread || prev->state == THREAD_DEAD)
                                    ? thread_queue : prev->next;
        next = search_start;
        while (!((next->state == THREAD_READY || next->state == THREAD_RUNNING) &&
                thread_effective(next) == max_priority)) {
            next = next->next;
        }
    }
    
    // Retire the outgoing thread. The quantum length follows the base
    // priority, so a degraded thread still gets a fair slice when it runs.
    if (prev != idle_thread) {
        prev->ticks_remaining = (uint32_t)prev->priority;
    }
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }
    
    next->state = THREAD_RUNNING;
    current_thread = next;
    
    // FPU/SSE state is saved and restored by the entry stubs
    // (scheduler_context.asm / scheduler_yield_context.asm), around this
    // function. Doing it here was too late: everything above (wd_tick,
    // reap_threads -> free -> memset, the 64-bit timestamp math, which GCC
    // may move through %xmm0 when SSE is enabled) ran first and could
    // clobber the outgoing thread's XMM registers.
    return next->esp;
}
