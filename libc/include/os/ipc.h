#ifndef _OS_IPC_H
#define _OS_IPC_H

/*
 * Inter-process communication: message pipes, named ports and shared
 * memory blocks. Needs SEC_IPC (os/security.h); ipc_close() and ipc_info()
 * always work.
 *
 * Everything is reached through handles (small ints, their own numbering,
 * separate from file descriptors). They are closed when the process exits.
 *
 * Pipes carry whole messages (1 .. IPC_MSG_MAX bytes) both ways, in order.
 * The plain calls never block; the *_wait() helpers below block until they
 * can complete. A message may carry one handle (a pipe end or a shared
 * memory block): the receiver gets its own handle to the same object.
 *
 *     // Server                              // Client
 *     int port = ipc_port_create("echo");    int p = ipc_connect("echo");
 *     int c = ipc_accept_wait(port);         ipc_send_wait(p, "hi", 2, 0);
 *     char buf[IPC_MSG_MAX];                 ipc_recv_wait(p, buf, sizeof(buf), NULL);
 *     ssize_t n = ipc_recv_wait(c, buf, sizeof(buf), NULL);
 *     ipc_send_wait(c, buf, n, 0);
 *
 * Shared memory: shm_create(size) makes a zeroed block, shm_map() maps it
 * into this process. Send the handle in a message so another process can
 * map the same memory.
 *
 * Errors: -1 with errno.
 *   EAGAIN  nothing to receive / accept, or the peer's queue is full
 *   EPIPE   the other end is closed (send), or closed and drained (recv)
 *   ERANGE  recv buffer too small (the message stays queued)
 *   EBADF   not an open handle of the right kind
 *   EBUSY   sending a pipe end that has pipe ends queued for it
 *   EMFILE  no free handle
 *   ENOENT  no port by that name;  EEXIST  name taken
 *   EPERM   no SEC_IPC (or "sys.*" port name without SEC_ADMIN)
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <os/syscall.h>

#define IPC_HANDLE_MAX      SYS_IPC_HANDLE_MAX
#define IPC_MSG_MAX         SYS_IPC_MSG_MAX
#define IPC_QUEUE_MAX       SYS_IPC_QUEUE_MAX
#define IPC_NAME_MAX        SYS_IPC_NAME_MAX      // Port name bytes, terminator included
#define IPC_WAIT_MAX        SYS_IPC_WAIT_MAX
#define SHM_MAX             SYS_SHM_MAX

#define IPC_FOREVER         (-1)                  // Timeout: wait as long as it takes

#define IPC_READABLE        SYS_IPC_READABLE
#define IPC_WRITABLE        SYS_IPC_WRITABLE
#define IPC_HANGUP          SYS_IPC_HANGUP

#define IPC_TYPE_PIPE       SYS_IPC_TYPE_PIPE
#define IPC_TYPE_PORT       SYS_IPC_TYPE_PORT
#define IPC_TYPE_SHM        SYS_IPC_TYPE_SHM
#define IPC_TYPE_INPUT      SYS_IPC_TYPE_INPUT

typedef SysIpcMsgInfo  ipc_msg_info_t;     // length, handle (0 = none), sender pid
typedef SysIpcWaitItem ipc_wait_item_t;    // handle, events (in), ready (out)
typedef SysIpcInfo     ipc_info_t;

int ipc_close(int handle);

// Two connected pipe ends in out[0] and out[1]
int ipc_pipe(int out[2]);

// Ports. Names: 1 .. IPC_NAME_MAX - 1 characters of [A-Za-z0-9._-].
int ipc_port_create(const char* name);     // Port handle
int ipc_connect(const char* name);         // Pipe handle (EAGAIN: port's backlog full)
int ipc_accept(int port);                  // Pipe handle, or -1 / EAGAIN

// Send `length` bytes, optionally with a handle (0 for none)
int ipc_send(int pipe, const void* buf, size_t length, int attach);

// Receive the next message into buf. Returns its length. `info` (may be
// NULL) gets the length, the handle that came with it and the sender.
ssize_t ipc_recv(int pipe, void* buf, size_t size, ipc_msg_info_t* info);

// Wait for any of the items. Returns how many are ready (their `ready` set),
// 0 on timeout (0 ms: just look). timeout_ms < 0 waits forever.
int ipc_wait(ipc_wait_item_t* items, size_t count, int32_t timeout_ms);

// One handle: its ready bits (IPC_READABLE ...), 0 on timeout, -1 on error
int ipc_wait_one(int handle, unsigned events, int32_t timeout_ms);

// Blocking versions: wait until the call can complete (or fails for good)
int     ipc_accept_wait(int port);
int     ipc_send_wait(int pipe, const void* buf, size_t length, int attach);
ssize_t ipc_recv_wait(int pipe, void* buf, size_t size, ipc_msg_info_t* info);

int ipc_info(int handle, ipc_info_t* out);

// Shared memory
int   shm_create(size_t size);             // Handle; size rounds up to whole pages
void* shm_map(int handle);                 // NULL on error; same address if already mapped
int   shm_unmap(int handle);
size_t shm_size(int handle);               // Bytes, 0 on error

#endif

