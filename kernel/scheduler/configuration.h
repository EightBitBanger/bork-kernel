#ifndef _SCHEDULER_CONFIG_H_
#define _SCHEDULER_CONFIG_H_

// Degradation: every time a thread burns SCHED_DEGRADE_QUANTA full quanta
// of CPU (counted across preemptions and yields, not reset by a short
// block), its effective priority drops one level, down to PRIORITY_IDLE.
#define SCHED_DEGRADE_QUANTA      2

// Recovery: blocking/sleeping wins one level back and halves the usage
// counter, and so does every SCHED_RECOVER_MS spent off the CPU. Threads
// never recover above their base priority.
#define SCHED_RECOVER_MS          20

// Starvation boost: a thread that has been READY for SCHED_STARVATION_MS
// without getting the CPU runs at PRIORITY_REALTIME for one quantum.
// This bounds the wait of every runnable thread, whatever the load.
#define SCHED_STARVATION_MS       100

// Priority degradation to prevent starvation
#define SCHED_DEGRADE_REALTIME    0

#endif
