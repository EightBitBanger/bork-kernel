#ifndef _KERNEL_SCHED_WATCHDOG_H_
#define _KERNEL_SCHED_WATCHDOG_H_

// Scheduler watchdog
//
// Four detectors, all ending in the PT_WATCHDOG panic screen (and a COM1
// mirror of the report when a UART is present):
//
//   Deadlock at formation   mutex_lock walks owner -> wait_mutex -> owner
//                           before blocking. If the chain leads back to the
//                           caller, the cycle is reported the instant it
//                           would form (wd_note_deadlock). Exact: the wait
//                           graph only changes with IRQs off on one CPU.
//                           Works for every mutex, registered or not.
//
//   Long mutex waits        Any thread BLOCKED on a mutex longer than the
//                           lock's limits gets a serial report, then a
//                           panic (lock holder spinning, sleeping forever,
//                           dead, or waiting on a lost wakeup).
//
//   Thread heartbeats       wd_watch() a long-lived thread, call wd_pet()
//                           from its main loop. It is stalled when it stops
//                           petting while NOT parked in an event wait or a
//                           sleep (parked threads are idle, not frozen).
//
//   Hard lockups (NMI)      wd_nmi_init() routes the RTC periodic interrupt
//                           through the IOAPIC as an NMI at 2 Hz. If the
//                           scheduler has not run for WD_NMI_STRIKES NMIs
//                           (interrupts left off, endless IRQ handler,
//                           lost timer EOI) the NMI handler panics.
//
// Thresholds are compile-time defaults (see watchdog.c) plus per-thread and
// per-lock values given at registration.

#include <stdint.h>
#include <stdbool.h>
#include <kernel/mutex.h>
#include <kernel/scheduler/scheduler.h>

// Pass as a limit to turn that stage off
#define WD_NEVER          0

void wd_init(void);

// ---- Heartbeats ----

// Watch a thread: serial report after report_ms without a wd_pet(), panic
// after panic_ms (WD_NEVER for either turns that stage off). Watching starts
// with the thread's first wd_pet(). Also names the thread in reports.
bool wd_watch(ThreadBlock* thread, const char* name, uint32_t report_ms, uint32_t panic_ms);
void wd_unwatch(ThreadBlock* thread);

// Called by a watched thread once per loop iteration / unit of work
void wd_pet(void);

// Old name, kept for existing callers
static inline void wd_heartbeat(void) { wd_pet(); }

// ---- Names and locks ----

// Label a thread in reports
void wd_name_thread(ThreadBlock* thread, const char* name);

// Name a mutex so reports can show it by name. Unregistered mutexes are still
// fully covered (deadlocks and long waits), they just show as an address.
void wd_register_lock(const char* name, mutex_t* mutex);

// Override the long-wait limits for one mutex (e.g. a VFS lock that may be
// held across slow disk operations). WD_NEVER turns a stage off.
void wd_set_lock_limits(mutex_t* mutex, uint32_t report_ms, uint32_t panic_ms);

// ---- Hooks (called by the scheduler and mutex code) ----

// thread_handler_c, right after the outgoing thread's esp is saved
void wd_tick(uint64_t now_ms);

// Scheduler reaper, right before a dead ThreadBlock is freed
void wd_forget_thread(ThreadBlock* thread);

// mutex_lock: the caller already owns the mutex. Never returns.
void wd_note_self_lock(const mutex_t* mutex, uint32_t caller);

// mutex_lock: blocking on `wanted` would close a wait cycle. Never returns.
void wd_note_deadlock(const mutex_t* wanted, uint32_t caller);

// ---- Hard-lockup NMI ----

// Call once after the scheduler is running and the display is set up, before
// any process is created. Returns false when the hardware can't do it (no
// local APIC / IOAPIC); the other detectors still work.
bool wd_nmi_init(void);

#endif

