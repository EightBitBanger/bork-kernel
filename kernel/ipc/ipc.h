#ifndef _KERNEL_IPC_H_
#define _KERNEL_IPC_H_

#include <stdint.h>
#include <stdbool.h>

//
// Inter-process communication: pipes, named ports, shared memory blocks,
// and the input device grab. The user ABI is in syscall/syscall.h (the
// "IPC" and "Input" sections); the syscall handlers in
// syscall/calls/sys_ipc.h call the ipc_sys_* functions below.
//
// Every object is reference counted: each handle, and each message that
// carries the object, holds one reference. All state is guarded by one lock
// (ipc_plat_lock); nothing here is called from interrupt handlers except
// ipc_input_irq().
//

// Who is making the call
typedef struct {
    uint32_t pid;
    void*    space;         // AddressSpace*, for user copies and mappings
    uint32_t security;      // SYS_SEC_* mask
} IpcCaller;

int32_t ipc_sys_close(const IpcCaller* c, uint32_t handle);
int32_t ipc_sys_pipe(const IpcCaller* c, uint32_t user_out);
int32_t ipc_sys_port_create(const IpcCaller* c, uint32_t user_name);
int32_t ipc_sys_connect(const IpcCaller* c, uint32_t user_name);
int32_t ipc_sys_accept(const IpcCaller* c, uint32_t port);
int32_t ipc_sys_send(const IpcCaller* c, uint32_t pipe, uint32_t user_buf, uint32_t length, uint32_t attach);
int32_t ipc_sys_recv(const IpcCaller* c, uint32_t pipe, uint32_t user_buf, uint32_t size, uint32_t user_info);
int32_t ipc_sys_wait(const IpcCaller* c, uint32_t user_items, uint32_t count, uint32_t timeout_ms);
int32_t ipc_sys_shm_create(const IpcCaller* c, uint32_t size);
int32_t ipc_sys_shm_map(const IpcCaller* c, uint32_t handle);
int32_t ipc_sys_shm_unmap(const IpcCaller* c, uint32_t handle);
int32_t ipc_sys_info(const IpcCaller* c, uint32_t handle, uint32_t user_info);
int32_t ipc_sys_input_open(const IpcCaller* c);
int32_t ipc_sys_input_read(const IpcCaller* c, uint32_t handle, uint32_t user_events, uint32_t max);

// Close every handle of a process (unmapping its shared memory) and drop
// its table. Wakes and waits for (up to IPC_EXIT_WAIT_MS) any of its
// threads still inside an IPC call. Thread context, while the address
// space still exists.
void ipc_process_exit(uint32_t pid);

// True while a process holds the keyboard and mouse (SYS_INPUT_OPEN). The
// kernel console stops reading input while this is set.
bool ipc_input_grabbed(void);

// Keyboard / mouse IRQ: new input queued. Wakes waiters while grabbed.
void ipc_input_irq(void);

#define IPC_EXIT_WAIT_MS    1000

#endif

