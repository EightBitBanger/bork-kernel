#ifndef _KERNEL_ELEVATE_H_
#define _KERNEL_ELEVATE_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>

//
// Elevation: a user program asks the user for SYS_SEC_ADMIN
//
// The user ABI is the "Elevation" section of syscall/syscall.h; the syscall
// handlers are in syscall/calls/sys_elevate.h.
//
//   requester   SYS_ELEVATE: elevate_request() queues a request and blocks
//               the calling thread until it is answered, given up on, or the
//               process is killed
//   agent       the desktop (one process, registered): elevate_agent_next()
//               hands it the oldest request to show, elevate_agent_answer()
//               takes the user's choice
//
// Only the kernel and the agent ever touch a request. The program asking
// gets nothing it could answer with: no handle, no id. Its name and path
// come from the process table, never from the program. The grant itself is
// made by elevate_request() on the requester's own thread, after the
// answer, so an answer for a process that has since ended (or a pid that
// was reused: ids carry a generation) grants nothing.
//
// All state is guarded by turning interrupts off (short sections only).
//

#define ELEVATE_QUEUE_MAX   4       // Requests waiting at once, system wide

// SYS_ELEVATE for the calling process. `reason` (kernel memory, may be NULL)
// is copied and cleaned. Blocks; thread context. SYS_OK or a SYS_E* code.
int32_t elevate_request(const char* reason);

// SYS_ELEVATE_AGENT. Each checks that the caller is the agent (REGISTER
// makes it one). Thread context.
int32_t elevate_agent_register(void);
int32_t elevate_agent_next(SysElevateRequest* out);    // 1 (filled in) or 0
int32_t elevate_agent_status(uint32_t id);             // 1 waiting, 0 over
int32_t elevate_agent_answer(uint32_t id, bool allow);

// True for the registered agent's pid
bool elevate_is_agent(int32_t pid);

// A request is waiting and none is being shown: the agent's input handle
// reads as ready (ipc_x86.c), so the desktop wakes up for it. Interrupt safe.
bool elevate_agent_has_work(void);

// A process is going away (syscall_process_exit): if it was the agent,
// every waiting request fails with SYS_EAGAIN; its own requests are dropped.
void elevate_process_exit(int32_t pid);

#endif
