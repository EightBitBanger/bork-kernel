/*
 * IPC and input tests: os/ipc.h and os/input.h through the kernel's own
 * ipc/ipc.c (ipc_shim.c emulates the platform under it). Linked like a
 * real user program.
 *
 * Several processes are emulated in one: __test_ipc_set_pid() chooses
 * which process id the next calls come from (0 = this one).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/ipc.h>
#include <os/input.h>
#include <os/process.h>
#include <os/security.h>
#include <sys/ioctl.h>
#include <unistd.h>

int malloc_check(void);

void     __test_ipc_set_pid(uint32_t pid);
void     __test_ipc_set_sleep_hook(void (*fn)(void));
uint32_t __test_ipc_now(void);
uint32_t __test_ipc_grab_changes(void);
bool     __test_ipc_grabbed(void);
void     __test_ipc_exit(uint32_t pid);
uint32_t __test_ipc_pages_live(void);
uint32_t __test_ipc_maps_live(void);
uint32_t __test_ipc_allocs_live(void);
void     __test_input_set_state(int16_t x, int16_t y, uint8_t buttons, uint8_t mods);
bool     __test_input_push(const input_event_t* ev);
uint32_t __test_input_queued(void);
int32_t  __test_tty_attach(uint32_t child, uint32_t handle);
int32_t  __test_stdio_read(void* buf, uint32_t size);
int32_t  __test_stdio_write(const void* buf, uint32_t length);

static int failures = 0, checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); } \
} while (0)

#define PID_A 101
#define PID_B 102
#define PID_C 103

static void as(uint32_t pid) { __test_ipc_set_pid(pid); }

// Every handle this process has open (closes them all as a side effect)
static int close_all(void) {
    int n = 0;
    for (int h = 1; h <= IPC_HANDLE_MAX; h++)
        n += (ipc_close(h) == 0);
    return n;
}

static void test_pipe_basics(void) {
    int p[2];
    char buf[IPC_MSG_MAX + 8];
    ipc_msg_info_t info;
    ipc_info_t ii;

    CHECK(ipc_pipe(p) == 0 && p[0] >= 1 && p[1] >= 1 && p[0] != p[1]);
    CHECK(ipc_recv(p[1], buf, sizeof(buf), NULL) == -1 && errno == EAGAIN);

    CHECK(ipc_send(p[0], "hello", 5, 0) == 0);
    CHECK(ipc_send(p[0], "world!", 6, 0) == 0);
    CHECK(ipc_send(p[1], "back", 4, 0) == 0);              // Both directions
    CHECK(ipc_info(p[1], &ii) == 0 && ii.type == IPC_TYPE_PIPE && ii.pending == 2 && ii.size == 11 && ii.flags == 0);

    // Too small: the message stays and its length is reported
    memset(&info, 0xFF, sizeof(info));
    CHECK(ipc_recv(p[1], buf, 4, &info) == -1 && errno == ERANGE && info.length == 5);
    memset(&info, 0xFF, sizeof(info));
    CHECK(ipc_recv(p[1], buf, sizeof(buf), &info) == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(info.length == 5 && info.handle == 0 && info.sender == (uint32_t)getpid());
    CHECK(ipc_recv(p[1], buf, 6, NULL) == 6 && memcmp(buf, "world!", 6) == 0);     // Exact fit
    CHECK(ipc_recv(p[1], buf, sizeof(buf), NULL) == -1 && errno == EAGAIN);
    CHECK(ipc_recv(p[0], buf, sizeof(buf), NULL) == 4 && memcmp(buf, "back", 4) == 0);

    // Sizes
    memset(buf, 'x', sizeof(buf));
    CHECK(ipc_send(p[0], buf, 0, 0) == -1 && errno == EINVAL);
    CHECK(ipc_send(p[0], buf, IPC_MSG_MAX + 1, 0) == -1 && errno == EINVAL);
    CHECK(ipc_send(p[0], buf, IPC_MSG_MAX, 0) == 0);
    CHECK(ipc_recv(p[1], buf, sizeof(buf), NULL) == IPC_MSG_MAX);

    // Bad handles and pointers
    CHECK(ipc_send(0, "x", 1, 0) == -1 && errno == EBADF);
    CHECK(ipc_send(IPC_HANDLE_MAX + 1, "x", 1, 0) == -1 && errno == EBADF);
    CHECK(ipc_send(p[0], (void*)16, 1, 0) == -1 && errno == EFAULT);
    CHECK(ipc_send(p[0], "x", 1, 0) == 0);
    CHECK(ipc_recv(p[1], (void*)16, 8, NULL) == -1 && errno == EFAULT);
    CHECK(ipc_recv(p[1], buf, 8, (void*)16) == -1 && errno == EFAULT);
    CHECK(ipc_recv(p[1], buf, 8, NULL) == 1);              // Still there after the faults
    CHECK(ipc_pipe((int*)0) == -1 && errno == EFAULT);

    // Queue limit and WRITABLE
    int sent = 0;
    while (ipc_send(p[0], "q", 1, 0) == 0)
        sent++;
    CHECK(sent == IPC_QUEUE_MAX && errno == EAGAIN);
    CHECK(ipc_wait_one(p[0], IPC_WRITABLE, 0) == 0);
    CHECK(ipc_wait_one(p[1], IPC_READABLE | IPC_WRITABLE, 0) == (IPC_READABLE | IPC_WRITABLE));
    CHECK(ipc_recv(p[1], buf, sizeof(buf), NULL) == 1);
    CHECK(ipc_wait_one(p[0], IPC_WRITABLE, 0) == IPC_WRITABLE);

    // Hangup: the reader drains what is queued, then sees the end
    CHECK(ipc_close(p[0]) == 0);
    CHECK(ipc_close(p[0]) == -1 && errno == EBADF);
    CHECK(ipc_send(p[1], "x", 1, 0) == -1 && errno == EPIPE);
    CHECK(ipc_info(p[1], &ii) == 0 && (ii.flags & SYS_IPC_INFO_PEER_CLOSED) && ii.pending == IPC_QUEUE_MAX - 1);
    CHECK(ipc_wait_one(p[1], IPC_READABLE, 0) == (IPC_READABLE | IPC_HANGUP));
    CHECK(ipc_wait_one(p[1], 0, 0) == IPC_HANGUP);         // Reported even unasked
    int drained = 0;
    while (ipc_recv(p[1], buf, sizeof(buf), NULL) == 1)
        drained++;
    CHECK(drained == IPC_QUEUE_MAX - 1 && errno == EPIPE);
    CHECK(ipc_close(p[1]) == 0);

    // Messages queued for an end that closes are dropped with it
    CHECK(ipc_pipe(p) == 0);
    CHECK(ipc_send(p[0], "lost", 4, 0) == 0);
    CHECK(ipc_close(p[1]) == 0);
    CHECK(ipc_send(p[0], "x", 1, 0) == -1 && errno == EPIPE);
    CHECK(ipc_recv(p[0], buf, sizeof(buf), NULL) == -1 && errno == EPIPE);
    CHECK(ipc_close(p[0]) == 0);

    // Handle table full
    int got = 0, last[2] = { 0, 0 };
    while (ipc_pipe(p) == 0) {
        got += 2;
        last[0] = p[0];
        last[1] = p[1];
    }
    CHECK(got == IPC_HANDLE_MAX && errno == EMFILE);
    CHECK(ipc_close(last[0]) == 0);
    CHECK(ipc_pipe(p) == -1 && errno == EMFILE);           // Needs two
    CHECK(close_all() == IPC_HANDLE_MAX - 1);
}

static void test_shm(void) {
    CHECK(shm_create(0) == -1 && errno == EINVAL);
    CHECK(shm_create(SHM_MAX + 1) == -1 && errno == EINVAL);

    int s = shm_create(5000);
    CHECK(s > 0 && shm_size(s) == 8192 && __test_ipc_pages_live() == 2);

    ipc_info_t ii;
    CHECK(ipc_info(s, &ii) == 0 && ii.type == IPC_TYPE_SHM && ii.address == 0 && !(ii.flags & SYS_IPC_INFO_MAPPED));
    CHECK(shm_unmap(s) == -1 && errno == EINVAL);

    uint8_t* m = shm_map(s);
    CHECK(m != NULL && shm_map(s) == m);                   // Already mapped: same address
    CHECK(ipc_info(s, &ii) == 0 && ii.address == (uint32_t)(uintptr_t)m && (ii.flags & SYS_IPC_INFO_MAPPED));
    bool zero = true;
    for (int i = 0; i < 8192; i++) zero = zero && m[i] == 0;
    CHECK(zero);
    strcpy((char*)m + 4096, "shared");
    CHECK(shm_unmap(s) == 0 && __test_ipc_maps_live() == 0);
    m = shm_map(s);
    CHECK(m != NULL && strcmp((char*)m + 4096, "shared") == 0);  // Unmapping keeps the contents

    // Not a shm handle
    int p[2];
    CHECK(ipc_pipe(p) == 0);
    CHECK(shm_map(p[0]) == NULL && errno == EBADF);
    CHECK(shm_size(p[0]) == 0);

    // Total limit
    int big[8], n = 0;
    while (n < 8 && (big[n] = shm_create(SHM_MAX)) > 0)
        n++;
    CHECK(n == 3 && errno == ENOMEM);                      // 64 MB total, 8 KB already used
    for (int i = 0; i < n; i++)
        CHECK(ipc_close(big[i]) == 0);
    CHECK(__test_ipc_pages_live() == 2);

    // Closing a mapped handle unmaps it; the memory goes with the last handle
    CHECK(ipc_close(s) == 0 && __test_ipc_maps_live() == 0 && __test_ipc_pages_live() == 0);
    CHECK(close_all() == 2);
}

static void test_handle_passing(void) {
    int p[2], q[2];
    char buf[64];
    ipc_msg_info_t info;

    CHECK(ipc_pipe(p) == 0);
    int s = shm_create(4096);
    char* a = shm_map(s);
    CHECK(a != NULL);
    strcpy(a, "from the sender");

    CHECK(ipc_send(p[0], "blk", 3, s) == 0);
    CHECK(ipc_close(s) == 0 && __test_ipc_pages_live() == 1);  // The message keeps it alive

    memset(&info, 0, sizeof(info));
    CHECK(ipc_recv(p[1], buf, sizeof(buf), &info) == 3 && info.handle > 0);
    int s2 = info.handle;
    char* b = shm_map(s2);
    CHECK(b != NULL && strcmp(b, "from the sender") == 0);
    CHECK(ipc_close(s2) == 0 && __test_ipc_pages_live() == 0);

    // Pipe ends travel too
    CHECK(ipc_pipe(q) == 0);
    CHECK(ipc_send(p[0], "end", 3, q[1]) == 0);
    CHECK(ipc_close(q[1]) == 0);
    CHECK(ipc_recv(p[1], buf, sizeof(buf), &info) == 3 && info.handle > 0);
    int q1 = info.handle;
    CHECK(ipc_send(q[0], "through", 7, 0) == 0);
    CHECK(ipc_recv(q1, buf, sizeof(buf), NULL) == 7 && memcmp(buf, "through", 7) == 0);

    // Not allowed: an end through its own pipe, a port, an input handle, a bad handle
    CHECK(ipc_send(p[0], "x", 1, p[1]) == -1 && errno == EINVAL);
    CHECK(ipc_send(p[0], "x", 1, p[0]) == -1 && errno == EINVAL);
    int port = ipc_port_create("pass.test");
    CHECK(port > 0 && ipc_send(p[0], "x", 1, port) == -1 && errno == EINVAL);
    CHECK(ipc_send(p[0], "x", 1, 60) == -1 && errno == EBADF);
    CHECK(ipc_close(port) == 0);

    // Receiver out of handles: EMFILE and the message stays
    s = shm_create(4096);
    CHECK(ipc_send(p[0], "late", 4, s) == 0);
    int fill[IPC_HANDLE_MAX], nfill = 0;
    while ((fill[nfill] = shm_create(4096)) > 0)
        nfill++;
    CHECK(errno == EMFILE);
    CHECK(ipc_recv(p[1], buf, sizeof(buf), &info) == -1 && errno == EMFILE);
    CHECK(ipc_close(fill[0]) == 0);
    CHECK(ipc_recv(p[1], buf, sizeof(buf), &info) == 4 && info.handle == fill[0]);
    for (int i = 1; i < nfill; i++)
        ipc_close(fill[i]);

    // A message carrying the last reference, dropped unread, frees it
    CHECK(close_all() > 0);
    CHECK(ipc_pipe(p) == 0);
    s = shm_create(4096 * 3);
    CHECK(ipc_send(p[0], "drop", 4, s) == 0 && ipc_close(s) == 0);
    CHECK(__test_ipc_pages_live() == 3);
    CHECK(ipc_close(p[1]) == 0 && __test_ipc_pages_live() == 0);

    // Nesting: shm inside A, A's reader inside B. B's reader now holds an
    // end, so it can't travel itself (EBUSY). Closing it frees all of it.
    int A[2], B[2];
    CHECK(ipc_pipe(A) == 0 && ipc_pipe(B) == 0);
    s = shm_create(4096);
    CHECK(ipc_send(A[0], "s", 1, s) == 0 && ipc_close(s) == 0);
    CHECK(ipc_send(B[0], "a", 1, A[1]) == 0 && ipc_close(A[1]) == 0 && ipc_close(A[0]) == 0);
    int C[2];
    CHECK(ipc_pipe(C) == 0);
    CHECK(ipc_send(C[0], "b", 1, B[1]) == -1 && errno == EBUSY);
    CHECK(ipc_close(B[0]) == 0 && __test_ipc_pages_live() == 1);
    CHECK(ipc_close(B[1]) == 0 && __test_ipc_pages_live() == 0);
    CHECK(close_all() == 3);

    // Loops are refused: once B[1] holds A[1], A[1] can't take B[1] in
    CHECK(ipc_pipe(A) == 0 && ipc_pipe(B) == 0);
    CHECK(ipc_send(B[0], "a", 1, A[1]) == 0);              // B[1] holds A[1]
    CHECK(ipc_send(A[0], "b", 1, B[1]) == -1 && errno == EBUSY);   // A[1] would hold B[1]
    CHECK(ipc_send(A[0], "b", 1, B[0]) == 0);              // B[0] holds nothing: fine
    CHECK(ipc_recv(B[1], buf, sizeof(buf), &info) == 1 && info.handle > 0);
    CHECK(close_all() > 0);
    s = shm_create(4096);
    CHECK(s > 0 && close_all() == 1 && __test_ipc_pages_live() == 0);
}

static void test_ports(void) {
    char buf[64];
    ipc_msg_info_t info;

    // Names
    CHECK(ipc_port_create("") == -1 && errno == EINVAL);
    CHECK(ipc_port_create("bad/name") == -1 && errno == EINVAL);
    CHECK(ipc_port_create("bad name") == -1 && errno == EINVAL);
    CHECK(ipc_port_create("0123456789012345678901234567890") > 0);     // 31: fits
    CHECK(ipc_port_create("01234567890123456789012345678901") == -1 && errno == ENAMETOOLONG);
    CHECK(ipc_port_create((const char*)16) == -1 && errno == EFAULT);
    CHECK(close_all() == 1);

    // Server A, clients B and C
    as(PID_A);
    int port = ipc_port_create("echo");
    CHECK(port > 0);
    CHECK(ipc_port_create("echo") == -1 && errno == EEXIST);
    CHECK(ipc_accept(port) == -1 && errno == EAGAIN);
    CHECK(ipc_wait_one(port, IPC_READABLE, 0) == 0);

    as(PID_B);
    CHECK(ipc_connect("nope") == -1 && errno == ENOENT);
    int cb = ipc_connect("echo");
    CHECK(cb > 0 && ipc_send(cb, "from B", 6, 0) == 0);
    CHECK(ipc_accept(port) == -1 && errno == EBADF);       // Not B's handle
    as(PID_C);
    int cc = ipc_connect("echo");
    CHECK(cc > 0 && ipc_send(cc, "from C", 6, 0) == 0);

    as(PID_A);
    ipc_info_t ii;
    CHECK(ipc_info(port, &ii) == 0 && ii.type == IPC_TYPE_PORT && ii.pending == 2);
    CHECK(ipc_wait_one(port, IPC_READABLE, 0) == IPC_READABLE);
    int sb = ipc_accept(port), sc = ipc_accept(port);       // Oldest first
    CHECK(sb > 0 && sc > 0);
    CHECK(ipc_recv(sb, buf, sizeof(buf), &info) == 6 && memcmp(buf, "from B", 6) == 0 && info.sender == PID_B);
    CHECK(ipc_recv(sc, buf, sizeof(buf), &info) == 6 && memcmp(buf, "from C", 6) == 0 && info.sender == PID_C);
    CHECK(ipc_send(sb, "to B", 4, 0) == 0);

    as(PID_B);
    CHECK(ipc_recv(cb, buf, sizeof(buf), &info) == 4 && info.sender == PID_A);

    // Backlog limit
    int pending[SYS_IPC_BACKLOG + 1], n = 0;
    while ((pending[n] = ipc_connect("echo")) > 0)
        n++;
    CHECK(n == SYS_IPC_BACKLOG && errno == EAGAIN);

    // Closing the port drops the name and every queued connection
    as(PID_A);
    CHECK(ipc_close(port) == 0);
    as(PID_B);
    CHECK(ipc_send(pending[0], "x", 1, 0) == -1 && errno == EPIPE);
    CHECK(ipc_connect("echo") == -1 && errno == ENOENT);
    CHECK(ipc_send(cb, "x", 1, 0) == 0);                   // Accepted pipes live on
    as(PID_A);
    CHECK(ipc_port_create("echo") > 0);                    // Name free again

    // Process exit closes everything: A's server ends hang up
    as(PID_B);
    __test_ipc_exit(PID_B);
    as(PID_A);
    CHECK(ipc_wait_one(sb, IPC_READABLE, 0) == (IPC_READABLE | IPC_HANGUP));
    CHECK(ipc_recv(sb, buf, sizeof(buf), NULL) == 1);      // B's last message still readable
    CHECK(ipc_recv(sb, buf, sizeof(buf), NULL) == -1 && errno == EPIPE);
    __test_ipc_exit(PID_A);
    as(PID_C);
    CHECK(ipc_send(cc, "x", 1, 0) == -1 && errno == EPIPE);
    as(PID_B);
    CHECK(ipc_close(cb) == -1 && errno == EBADF);          // A fresh table for "B"
    __test_ipc_exit(PID_B);
    __test_ipc_exit(PID_C);
    __test_ipc_exit(PID_A);
    as(0);

    CHECK(ipc_port_create("sys.thing") > 0);               // We are admin
    CHECK(close_all() == 1);
}

// Last: an admin can't get SEC_ADMIN back once dropped
static void test_no_admin(void) {
    CHECK(sec_set(SEC_ALL & ~SEC_ADMIN) == 0);
    CHECK(ipc_port_create("sys.thing") == -1 && errno == EPERM);
    CHECK(ipc_port_create("sysx.thing") > 0);
    CHECK(ipc_connect("sys.nothing") == -1 && errno == ENOENT);  // Connecting needs no admin
    CHECK(close_all() == 1);
}

// Sleep hooks: run "while the waiter sleeps"
static int hook_pipe;
static void hook_send(void) {
    as(PID_B);
    ipc_send(hook_pipe, "wake", 4, 0);
}
static void hook_mouse(void) {
    input_event_t ev = { INPUT_MOUSE, 0, 200, 150, INPUT_BTN_LEFT, 0, 0, 7 };
    __test_input_push(&ev);
}
static void hook_exit_b(void) {
    __test_ipc_exit(PID_B);
}
static void hook_connect(void) {
    as(PID_C);
    int c = ipc_connect("waiter");
    ipc_send(c, "hello", 5, 0);
}

static void test_wait(void) {
    as(PID_A);
    int port = ipc_port_create("waiter");
    as(PID_B);
    hook_pipe = ipc_connect("waiter");
    as(PID_A);
    int sp = ipc_accept(port);
    CHECK(port > 0 && hook_pipe > 0 && sp > 0);

    // Timeout and poll
    uint32_t t0 = __test_ipc_now();
    CHECK(ipc_wait_one(sp, IPC_READABLE, 50) == 0 && __test_ipc_now() - t0 >= 50);
    t0 = __test_ipc_now();
    CHECK(ipc_wait_one(sp, IPC_READABLE, 0) == 0 && __test_ipc_now() == t0);

    // Woken by another process's send, no timeout
    __test_ipc_set_sleep_hook(hook_send);
    CHECK(ipc_wait_one(sp, IPC_READABLE, IPC_FOREVER) == IPC_READABLE);
    char buf[16];
    CHECK(ipc_recv_wait(sp, buf, sizeof(buf), NULL) == 4);  // Already queued: no sleep
    __test_ipc_set_sleep_hook(hook_send);
    CHECK(ipc_recv_wait(sp, buf, sizeof(buf), NULL) == 4);  // Sleeps until the hook sends

    // Several items: only the ready ones are marked
    int p[2];
    CHECK(ipc_pipe(p) == 0 && ipc_send(p[0], "x", 1, 0) == 0);
    ipc_wait_item_t items[3] = {
        { sp, IPC_READABLE, 0xFFFF }, { p[1], IPC_READABLE, 0xFFFF }, { port, IPC_READABLE, 0xFFFF },
    };
    CHECK(ipc_wait(items, 3, 0) == 1 && items[0].ready == 0 && items[1].ready == IPC_READABLE && items[2].ready == 0);

    // Errors
    items[0].handle = 63;
    CHECK(ipc_wait(items, 3, 0) == -1 && errno == EBADF);
    CHECK(ipc_wait(items, 0, 0) == -1 && errno == EINVAL);
    CHECK(ipc_wait((ipc_wait_item_t*)16, 1, 0) == -1 && errno == EFAULT);

    // Peer process ends while we wait
    __test_ipc_set_sleep_hook(hook_exit_b);
    CHECK(ipc_wait_one(sp, IPC_READABLE, IPC_FOREVER) == (IPC_READABLE | IPC_HANGUP));

    // Blocking accept
    __test_ipc_set_sleep_hook(hook_connect);
    int sc = ipc_accept_wait(port);
    CHECK(sc > 0 && ipc_recv(sc, buf, sizeof(buf), NULL) == 5 && memcmp(buf, "hello", 5) == 0);

    __test_ipc_exit(PID_A);
    __test_ipc_exit(PID_C);
    as(0);
}

static void test_input(void) {
    input_event_t ev[40];

    CHECK(!__test_ipc_grabbed());
    CHECK(sec_set(SEC_ALL & ~SEC_INPUT) == 0);
    CHECK(input_open() == -1 && errno == EPERM);
    CHECK(sec_set(SEC_ALL) == 0);

    // Events queued before the grab belonged to the window manager
    input_event_t stale = { INPUT_KEY, 'z', 0, 0, 0, 0, 0, 1 };
    CHECK(__test_input_push(&stale) && __test_input_queued() == 1);

    __test_input_set_state(320, 240, INPUT_BTN_RIGHT, INPUT_MOD_SHIFT);
    uint32_t changes = __test_ipc_grab_changes();
    int in = input_open();
    CHECK(in > 0 && __test_ipc_grabbed() && __test_ipc_grab_changes() == changes + 1);
    CHECK(__test_input_queued() == 0);
    CHECK(input_open() == -1 && errno == EBUSY);
    as(PID_C);
    CHECK(input_open() == -1 && errno == EBUSY);           // Any process
    CHECK(input_read(in, ev, 4) == -1 && errno == EBADF);  // Not C's handle
    as(0);

    ipc_info_t ii;
    CHECK(ipc_info(in, &ii) == 0 && ii.type == IPC_TYPE_INPUT);

    // First: where the mouse is now
    CHECK(input_wait(in, 0) == 1);
    CHECK(input_read(in, ev, 0) == 0);
    CHECK(input_read(in, ev, 40) == 1);
    CHECK(ev[0].type == INPUT_MOUSE && ev[0].x == 320 && ev[0].y == 240 &&
          ev[0].buttons == INPUT_BTN_RIGHT && ev[0].mods == INPUT_MOD_SHIFT);
    CHECK(input_read(in, ev, 40) == 0);
    CHECK(input_wait(in, 0) == 0);

    // Queued events in order
    input_event_t m1 = { INPUT_MOUSE, 0, 10, 20, INPUT_BTN_LEFT, 0, 0, 100 };
    input_event_t k1 = { INPUT_KEY, 'a', 10, 20, INPUT_BTN_LEFT, 0, 0, 101 };
    input_event_t k2 = { INPUT_KEY, KEY_ESCAPE, 10, 20, 0, 0, 0, 102 };
    CHECK(__test_input_push(&m1) && __test_input_push(&k1) && __test_input_push(&k2));
    CHECK(input_read(in, ev, 2) == 2 && ev[0].type == INPUT_MOUSE && ev[0].buttons == INPUT_BTN_LEFT &&
          ev[1].type == INPUT_KEY && ev[1].key == 'a');
    CHECK(input_read(in, ev, 2) == 1 && ev[0].key == KEY_ESCAPE);

    // At most INPUT_READ_MAX per call
    for (int i = 0; i < 40; i++) {
        input_event_t m = { INPUT_MOUSE, 0, (int16_t)i, 0, 0, 0, 0, (uint32_t)i };
        __test_input_push(&m);
    }
    CHECK(input_read(in, ev, 40) == INPUT_READ_MAX && ev[31].x == 31);
    CHECK(input_read(in, ev, 40) == 8 && ev[7].x == 39);
    CHECK(input_read(in, (input_event_t*)16, 4) == -1 && errno == EFAULT);

    // Woken by the "IRQ"
    __test_ipc_set_sleep_hook(hook_mouse);
    CHECK(input_wait(in, IPC_FOREVER) == 1);
    CHECK(input_read(in, ev, 4) == 1 && ev[0].x == 200 && ev[0].y == 150);

    // No SEC_IPC needed: input alone is enough to wait and close
    CHECK(sec_set(SEC_ALL & ~SEC_IPC) == 0);
    CHECK(input_wait(in, 0) == 0);
    int p[2];
    CHECK(ipc_pipe(p) == -1 && errno == EPERM);
    CHECK(input_close(in) == 0 && !__test_ipc_grabbed() && __test_ipc_grab_changes() == changes + 2);
    CHECK(sec_set(SEC_ALL) == 0);

    // Ending the process lets go too
    as(PID_B);
    in = input_open();
    CHECK(in > 0 && __test_ipc_grabbed());
    __test_ipc_exit(PID_B);
    CHECK(!__test_ipc_grabbed());
    as(0);
}

// Random operations from three processes; at the end everything is freed
static void test_fuzz(void) {
    uint32_t seed = 12345;
    #define RND() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7FFF)
    const uint32_t pids[3] = { PID_A, PID_B, PID_C };
    char buf[IPC_MSG_MAX];
    memset(buf, 'f', sizeof(buf));
    int ok = 0;

    for (int i = 0; i < 20000; i++) {
        as(pids[RND() % 3]);
        int h = 1 + (int)(RND() % IPC_HANDLE_MAX);
        int other = (RND() % 4 == 0) ? 1 + (int)(RND() % IPC_HANDLE_MAX) : 0;
        int r = -1;
        switch (RND() % 13) {
            case 0: { int p[2]; r = ipc_pipe(p); break; }
            case 1: r = ipc_close(h); break;
            case 2: r = ipc_send(h, buf, 1 + RND() % IPC_MSG_MAX, other); break;
            case 3: r = (int)ipc_recv(h, buf, RND() % (IPC_MSG_MAX + 1), NULL); break;
            case 4: r = shm_create(1 + RND() % 20000); break;
            case 5: r = shm_map(h) ? 0 : -1; break;
            case 6: r = shm_unmap(h); break;
            case 7: { char name[8] = { 'f', 'z', (char)('a' + RND() % 4), 0 }; r = ipc_port_create(name); break; }
            case 8: { char name[8] = { 'f', 'z', (char)('a' + RND() % 4), 0 }; r = ipc_connect(name); break; }
            case 9: r = ipc_accept(h); break;
            case 10: { ipc_wait_item_t it = { h, IPC_READABLE | IPC_WRITABLE, 0 }; r = ipc_wait(&it, 1, 0); break; }
            case 11: r = (RND() % 50 == 0) ? (__test_ipc_exit(pids[RND() % 3]), 0) : 0; break;
            case 12: { ipc_info_t ii; r = ipc_info(h, &ii); break; }
        }
        ok += (r >= 0);
    }
    for (int i = 0; i < 3; i++)
        __test_ipc_exit(pids[i]);
    as(0);
    __test_ipc_exit((uint32_t)getpid());       // This process's (empty) table too
    if (__test_ipc_allocs_live() != 0)
        printf("ipc fuzz: %u kernel allocations still live\n", (unsigned)__test_ipc_allocs_live());

    CHECK(ok > 1000);
    CHECK(__test_ipc_pages_live() == 0 && __test_ipc_maps_live() == 0);
    CHECK(__test_ipc_allocs_live() == 0);     // Every pipe, port, message and table
    CHECK(malloc_check() == 0);
    printf("ipc fuzz: 20000 random calls from 3 processes (%d succeeded), everything freed\n", ok);
}

// The host's end of the terminal in test_tty
static int tty_host;

static void tty_host_sends(void) {
    as(PID_A);
    ipc_send(tty_host, "late\n", 5, 0);
}

static void tty_host_reads(void) {
    char buf[IPC_MSG_MAX];
    as(PID_A);
    ipc_recv(tty_host, buf, sizeof(buf), NULL);
}

static void test_tty(void) {
    char buf[IPC_MSG_MAX + 8];
    struct winsize ws;
    int ready;
    int p[2];

    // A hosts B on a terminal
    as(PID_A);
    CHECK(ipc_pipe(p) == 0);
    CHECK(__test_tty_attach(PID_B, 999) == SYS_EBADF);
    CHECK(__test_tty_attach(PID_B, (uint32_t)p[1]) == SYS_OK);
    CHECK(ipc_close(p[1]) == 0);        // B holds its own reference
    tty_host = p[0];

    // Size: the default, then what the host sets
    as(PID_B);
    memset(&ws, 0xFF, sizeof(ws));
    CHECK(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col == 80 && ws.ws_row == 25 &&
          ws.ws_xpixel == 0 && ws.ws_ypixel == 0);
    CHECK(ioctl(3, TIOCGWINSZ, &ws) == -1 && errno == ENOTTY);
    CHECK(ioctl(SYS_FD_MAX, TIOCGWINSZ, &ws) == -1 && errno == EBADF);
    CHECK(ioctl(STDIN_FILENO, 0x1234, &ws) == -1 && errno == ENOTTY);
    CHECK(ioctl(STDIN_FILENO, TIOCGWINSZ, (void*)0x10) == -1 && errno == EFAULT);
    as(PID_A);
    CHECK(tty_resize(p[0], 0, 40) == -1 && errno == EINVAL);
    CHECK(tty_resize(p[0], 120, 0x10000) == -1 && errno == EINVAL);
    CHECK(tty_resize(p[0], 120, 40) == 0);
    as(PID_B);
    CHECK(ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col == 120 && ws.ws_row == 40);

    // Input: each line as sent, split over reads too small for it
    as(PID_A);
    CHECK(ipc_send(p[0], "hello\n", 6, 0) == 0);
    CHECK(ipc_send(p[0], "x\n", 2, 0) == 0);
    as(PID_B);
    CHECK(ioctl(STDIN_FILENO, FIONREAD, &ready) == 0 && ready == 8);
    CHECK(__test_stdio_read(buf, 3) == 3 && memcmp(buf, "hel", 3) == 0);
    CHECK(ioctl(STDIN_FILENO, FIONREAD, &ready) == 0 && ready == 5);
    CHECK(__test_stdio_read(buf, sizeof(buf)) == 3 && memcmp(buf, "lo\n", 3) == 0);
    CHECK(__test_stdio_read(buf, sizeof(buf)) == 2 && memcmp(buf, "x\n", 2) == 0);
    CHECK(__test_stdio_read(buf, 0) == 0);

    // Nothing queued: the read blocks until the host sends
    __test_ipc_set_sleep_hook(tty_host_sends);
    CHECK(__test_stdio_read(buf, sizeof(buf)) == 5 && memcmp(buf, "late\n", 5) == 0);

    // Output: in messages of at most IPC_MSG_MAX
    static char big[1300];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (char)('a' + i % 26);
    as(PID_B);
    CHECK(__test_stdio_write("out", 3) == 3);
    CHECK(__test_stdio_write(big, sizeof(big)) == (int32_t)sizeof(big));
    CHECK(__test_stdio_write((void*)0x10, 4) == SYS_EFAULT);
    as(PID_A);
    CHECK(ipc_recv(p[0], buf, sizeof(buf), NULL) == 3 && memcmp(buf, "out", 3) == 0);
    CHECK(ipc_recv(p[0], buf, sizeof(buf), NULL) == IPC_MSG_MAX && memcmp(buf, big, IPC_MSG_MAX) == 0);
    CHECK(ipc_recv(p[0], buf, sizeof(buf), NULL) == IPC_MSG_MAX && memcmp(buf, big + IPC_MSG_MAX, IPC_MSG_MAX) == 0);
    CHECK(ipc_recv(p[0], buf, sizeof(buf), NULL) == 1300 - 2 * IPC_MSG_MAX &&
          memcmp(buf, big + 2 * IPC_MSG_MAX, 1300 - 2 * IPC_MSG_MAX) == 0);
    CHECK(ipc_recv(p[0], buf, sizeof(buf), NULL) == -1 && errno == EAGAIN);

    // Host's queue full: the write waits for it to read
    as(PID_B);
    for (int i = 0; i < SYS_IPC_QUEUE_MAX; i++)
        CHECK(__test_stdio_write("q", 1) == 1);
    __test_ipc_set_sleep_hook(tty_host_reads);
    CHECK(__test_stdio_write("z", 1) == 1);
    as(PID_A);
    for (int i = 0; i < SYS_IPC_QUEUE_MAX; i++)
        CHECK(ipc_recv(p[0], buf, sizeof(buf), NULL) == 1);
    CHECK(buf[0] == 'z');

    // No terminal: read EBADF, write falls through to the console, no ioctl
    as(PID_C);
    CHECK(__test_stdio_read(buf, sizeof(buf)) == SYS_EBADF);
    CHECK(__test_stdio_write("c", 1) == SYS_ENOTTY);
    CHECK(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 && errno == ENOTTY);

    // Host gone: end of file, broken pipe
    as(PID_A);
    CHECK(ipc_send(p[0], "last\n", 5, 0) == 0);
    CHECK(ipc_close(p[0]) == 0);
    as(PID_B);
    CHECK(__test_stdio_read(buf, sizeof(buf)) == 5 && memcmp(buf, "last\n", 5) == 0);   // What was sent first
    CHECK(__test_stdio_read(buf, sizeof(buf)) == 0);
    CHECK(__test_stdio_write("x", 1) == SYS_EPIPE);
    CHECK(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0);     // Still its terminal

    // The child ends: everything is freed
    __test_ipc_exit(PID_B);
    __test_ipc_exit(PID_C);
    __test_ipc_exit(PID_A);
    as(0);
}

int main(void) {
    test_pipe_basics();
    test_shm();
    test_handle_passing();
    test_ports();
    test_wait();
    test_input();
    test_fuzz();
    test_no_admin();
    test_tty();

    __test_ipc_exit((uint32_t)getpid());
    CHECK(__test_ipc_pages_live() == 0 && __test_ipc_maps_live() == 0 && __test_ipc_allocs_live() == 0);
    CHECK(malloc_check() == 0);

    printf("ipc: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

