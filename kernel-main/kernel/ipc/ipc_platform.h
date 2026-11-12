#ifndef _KERNEL_IPC_PLATFORM_H_
#define _KERNEL_IPC_PLATFORM_H_

#include <stdint.h>
#include <stdbool.h>
#include <kernel/syscall/syscall.h>

// What the IPC core (ipc.c) needs from the rest of the kernel
//
// ipc.c itself touches no hardware, scheduler or page table: everything
// goes through these calls, implemented for the kernel in ipc_x86.c. The
// libc host tests build the very same ipc.c against an emulation of them
// (libc/test/ipc_shim.c).
//
// `space` is the caller's address space, passed through untouched.

// The one IPC lock. Thread context; may sleep.
void ipc_plat_lock(void);
void ipc_plat_unlock(void);

// Copy to / from user memory, checking that the range is user memory,
// mapped and (for writes) writable. False on any bad byte.
bool ipc_plat_user_read(void* space, void* dst, uint32_t user_addr, uint32_t length);
bool ipc_plat_user_write(void* space, uint32_t user_addr, const void* src, uint32_t length);

// Page-aligned, zeroed kernel memory for shared blocks. NULL if out of memory.
void* ipc_plat_alloc_pages(uint32_t pages);
void  ipc_plat_free_pages(void* kva, uint32_t pages);

// Map the kernel pages [kva, kva + pages) into `space`, read-write for the
// user, at *addr (page aligned, proposed by the core). An implementation may
// place it elsewhere and store the address it used in *addr. All or nothing.
bool ipc_plat_map(void* space, void* kva, uint32_t pages, uint32_t* addr);
void ipc_plat_unmap(void* space, uint32_t addr, uint32_t pages);

// Where the core places mappings: [IPC_MAP_BASE, IPC_MAP_END)
#define IPC_MAP_BASE    0x80000000u
#define IPC_MAP_END     0xBF000000u

// Milliseconds since boot
uint32_t ipc_plat_time_ms(void);

// Give up the CPU for about a millisecond (teardown waits)
void ipc_plat_sleep_ms(uint32_t ms);

//
// Sleeping until IPC state changes
//
// A sleeper is registered (begin) BEFORE the caller checks whether it has
// anything to do, so a wake that lands between that check and the sleep is
// remembered and the sleep returns at once. wake_all() may be called from
// interrupt handlers.
//
typedef struct {
    uint32_t opaque[8];
} IpcSleeper;

void ipc_plat_sleep_begin(IpcSleeper* s);
void ipc_plat_sleep(IpcSleeper* s, uint32_t timeout_ms);   // SYS_IPC_WAIT_FOREVER = no timeout
void ipc_plat_sleep_end(IpcSleeper* s);
void ipc_plat_wake_all(void);

// True when the calling process is being killed: a wait gives up at once
// (the kernel ends the process as the call returns)
bool ipc_plat_cancelled(void);

//
// Input devices
//

// Fill in the current mouse position, buttons and modifier keys (type,
// key and time are left to the caller)
void ipc_plat_input_state(SysInputEvent* ev);

// Take the next queued event (mouse events first). False when none.
bool ipc_plat_input_pop(SysInputEvent* ev);
bool ipc_plat_input_pending(void);

// Drop everything queued
void ipc_plat_input_flush(void);

// The grab was taken (true) or let go (false). Called with the IPC lock held.
void ipc_plat_input_grab_changed(bool grabbed);

#endif
