#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/watchdog.h>
#include <kernel/mutex.h>
#include <kernel/panic/panic_error.h>
#include <kernel/util/timer.h>

// ==========================================
// Configuration (override with -D)
// ==========================================

#define WD_SERIAL           1           // Mirror reports to COM1 when present
#define WD_LOCK_REPORT_MS   5000        // Mutex wait: COM1 report, keep running
#define WD_LOCK_PANIC_MS    20000       // Mutex wait: panic screen (WD_NEVER = never)
#define WD_SCAN_MS          50          // How often wd_tick looks at threads and locks
#define WD_NMI              1           // Hard-lockup NMI (wd_nmi_init)
#define WD_NMI_STRIKES      4           // NMIs (2 Hz) with no scheduler run -> panic (~2 s)
#define WD_IOAPIC_PHYS      0xFEC00000u // Standard IOAPIC address (ACPI MADT has the real one)

#ifndef WD_IOAPIC_MAP_FLAGS
#define WD_IOAPIC_MAP_FLAGS (VM_PRESENT | VM_READWRITE) // MMIO must be uncached: use your vmm.h's name for it
#endif

#define COM1              0x3F8
#define WD_BUF_SIZE       8192
#define WD_MAX_LOCKS      24
#define WD_MAX_NAMES      24
#define WD_MAX_WATCHES    16
#define WD_MAX_THREADS    64
#define WD_MAX_WAITERS    32
#define WD_MAX_FRAMES     10        // One backtrace line must fit the screen at 1x
#define WD_MAX_CHAIN      16
#define WD_PAGE           4096u
#define WD_STACK_PAGES    5         // THREAD_ALLOC_PAGES in scheduler.c (page 0 is the guard)
#define WD_BOOT_STACK     0x4000u   // multiboot.asm reserves 16 KB

// Must match struct ThreadInterruptFrame in scheduler.c
struct WdFrame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags;
};

// What isr_nmi.asm passes: pusha block + CPU frame (no privilege change)
struct WdNmiFrame {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags;
};

extern ThreadBlock* thread_queue;
extern ThreadBlock* current_thread;

typedef struct {
    const char* name;           // NULL: limits only, shown as an address
    mutex_t*    mutex;
    uint32_t    report_ms;
    uint32_t    panic_ms;
} WdLock;

typedef struct {
    ThreadBlock*      thread;   // NULL: free slot
    const char*       name;
    uint32_t          report_ms;
    uint32_t          panic_ms;
    volatile uint32_t beats;    // Bumped by wd_pet()
    uint32_t          seen;     // beats at the last change
    uint64_t          last_change;
    bool              armed;    // First pet seen
    bool              reported;
} WdWatch;

static WdLock  wd_locks[WD_MAX_LOCKS];
static int     wd_lock_count = 0;

static struct { ThreadBlock* thread; const char* name; } wd_names[WD_MAX_NAMES];
static int     wd_name_count = 0;

static WdWatch wd_watches[WD_MAX_WATCHES];

static bool     wd_serial_ok  = false;
static uint64_t wd_last_scan  = 0;

// Scheduler entries: the NMI checks this moves
static volatile uint32_t wd_progress = 0;

// The one long mutex wait currently reported (to avoid repeating it)
static ThreadBlock* lw_thread = NULL;
static mutex_t*     lw_mutex  = NULL;
static uint64_t     lw_start  = 0;

// Report text. Static: the watchdog runs inside the scheduler or an NMI with
// IF=0, so it must not allocate.
static char     wd_buf[WD_BUF_SIZE];
static uint32_t wd_len = 0;

//
// Output: the report buffer, mirrored to COM1
//

static void ser_putc(char c) {
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) { }
    outb(COM1, (uint8_t)c);
}

static void out_reset(void) {
    wd_len = 0;
    wd_buf[0] = '\0';
}

static void out_c(char c) {
    if (wd_len + 1 < WD_BUF_SIZE) {
        wd_buf[wd_len++] = c;
        wd_buf[wd_len] = '\0';
    }
#if WD_SERIAL
    if (wd_serial_ok) {
        if (c == '\n') ser_putc('\r');
        ser_putc(c);
    }
#endif
}

static void out_s(const char* s) {
    while (*s) out_c(*s++);
}

static void out_hex(uint32_t v) {
    out_s("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        out_c("0123456789abcdef"[(v >> shift) & 0xF]);
}

static void out_dec(uint32_t v) {
    char digits[11];
    int n = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v != 0);
    while (n > 0) out_c(digits[--n]);
}

//
// Lookups
//

static const char* wd_thread_name(const ThreadBlock* t) {
    for (int i = 0; i < wd_name_count; i++)
        if (wd_names[i].thread == t) return wd_names[i].name;
    return NULL;
}

static void out_thread(const ThreadBlock* t) {
    if (t == NULL) {
        out_s("nobody");
        return;
    }
    const char* name = wd_thread_name(t);
    if (name != NULL) {
        out_s(name);
        out_c('(');
        out_hex((uint32_t)t);
        out_c(')');
    } else {
        out_hex((uint32_t)t);
    }
}

static const char* wd_state_name(ThreadState s) {
    switch (s) {
        case THREAD_READY:    return "READY";
        case THREAD_RUNNING:  return "RUNNING";
        case THREAD_SLEEPING: return "SLEEPING";
        case THREAD_BLOCKED:  return "BLOCKED";
        case THREAD_DEAD:     return "DEAD";
        default:              return "?";
    }
}

static WdLock* wd_lock_find(const mutex_t* m) {
    for (int i = 0; i < wd_lock_count; i++)
        if (wd_locks[i].mutex == m) return &wd_locks[i];
    return NULL;
}

static const char* wd_lock_name(const mutex_t* m) {
    WdLock* l = wd_lock_find(m);
    return l ? l->name : NULL;
}

static void out_mutex(const mutex_t* m) {
    const char* name = wd_lock_name(m);
    if (name != NULL) out_s(name); else out_hex((uint32_t)m);
}

static void wd_lock_limits(const mutex_t* m, uint32_t* report_ms, uint32_t* panic_ms) {
    WdLock* l = wd_lock_find(m);
    *report_ms = l ? l->report_ms : WD_LOCK_REPORT_MS;
    *panic_ms  = l ? l->panic_ms  : WD_LOCK_PANIC_MS;
}

// The mutex this thread is blocked on, or NULL. Every mutex is covered:
// mutex_lock records it in the ThreadBlock.
static mutex_t* wd_waiting_on(const ThreadBlock* t) {
    return (t->state == THREAD_BLOCKED) ? t->wait_mutex : NULL;
}

static WdWatch* wd_watch_find(const ThreadBlock* t) {
    for (int i = 0; i < WD_MAX_WATCHES; i++)
        if (wd_watches[i].thread == t) return &wd_watches[i];
    return NULL;
}

// Parked = idle on purpose (thread_sleep, or event_wait with or without a
// timeout). A parked thread is not frozen, so its heartbeat clock is paused.
static bool wd_is_parked(const ThreadBlock* t) {
    if (t->state == THREAD_SLEEPING) return true;
    if (t->state == THREAD_BLOCKED && t->wait_event != NULL) return true;
    return false;
}

//
// Stacks and backtraces
//

static void wd_stack_range(const ThreadBlock* t, uint32_t* lo, uint32_t* hi) {
    if (t->stack_base != NULL) {
        *lo = (uint32_t)t->stack_base + WD_PAGE;              // Skip the unmapped guard page
        *hi = (uint32_t)t->stack_base + WD_STACK_PAGES * WD_PAGE;
    } else {
        *lo = t->esp;                                         // Boot thread: .bss stack
        *hi = t->esp + WD_BOOT_STACK;
    }
}

// The interrupt frame saved when the thread last entered the scheduler
static const struct WdFrame* wd_frame(const ThreadBlock* t) {
    uint32_t lo, hi;
    wd_stack_range(t, &lo, &hi);
    if (t->esp == 0 || (t->esp & 3) || t->esp < lo || t->esp + sizeof(struct WdFrame) > hi)
        return NULL;
    return (const struct WdFrame*)t->esp;
}

// " bt: a b c" following saved EBPs inside [lo, hi)
static void out_backtrace(uint32_t ebp, uint32_t lo, uint32_t hi) {
    out_s("  bt:");
    for (int i = 0; i < WD_MAX_FRAMES; i++) {
        if (ebp < lo || ebp + 8 > hi || (ebp & 3)) break;
        uint32_t next = ((const uint32_t*)ebp)[0];
        out_c(' ');
        out_hex(((const uint32_t*)ebp)[1]);
        if (next <= ebp) break;
        ebp = next;
    }
}

// Backtrace of the code calling this (used by the mutex hooks, where the
// current thread has no saved frame worth reading)
static void out_backtrace_here(void) {
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    uint32_t ebp = (uint32_t)__builtin_frame_address(0);

    uint32_t hi = esp + WD_BOOT_STACK;
    if (current_thread != NULL && current_thread->stack_base != NULL)
        hi = (uint32_t)current_thread->stack_base + WD_STACK_PAGES * WD_PAGE;

    out_s("      ");
    out_backtrace(ebp, esp, hi);
    out_c('\n');
}

//
// Report sections
//

static void wd_print_chain(ThreadBlock** chain, mutex_t** via, int from, int count, const ThreadBlock* end) {
    out_s("  ");
    for (int k = from; k < count; k++) {
        out_thread(chain[k]);
        out_s(" -> [");
        out_mutex(via[k]);
        out_s("] -> ");
    }
    out_thread(end);
    out_c('\n');
}

// Follow `start`'s wait chain (waits on mutex -> owned by -> waits on ...)
// and say what it ends in
static void wd_verdict(ThreadBlock* start, uint64_t now) {
    ThreadBlock* t = start;
    out_s("VERDICT: ");

    ThreadBlock* chain[WD_MAX_CHAIN];
    mutex_t* via[WD_MAX_CHAIN];
    int n = 0;

    while (n < WD_MAX_CHAIN) {
        for (int j = 0; j < n; j++) {
            if (chain[j] == t) {
                // mutex_lock refuses to close a cycle, so this means the
                // wait state was corrupted (or a lock bypassed mutex_lock)
                out_s("DEADLOCK, lock cycle:\n");
                wd_print_chain(chain, via, j, n, t);
                return;
            }
        }

        mutex_t* m = wd_waiting_on(t);
        if (m == NULL) break;

        chain[n] = t;
        via[n]   = m;
        n++;

        if (m->owner == NULL) {
            out_s("lock '");
            out_mutex(m);
            out_s("' has waiters but no owner (corrupt mutex)\n");
            wd_print_chain(chain, via, 0, n, t);
            return;
        }
        t = m->owner;
    }

    if (n == WD_MAX_CHAIN) {
        out_s("wait chain too long to follow\n");
        return;
    }

    switch (t->state) {
        case THREAD_RUNNING:
        case THREAD_READY:
            if (n == 0) {
                out_s("NOT A LOCK WAIT. ");
                out_thread(t);
                out_s(" is runnable but not making\n"
                      "  progress: it is spinning or doing very slow work. Read its backtrace.\n");
            } else {
                out_s("NOT A LOCK CYCLE. The lock holder is runnable: it is making\n"
                      "  slow progress or spinning. Read its backtrace.\n");
            }
            break;

        case THREAD_SLEEPING:
            out_s("the chain ends at a sleeping thread (wakes in ");
            out_dec(t->wake_tick > now ? (uint32_t)(t->wake_tick - now) : 0);
            out_s(" ms)\n");
            break;

        case THREAD_BLOCKED:
            if (t->wait_event != NULL) {
                out_s("the chain ends at a thread waiting on event ");
                out_hex((uint32_t)t->wait_event);
                out_s(":\n  a wakeup that never came (lost or never sent)\n");
            } else {
                out_s("the chain ends at a BLOCKED thread with no recorded wait\n"
                      "  (blocked outside mutex_lock / event_wait, or corrupt state)\n");
            }
            break;

        case THREAD_DEAD:
            out_s("the chain ends at a DEAD thread that still owns a lock\n");
            break;

        default:
            out_s("the chain ends at a thread in an unknown state\n");
            break;
    }

    if (n > 0) wd_print_chain(chain, via, 0, n, t);
}

static void wd_print_lock(const mutex_t* m) {
    out_s("  ");
    out_mutex(m);
    if (!m->locked) {
        out_s("  free\n");
        return;
    }
    out_s("  held by ");
    out_thread(m->owner);

    int guard = WD_MAX_WAITERS;
    bool any = false;
    for (MutexWaiter* w = m->wait_head; w != NULL && guard-- > 0; w = w->next) {
        out_s(any ? ", " : "  waiters: ");
        out_thread(w->thread);
        any = true;
    }
    out_c('\n');
}

static void wd_locks_section(void) {
    out_s("\nLOCKS\n");
    for (int i = 0; i < wd_lock_count; i++)
        if (wd_locks[i].name != NULL) wd_print_lock(wd_locks[i].mutex);

    // Unnamed mutexes somebody is waiting on (each printed once)
    if (thread_queue == NULL) return;
    ThreadBlock* t = thread_queue;
    int guard = WD_MAX_THREADS;
    do {
        mutex_t* m = wd_waiting_on(t);
        if (m != NULL && wd_lock_name(m) == NULL) {
            bool seen = false;
            for (ThreadBlock* u = thread_queue; u != t; u = u->next) {
                if (wd_waiting_on(u) == m) { seen = true; break; }
            }
            if (!seen) wd_print_lock(m);
        }
        t = t->next;
    } while (t != NULL && t != thread_queue && guard-- > 0);
}

static void wd_dump_thread(ThreadBlock* t, uint64_t now, const ThreadBlock* stale) {
    out_s("  ");
    out_thread(t);
    out_s("  ");
    out_s(wd_state_name(t->state));
    out_s("  prio ");
    out_dec((uint32_t)t->priority);
    out_c('/');
    out_dec((uint32_t)thread_get_effective_priority(t));
    if (t->boosted) out_s(" boosted");

    if (t->state == THREAD_SLEEPING) {
        out_s("  wakes in ");
        out_dec(t->wake_tick > now ? (uint32_t)(t->wake_tick - now) : 0);
        out_s(" ms");
    }
    if (t->wait_event != NULL) {
        out_s("  event ");
        out_hex((uint32_t)t->wait_event);
    }

    mutex_t* m = wd_waiting_on(t);
    if (m != NULL) {
        out_s("  waits on ");
        out_mutex(m);
        out_s(" (held by ");
        out_thread(m->owner);
        out_s(") for ");
        out_dec((uint32_t)(now - t->wait_mutex_ms));
        out_s(" ms");
    }

    WdWatch* w = wd_watch_find(t);
    if (w != NULL && w->armed) {
        out_s("  last pet ");
        out_dec((uint32_t)(now - w->last_change));
        out_s(" ms ago");
    }
    out_c('\n');

    if (t == stale) {
        out_s("      (running now, no saved frame)\n");
        return;
    }

    const struct WdFrame* f = wd_frame(t);
    if (f == NULL) {
        out_s("      (saved frame is not readable)\n");
        return;
    }

    out_s("      eip ");
    out_hex(f->eip);
    uint32_t lo, hi;
    wd_stack_range(t, &lo, &hi);
    out_backtrace(f->ebp, lo, hi);
    out_c('\n');
}

static void wd_threads_section(uint64_t now, const ThreadBlock* stale) {
    out_s("\nTHREADS  (prio is base/effective)\n");

    bool current_seen = false;
    if (thread_queue != NULL) {
        ThreadBlock* t = thread_queue;
        int guard = WD_MAX_THREADS;
        do {
            if (t == current_thread) current_seen = true;
            wd_dump_thread(t, now, stale);
            t = t->next;
        } while (t != NULL && t != thread_queue && guard-- > 0);
    }

    // The idle thread is not in the run queue
    if (!current_seen && current_thread != NULL)
        wd_dump_thread(current_thread, now, stale);
}

// Everything after the headline. `focus` gets a verdict (may be NULL).
// `stale` is a thread whose saved frame is out of date (it is running right
// now), or NULL.
static void wd_body(uint64_t now, ThreadBlock* focus, const ThreadBlock* stale) {
    if (focus != NULL) wd_verdict(focus, now);
    wd_locks_section();
    wd_threads_section(now, stale);
    out_s("\nMap eip/bt addresses with: addr2line -f -e kernel.elf <address>\n");
}

static __attribute__((noreturn)) void wd_panic(uint32_t code, uint32_t address) {
    kernel_crashout(code, address, PT_WATCHDOG, wd_buf);
    for (;;) __asm__ volatile("cli; hlt");
}

//
// Detector 3: heartbeats
//

static void wd_check_watches(uint64_t now) {
    for (int i = 0; i < WD_MAX_WATCHES; i++) {
        WdWatch* w = &wd_watches[i];
        ThreadBlock* t = w->thread;
        if (t == NULL) continue;

        uint32_t beats = w->beats;
        if (beats != w->seen) {
            if (w->reported && wd_serial_ok) {
                out_reset();
                out_s("[watchdog] ");
                out_s(w->name);
                out_s(" resumed after ");
                out_dec((uint32_t)(now - w->last_change));
                out_s(" ms: slow, not frozen\n");
            }
            w->seen        = beats;
            w->last_change = now;
            w->armed       = true;
            w->reported    = false;
            continue;
        }

        // Not started yet, or idle on purpose: the clock does not run
        if (!w->armed || wd_is_parked(t) || t->state == THREAD_DEAD) {
            w->last_change = now;
            w->reported    = false;
            continue;
        }

        uint32_t stalled = (uint32_t)(now - w->last_change);

        if (w->panic_ms != WD_NEVER && stalled >= w->panic_ms) {
            out_reset();
            out_s("STALLED: ");
            out_thread(t);
            out_s(" has not checked in for ");
            out_dec(stalled);
            out_s(" ms (limit ");
            out_dec(w->panic_ms);
            out_s(" ms)   tick ");
            out_dec((uint32_t)now);
            out_s("\n\n");
            wd_body(now, t, NULL);
            wd_panic(stalled, 0);
        }

        if (!w->reported && w->report_ms != WD_NEVER && stalled >= w->report_ms) {
            w->reported = true;
            if (wd_serial_ok) {
                out_reset();
                out_s("\n[watchdog] early report, system still running\n");
                out_s("No check-in from ");
                out_thread(t);
                out_s(" for ");
                out_dec(stalled);
                out_s(" ms\n\n");
                wd_body(now, t, NULL);
            }
        }
    }
}

//
// Detector 2: long mutex waits
//

static void wd_check_lock_waits(uint64_t now) {
    // Has the wait we reported ended?
    if (lw_thread != NULL &&
        !(lw_thread->state == THREAD_BLOCKED && lw_thread->wait_mutex == lw_mutex &&
          lw_thread->wait_mutex_ms == lw_start)) {
        if (wd_serial_ok) {
            out_reset();
            out_s("[watchdog] ");
            out_thread(lw_thread);
            out_s(" stopped waiting for ");
            out_mutex(lw_mutex);
            out_s(" after ");
            out_dec((uint32_t)(now - lw_start));
            out_s(" ms\n");
        }
        lw_thread = NULL;
    }

    if (thread_queue == NULL) return;

    ThreadBlock* worst = NULL;
    ThreadBlock* t = thread_queue;
    int guard = WD_MAX_THREADS;
    do {
        mutex_t* m = wd_waiting_on(t);
        if (m != NULL) {
            uint32_t report_ms, panic_ms;
            wd_lock_limits(m, &report_ms, &panic_ms);
            uint32_t waited = (uint32_t)(now - t->wait_mutex_ms);

            if (panic_ms != WD_NEVER && waited >= panic_ms) {
                out_reset();
                out_s("LOCK WAIT: ");
                out_thread(t);
                out_s(" has waited ");
                out_dec(waited);
                out_s(" ms for ");
                out_mutex(m);
                out_s(" held by ");
                out_thread(m->owner);
                out_s(" (limit ");
                out_dec(panic_ms);
                out_s(" ms)\n\n");
                wd_body(now, t, NULL);
                wd_panic(waited, 0);
            }

            if (report_ms != WD_NEVER && waited >= report_ms &&
                (worst == NULL || t->wait_mutex_ms < worst->wait_mutex_ms))
                worst = t;
        }
        t = t->next;
    } while (t != NULL && t != thread_queue && guard-- > 0);

    // One serial report per stuck wait
    if (worst != NULL && lw_thread == NULL) {
        lw_thread = worst;
        lw_mutex  = worst->wait_mutex;
        lw_start  = worst->wait_mutex_ms;

        if (wd_serial_ok) {
            out_reset();
            out_s("\n[watchdog] early report, system still running\n");
            out_thread(worst);
            out_s(" has waited ");
            out_dec((uint32_t)(now - lw_start));
            out_s(" ms for ");
            out_mutex(lw_mutex);
            out_s("\n\n");
            wd_body(now, worst, NULL);
        }
    }
}

//
// Detector 4: hard-lockup NMI
//
// The RTC raises its periodic interrupt at 2 Hz. It is routed through the
// IOAPIC with delivery mode NMI, so it arrives even with interrupts disabled.
// (The local APIC timer cannot do this: its LVT has no delivery mode field.)
// The local APIC accepts NMI messages even while software-disabled, so the
// 8259 PIC setup is not touched. IRQ8 stays masked on the slave PIC.

#define CMOS_INDEX        0x70
#define CMOS_DATA         0x71
#define CMOS_NMI_OFF      0x80
#define RTC_REG_A         0x0A
#define RTC_REG_B         0x0B
#define RTC_REG_C         0x0C
#define RTC_RATE_2HZ      0x0F      // 32768 >> (15 - 1)
#define RTC_B_PIE         0x40
#define RTC_C_PF          0x40
#define RTC_NMI_MS        500

#define IOAPIC_REG_VER    0x01
#define IOAPIC_REDTBL(n)  (0x10 + 2 * (n))
#define IOAPIC_RTC_PIN    8         // ISA IRQ 8 (no MADT override in practice)
#define IOAPIC_DM_NMI     (4u << 8)

static volatile bool     wd_nmi_enabled   = false;
static volatile uint32_t wd_nmi_count     = 0;
static uint32_t          wd_nmi_last_prog = 0;
static uint32_t          wd_nmi_strikes   = 0;
static uint64_t          wd_nmi_armed_ms  = 0;
static bool              wd_nmi_verified  = false;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg | CMOS_NMI_OFF);
    return inb(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t value) {
    outb(CMOS_INDEX, reg | CMOS_NMI_OFF);
    outb(CMOS_DATA, value);
}

// Read (and so clear) RTC register C, leaving NMIs unmasked. The RTC raises
// no further interrupt until C has been read.
static uint8_t rtc_ack(void) {
    outb(CMOS_INDEX, RTC_REG_C);
    return inb(CMOS_DATA);
}

static volatile uint32_t* wd_ioapic = NULL;

static uint32_t ioapic_read(uint8_t reg) {
    wd_ioapic[0] = reg;
    return wd_ioapic[4];
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    wd_ioapic[0] = reg;
    wd_ioapic[4] = value;
}

bool wd_nmi_init(void) {
#if WD_NMI
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (!(edx & (1u << 9)))
        return false;               // No local APIC to receive IOAPIC messages

    uint32_t base_lo, base_hi;
    __asm__ volatile("rdmsr" : "=a"(base_lo), "=d"(base_hi) : "c"(0x1B));
    if (!(base_lo & (1u << 11)))
        return false;               // APIC globally disabled by firmware
    uint8_t apic_id = (uint8_t)(ebx >> 24);

    // Identity mapped, created now (before any process copies the kernel
    // directory). Only used here; the NMI handler needs nothing but ports.
    vmm_map_hardware_region(WD_IOAPIC_PHYS, WD_IOAPIC_PHYS, WD_PAGE, WD_IOAPIC_MAP_FLAGS);
    wd_ioapic = (volatile uint32_t*)WD_IOAPIC_PHYS;

    uint32_t version = ioapic_read(IOAPIC_REG_VER);
    if (version == 0xFFFFFFFFu || ((version >> 16) & 0xFF) < IOAPIC_RTC_PIN)
        return false;               // No IOAPIC there

    uint32_t flags = irq_save();

    wd_nmi_last_prog = wd_progress;
    wd_nmi_strikes   = 0;
    wd_nmi_armed_ms  = timer_get_ms();
    wd_nmi_enabled   = true;

    uint8_t a = cmos_read(RTC_REG_A);
    cmos_write(RTC_REG_A, (uint8_t)((a & 0xF0) | RTC_RATE_2HZ));
    uint8_t b = cmos_read(RTC_REG_B);
    cmos_write(RTC_REG_B, (uint8_t)(b | RTC_B_PIE));

    // Pin 8 -> NMI, physical destination = this CPU, edge, active high
    ioapic_write(IOAPIC_REDTBL(IOAPIC_RTC_PIN) + 1, (uint32_t)apic_id << 24);
    ioapic_write(IOAPIC_REDTBL(IOAPIC_RTC_PIN), IOAPIC_DM_NMI | 0x02);

    // Clears anything pending (so the next period is a clean edge) and
    // leaves port 0x70 with NMIs unmasked
    rtc_ack();

    irq_restore(flags);
    return true;
#else
    return false;
#endif
}

// Called from isr_nmi.asm
void wd_nmi_handler(const struct WdNmiFrame* f) {
    // Chipset error NMIs: SERR / parity (bit 7), IOCHK (bit 6)
    uint8_t sysctl = inb(0x61);
    if ((sysctl & 0xC0) && !kernel_panicking)
        kernel_crashout(sysctl, f->eip, PT_CPU_EXCEPTION, "NMI: HARDWARE ERROR (SERR / IOCHK)");

    uint8_t rtc = wd_nmi_enabled ? rtc_ack() : 0;
    if (kernel_panicking) return;   // Leave the first panic screen alone

    if (!(rtc & RTC_C_PF)) {
        // Not ours and not a chipset error
        kernel_crashout(0, f->eip, PT_CPU_EXCEPTION, "NMI");
    }

    wd_nmi_count++;

    uint32_t progress = wd_progress;
    if (progress != wd_nmi_last_prog) {
        wd_nmi_last_prog = progress;
        wd_nmi_strikes   = 0;
        return;
    }
    if (++wd_nmi_strikes < WD_NMI_STRIKES)
        return;

    // The scheduler has not run for WD_NMI_STRIKES periods
    out_reset();
    out_s("The scheduler has not run for about ");
    out_dec(WD_NMI_STRIKES * RTC_NMI_MS);
    out_s(" ms.\n");
    if (f->eflags & 0x200)
        out_s("Interrupts are ON but the timer IRQ is not being serviced\n"
              "(IRQ 0 masked, a missing EOI, or an IRQ storm).\n");
    else
        out_s("Interrupts are OFF: a cli section or an IRQ handler never finished.\n");

    out_s("Interrupted at eip ");
    out_hex(f->eip);
    out_s("  eflags ");
    out_hex(f->eflags);
    out_s("  on ");
    out_thread(current_thread);
    out_c('\n');

    // No privilege change: the interrupted ESP is just above the CPU frame
    uint32_t lo = (uint32_t)&f->eflags + 4;
    uint32_t hi = lo + WD_BOOT_STACK;
    if (current_thread != NULL && current_thread->stack_base != NULL)
        hi = (uint32_t)current_thread->stack_base + WD_STACK_PAGES * WD_PAGE;
    out_s("     ");
    out_backtrace(f->ebp, lo, hi);
    out_s("\n\n");

    // Scheduler state may be mid-update; the dumps are bounded either way
    wd_body(timer_get_ms(), NULL, current_thread);
    kernel_crashout(WD_NMI_STRIKES * RTC_NMI_MS, f->eip, PT_WATCHDOG, wd_buf);
}

static void wd_check_nmi_armed(uint64_t now) {
    if (!wd_nmi_enabled || wd_nmi_verified) return;
    if (now - wd_nmi_armed_ms < 4 * RTC_NMI_MS) return;

    wd_nmi_verified = true;
    if (wd_serial_ok) {
        out_reset();
        out_s(wd_nmi_count > 0 ? "[watchdog] NMI active (RTC via IOAPIC)\n"
                               : "[watchdog] NMI not active\n");
    }
}

//
// Public
//

void wd_init(void) {
#if WD_SERIAL
    // Scratch register test: no UART, no serial output
    outb(COM1 + 7, 0xA5);
    if (inb(COM1 + 7) != 0xA5)
        return;

    outb(COM1 + 1, 0x00);   // No serial interrupts
    outb(COM1 + 3, 0x80);   // DLAB on
    outb(COM1 + 0, 0x01);   // 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   // 8N1, DLAB off
    outb(COM1 + 2, 0xC7);   // FIFO on, cleared
    outb(COM1 + 4, 0x03);   // DTR + RTS

    wd_serial_ok = true;
    out_reset();
    out_s("\n[watchdog] serial up\n");
#endif
}

bool wd_watch(ThreadBlock* thread, const char* name, uint32_t report_ms, uint32_t panic_ms) {
    if (thread == NULL) return false;
    if (name != NULL) wd_name_thread(thread, name);

    uint32_t flags = irq_save();
    WdWatch* w = wd_watch_find(thread);
    if (w == NULL) w = wd_watch_find(NULL);
    if (w == NULL) {
        irq_restore(flags);
        return false;
    }

    w->name        = (name != NULL) ? name : "thread";
    w->report_ms   = report_ms;
    w->panic_ms    = panic_ms;
    w->beats       = 0;
    w->seen        = 0;
    w->last_change = timer_get_ms();
    w->armed       = false;
    w->reported    = false;
    w->thread      = thread;        // Last: the slot goes live
    irq_restore(flags);
    return true;
}

void wd_unwatch(ThreadBlock* thread) {
    if (thread == NULL) return;
    uint32_t flags = irq_save();
    WdWatch* w = wd_watch_find(thread);
    if (w != NULL) w->thread = NULL;
    irq_restore(flags);
}

void wd_pet(void) {
    ThreadBlock* self = current_thread;
    if (self == NULL) return;
    for (int i = 0; i < WD_MAX_WATCHES; i++) {
        if (wd_watches[i].thread == self) {
            wd_watches[i].beats++;
            return;
        }
    }
}

void wd_name_thread(ThreadBlock* thread, const char* name) {
    if (thread == NULL || name == NULL) return;
    uint32_t flags = irq_save();
    for (int i = 0; i < wd_name_count; i++) {
        if (wd_names[i].thread == thread) {
            wd_names[i].name = name;
            irq_restore(flags);
            return;
        }
    }
    if (wd_name_count < WD_MAX_NAMES) {
        wd_names[wd_name_count].thread = thread;
        wd_names[wd_name_count].name   = name;
        wd_name_count++;
    }
    irq_restore(flags);
}

static WdLock* wd_lock_add(mutex_t* mutex) {
    WdLock* l = wd_lock_find(mutex);
    if (l != NULL) return l;
    if (wd_lock_count >= WD_MAX_LOCKS) return NULL;
    l = &wd_locks[wd_lock_count++];
    l->name      = NULL;
    l->mutex     = mutex;
    l->report_ms = WD_LOCK_REPORT_MS;
    l->panic_ms  = WD_LOCK_PANIC_MS;
    return l;
}

void wd_register_lock(const char* name, mutex_t* mutex) {
    if (mutex == NULL) return;
    uint32_t flags = irq_save();
    WdLock* l = wd_lock_add(mutex);
    if (l != NULL) l->name = name;
    irq_restore(flags);
}

void wd_set_lock_limits(mutex_t* mutex, uint32_t report_ms, uint32_t panic_ms) {
    if (mutex == NULL) return;
    uint32_t flags = irq_save();
    WdLock* l = wd_lock_add(mutex);
    if (l != NULL) {
        l->report_ms = report_ms;
        l->panic_ms  = panic_ms;
    }
    irq_restore(flags);
}

void wd_forget_thread(ThreadBlock* thread) {
    if (thread == NULL) return;

    for (int i = 0; i < WD_MAX_WATCHES; i++)
        if (wd_watches[i].thread == thread) wd_watches[i].thread = NULL;

    // Keep the name table compact so a reused address never inherits a name
    for (int i = 0; i < wd_name_count; i++) {
        if (wd_names[i].thread == thread) {
            wd_names[i] = wd_names[--wd_name_count];
            break;
        }
    }

    if (lw_thread == thread) lw_thread = NULL;
}

void wd_tick(uint64_t now) {
    wd_progress++;

    if (now - wd_last_scan < WD_SCAN_MS) return;
    wd_last_scan = now;

    wd_check_watches(now);
    wd_check_lock_waits(now);
    wd_check_nmi_armed(now);
}

void wd_note_self_lock(const mutex_t* mutex, uint32_t caller) {
    out_reset();
    out_s("SELF-DEADLOCK: ");
    out_thread(current_thread);
    out_s(" locked ");
    out_mutex(mutex);
    out_s(" which it already owns.\nCalled from ");
    out_hex(caller);
    out_s(". The mutex is not recursive, so this thread would wait on itself forever.\n");
    out_backtrace_here();
    out_c('\n');

    wd_body(timer_get_ms(), NULL, current_thread);
    wd_panic(0, caller);
}

void wd_note_deadlock(const mutex_t* wanted, uint32_t caller) {
    out_reset();
    out_s("DEADLOCK: ");
    out_thread(current_thread);
    out_s(" locking ");
    out_mutex(wanted);
    out_s(" from ");
    out_hex(caller);
    out_s(" would close this cycle:\n  ");

    // current -> [wanted] -> owner -> [what it waits on] -> ... -> current
    out_thread(current_thread);
    const mutex_t* m = wanted;
    for (int i = 0; i < WD_MAX_CHAIN && m != NULL; i++) {
        out_s(" -> [");
        out_mutex(m);
        out_s("] -> ");
        ThreadBlock* owner = m->owner;
        out_thread(owner);
        if (owner == NULL || owner == current_thread) break;
        m = owner->wait_mutex;
    }
    out_s("\n\nEvery thread in the cycle waits on the next one, so none can ever run.\n"
          "Take these locks in one fixed order everywhere. The locking call:\n");
    out_backtrace_here();
    out_c('\n');

    wd_body(timer_get_ms(), NULL, current_thread);
    wd_panic(0, caller);
}
