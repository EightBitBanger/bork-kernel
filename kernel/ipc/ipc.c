// IPC core: pipes, ports, shared memory blocks and the input grab.
//
// See ipc.h for the overview and syscall/syscall.h for the user ABI. This
// file only uses the platform calls in ipc_platform.h, malloc and the
// string helpers, so the libc host tests compile it unchanged.
//
// Locking: one lock (ipc_plat_lock) guards every object, queue, port name
// and handle table. Every call runs start to finish under it, except
// SYS_IPC_WAIT, which drops it while it sleeps. A waiting thread keeps its
// handle table alive through the table's `users` count; teardown marks the
// table `dying`, wakes everyone and waits for `users` to reach zero.
//
// Reference counts: every handle and every queued message that carries an
// object holds one reference. When the count reaches zero the object goes
// on a dead list and is destroyed by ipc_reap(), iteratively, because
// destroying a pipe end drops the messages queued for it, which may carry
// the last reference to other objects, and so on.
//
// No cycles: a pipe end X queued for end Y means Y holds X. If X could in
// turn hold Y (directly or through others), closing every handle would
// leave both alive forever. So an end can only be sent while nothing in its
// own queue carries a pipe end (SYS_EBUSY otherwise), and never through its
// own pipe. Every new "holds" edge then points at an end that holds
// nothing, which cannot close a cycle.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>
#include <kernel/ipc/ipc.h>
#include <kernel/ipc/ipc_platform.h>

#define IPC_PAGE_SIZE   4096u

_Static_assert(sizeof(SysIpcMsgInfo) == 16 && sizeof(SysIpcWaitItem) == 8 &&
               sizeof(SysIpcInfo) == 32 && sizeof(SysInputEvent) == 16,
               "IPC structures are part of the user ABI");

//
// Objects
//

typedef struct IpcObject {
    uint32_t type;                  // SYS_IPC_TYPE_*
    uint32_t refs;
    struct IpcObject* dead_next;    // Dead list link (refs reached zero)
} IpcObject;

typedef struct IpcMsg {
    struct IpcMsg* next;
    uint32_t   length;
    uint32_t   sender;              // Process id
    IpcObject* attach;              // Carried object (holds a reference), or NULL
    uint8_t    data[];
} IpcMsg;

typedef struct {
    IpcMsg*  head;
    IpcMsg*  tail;
    uint32_t count;
    uint32_t bytes;
    uint32_t ends;                  // Messages carrying a pipe end
} IpcQueue;

struct IpcPipe;

// One end of a pipe. Ends live inside their pipe.
typedef struct {
    IpcObject       obj;
    struct IpcPipe* pipe;
    uint32_t        side;           // 0 or 1
} IpcEnd;

typedef struct IpcPipe {
    IpcEnd   end[2];
    IpcQueue queue[2];              // queue[i]: messages waiting for end i
    bool     closed[2];             // end i has no references left
} IpcPipe;

typedef struct IpcPort {
    IpcObject obj;
    char      name[SYS_IPC_NAME_MAX];
    IpcEnd*   backlog[SYS_IPC_BACKLOG];     // Server ends waiting for accept (each holds a reference)
    uint32_t  backlog_count;
    struct IpcPort* next;
} IpcPort;

typedef struct {
    IpcObject obj;
    void*     kva;                  // Kernel address of the pages
    uint32_t  pages;
} IpcShm;

typedef struct {
    IpcObject obj;
    bool      sync_pending;         // First read starts with the current mouse state
} IpcInput;

//
// Handle tables
//

typedef struct {
    IpcObject* obj;                 // NULL = free
    uint32_t   map_addr;            // Shm: where this handle is mapped, 0 = not
} IpcHandle;

typedef struct IpcProc {
    uint32_t  pid;
    void*     space;
    uint32_t  users;                // Threads sleeping in SYS_IPC_WAIT
    bool      dying;
    IpcHandle handles[SYS_IPC_HANDLE_MAX];
    struct IpcProc* next;
} IpcProc;

static IpcProc*   ipc_procs     = NULL;
static IpcPort*   ipc_ports     = NULL;
static IpcObject* ipc_dead      = NULL;
static bool       ipc_reaping   = false;
static uint32_t   ipc_queued    = 0;        // Message bytes queued system-wide
static uint32_t   ipc_shm_bytes = 0;        // Bytes in all shared blocks
static IpcInput*  ipc_input_owner = NULL;
static volatile bool ipc_grab   = false;    // Read by IRQs and the console without the lock

//
// Reference counting
//

static void ipc_destroy(IpcObject* obj);

static void ipc_reap(void) {
    if (ipc_reaping)
        return;     // The outer ipc_reap() loop picks up what was just added
    ipc_reaping = true;
    while (ipc_dead != NULL) {
        IpcObject* obj = ipc_dead;
        ipc_dead = obj->dead_next;
        ipc_destroy(obj);
    }
    ipc_reaping = false;
}

static void ipc_ref(IpcObject* obj) {
    obj->refs++;
}

static void ipc_unref(IpcObject* obj) {
    if (--obj->refs != 0)
        return;
    obj->dead_next = ipc_dead;
    ipc_dead = obj;
    ipc_reap();
}

static void ipc_queue_push(IpcQueue* q, IpcMsg* m) {
    m->next = NULL;
    if (q->tail != NULL) q->tail->next = m;
    else                 q->head = m;
    q->tail = m;
    q->count++;
    q->bytes += m->length;
    if (m->attach != NULL && m->attach->type == SYS_IPC_TYPE_PIPE)
        q->ends++;
    ipc_queued += m->length;
}

static IpcMsg* ipc_queue_pop(IpcQueue* q) {
    IpcMsg* m = q->head;
    if (m == NULL)
        return NULL;
    q->head = m->next;
    if (q->head == NULL)
        q->tail = NULL;
    q->count--;
    q->bytes -= m->length;
    if (m->attach != NULL && m->attach->type == SYS_IPC_TYPE_PIPE)
        q->ends--;
    ipc_queued -= m->length;
    return m;
}

static void ipc_msg_free(IpcMsg* m) {
    IpcObject* attach = m->attach;
    free(m);
    if (attach != NULL)
        ipc_unref(attach);
}

static void ipc_destroy(IpcObject* obj) {
    switch (obj->type) {
        case SYS_IPC_TYPE_PIPE: {
            IpcEnd*  end  = (IpcEnd*)obj;
            IpcPipe* pipe = end->pipe;
            pipe->closed[end->side] = true;

            // Nobody can receive these any more
            IpcMsg* m;
            while ((m = ipc_queue_pop(&pipe->queue[end->side])) != NULL)
                ipc_msg_free(m);

            if (pipe->closed[0] && pipe->closed[1])
                free(pipe);
            break;
        }

        case SYS_IPC_TYPE_PORT: {
            IpcPort* port = (IpcPort*)obj;
            for (IpcPort** link = &ipc_ports; *link != NULL; link = &(*link)->next) {
                if (*link == port) {
                    *link = port->next;
                    break;
                }
            }
            for (uint32_t i = 0; i < port->backlog_count; i++)
                ipc_unref(&port->backlog[i]->obj);
            free(port);
            break;
        }

        case SYS_IPC_TYPE_SHM: {
            IpcShm* shm = (IpcShm*)obj;
            ipc_plat_free_pages(shm->kva, shm->pages);
            ipc_shm_bytes -= shm->pages * IPC_PAGE_SIZE;
            free(shm);
            break;
        }

        case SYS_IPC_TYPE_INPUT: {
            IpcInput* input = (IpcInput*)obj;
            if (ipc_input_owner == input) {
                ipc_input_owner = NULL;
                ipc_grab = false;
                ipc_plat_input_grab_changed(false);
            }
            free(input);
            break;
        }
    }

    // Peers may now see a hangup, writers may have room
    ipc_plat_wake_all();
}

static IpcPipe* ipc_pipe_new(void) {
    IpcPipe* pipe = (IpcPipe*)malloc(sizeof(IpcPipe));
    if (pipe == NULL)
        return NULL;
    memset(pipe, 0, sizeof(IpcPipe));
    for (uint32_t i = 0; i < 2; i++) {
        pipe->end[i].obj.type = SYS_IPC_TYPE_PIPE;
        pipe->end[i].obj.refs = 1;
        pipe->end[i].pipe     = pipe;
        pipe->end[i].side     = i;
    }
    return pipe;
}

//
// Handle tables (lock held)
//

static IpcHandle* ipc_handle_get(IpcProc* t, uint32_t handle) {
    if (handle == 0 || handle > SYS_IPC_HANDLE_MAX)
        return NULL;
    IpcHandle* h = &t->handles[handle - 1];
    return (h->obj != NULL) ? h : NULL;
}

static IpcHandle* ipc_handle_typed(IpcProc* t, uint32_t handle, uint32_t type) {
    IpcHandle* h = ipc_handle_get(t, handle);
    return (h != NULL && h->obj->type == type) ? h : NULL;
}

// A free handle number other than `skip`, or 0
static uint32_t ipc_handle_free(IpcProc* t, uint32_t skip) {
    for (uint32_t i = 1; i <= SYS_IPC_HANDLE_MAX; i++) {
        if (i != skip && t->handles[i - 1].obj == NULL)
            return i;
    }
    return 0;
}

// Takes over one reference the caller holds
static void ipc_handle_install(IpcProc* t, uint32_t handle, IpcObject* obj) {
    t->handles[handle - 1].obj      = obj;
    t->handles[handle - 1].map_addr = 0;
}

static void ipc_handle_close(IpcProc* t, IpcHandle* h, bool unmap) {
    IpcObject* obj = h->obj;
    if (h->map_addr != 0 && unmap)
        ipc_plat_unmap(t->space, h->map_addr, ((IpcShm*)obj)->pages);
    h->obj      = NULL;
    h->map_addr = 0;
    ipc_unref(obj);
}

static void ipc_close_all(IpcProc* t, bool unmap) {
    for (uint32_t i = 0; i < SYS_IPC_HANDLE_MAX; i++) {
        if (t->handles[i].obj != NULL)
            ipc_handle_close(t, &t->handles[i], unmap);
    }
}

// The caller's table, created if needed. NULL when out of memory.
static IpcProc* ipc_proc(const IpcCaller* c) {
    IpcProc* t;
    for (t = ipc_procs; t != NULL; t = t->next) {
        if (t->pid == c->pid)
            break;
    }

    if (t == NULL) {
        t = (IpcProc*)malloc(sizeof(IpcProc));
        if (t == NULL)
            return NULL;
        memset(t, 0, sizeof(IpcProc));
        t->pid   = c->pid;
        t->space = c->space;
        t->next  = ipc_procs;
        ipc_procs = t;
    }

    // Left by an earlier process with this id that ended without
    // ipc_process_exit(). Its address space is gone: drop, don't unmap.
    if (t->space != c->space) {
        ipc_close_all(t, false);
        t->space = c->space;
    }
    return t;
}

//
// User strings
//

// Port name: 1 .. SYS_IPC_NAME_MAX - 1 characters of [A-Za-z0-9._-]
static int32_t ipc_user_name(const IpcCaller* c, uint32_t user_addr, char out[SYS_IPC_NAME_MAX]) {
    for (uint32_t i = 0; i < SYS_IPC_NAME_MAX; i++) {
        char ch;
        if (user_addr + i < user_addr || !ipc_plat_user_read(c->space, &ch, user_addr + i, 1))
            return SYS_EFAULT;
        out[i] = ch;
        if (ch == '\0')
            return (i == 0) ? SYS_EINVAL : SYS_OK;

        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '.' || ch == '_' || ch == '-';
        if (!ok)
            return SYS_EINVAL;
    }
    return SYS_ENAMETOOLONG;
}

static IpcPort* ipc_port_find(const char* name) {
    for (IpcPort* p = ipc_ports; p != NULL; p = p->next) {
        if (strcmp(p->name, name) == 0)
            return p;
    }
    return NULL;
}

//
// Calls. Each takes the lock, finds the caller's table and works on it.
//

#define IPC_ENTER(c, t)                     \
    ipc_plat_lock();                        \
    IpcProc* t = ipc_proc(c);               \
    if (t == NULL) {                        \
        ipc_plat_unlock();                  \
        return SYS_ENOMEM;                  \
    }

#define IPC_LEAVE(result)                   \
    do {                                    \
        ipc_plat_unlock();                  \
        return (result);                    \
    } while (0)

int32_t ipc_sys_close(const IpcCaller* c, uint32_t handle) {
    IPC_ENTER(c, t);
    IpcHandle* h = ipc_handle_get(t, handle);
    if (h == NULL)
        IPC_LEAVE(SYS_EBADF);
    ipc_handle_close(t, h, true);
    IPC_LEAVE(SYS_OK);
}

int32_t ipc_sys_pipe(const IpcCaller* c, uint32_t user_out) {
    IPC_ENTER(c, t);
    uint32_t a = ipc_handle_free(t, 0);
    uint32_t b = (a != 0) ? ipc_handle_free(t, a) : 0;
    if (b == 0)
        IPC_LEAVE(SYS_EMFILE);

    IpcPipe* pipe = ipc_pipe_new();
    if (pipe == NULL)
        IPC_LEAVE(SYS_ENOMEM);

    ipc_handle_install(t, a, &pipe->end[0].obj);
    ipc_handle_install(t, b, &pipe->end[1].obj);

    int32_t out[2] = { (int32_t)a, (int32_t)b };
    if (!ipc_plat_user_write(c->space, user_out, out, sizeof(out))) {
        ipc_handle_close(t, &t->handles[a - 1], false);
        ipc_handle_close(t, &t->handles[b - 1], false);
        IPC_LEAVE(SYS_EFAULT);
    }
    IPC_LEAVE(SYS_OK);
}

int32_t ipc_sys_port_create(const IpcCaller* c, uint32_t user_name) {
    char name[SYS_IPC_NAME_MAX];
    int32_t r = ipc_user_name(c, user_name, name);
    if (r < 0)
        return r;
    if (strncmp(name, "sys.", 4) == 0 && !(c->security & SYS_SEC_ADMIN))
        return SYS_EPERM;

    IPC_ENTER(c, t);
    if (ipc_port_find(name) != NULL)
        IPC_LEAVE(SYS_EEXIST);

    uint32_t handle = ipc_handle_free(t, 0);
    if (handle == 0)
        IPC_LEAVE(SYS_EMFILE);

    IpcPort* port = (IpcPort*)malloc(sizeof(IpcPort));
    if (port == NULL)
        IPC_LEAVE(SYS_ENOMEM);
    memset(port, 0, sizeof(IpcPort));
    port->obj.type = SYS_IPC_TYPE_PORT;
    port->obj.refs = 1;
    memcpy(port->name, name, sizeof(name));
    port->next = ipc_ports;
    ipc_ports  = port;

    ipc_handle_install(t, handle, &port->obj);
    IPC_LEAVE((int32_t)handle);
}

int32_t ipc_sys_connect(const IpcCaller* c, uint32_t user_name) {
    char name[SYS_IPC_NAME_MAX];
    int32_t r = ipc_user_name(c, user_name, name);
    if (r < 0)
        return r;

    IPC_ENTER(c, t);
    IpcPort* port = ipc_port_find(name);
    if (port == NULL)
        IPC_LEAVE(SYS_ENOENT);
    if (port->backlog_count >= SYS_IPC_BACKLOG)
        IPC_LEAVE(SYS_EAGAIN);

    uint32_t handle = ipc_handle_free(t, 0);
    if (handle == 0)
        IPC_LEAVE(SYS_EMFILE);

    IpcPipe* pipe = ipc_pipe_new();
    if (pipe == NULL)
        IPC_LEAVE(SYS_ENOMEM);

    // end[0] to the client, end[1] waits on the port for the server
    ipc_handle_install(t, handle, &pipe->end[0].obj);
    port->backlog[port->backlog_count++] = &pipe->end[1];

    ipc_plat_wake_all();
    IPC_LEAVE((int32_t)handle);
}

int32_t ipc_sys_accept(const IpcCaller* c, uint32_t port_handle) {
    IPC_ENTER(c, t);
    IpcHandle* h = ipc_handle_typed(t, port_handle, SYS_IPC_TYPE_PORT);
    if (h == NULL)
        IPC_LEAVE(SYS_EBADF);

    IpcPort* port = (IpcPort*)h->obj;
    if (port->backlog_count == 0)
        IPC_LEAVE(SYS_EAGAIN);

    uint32_t handle = ipc_handle_free(t, 0);
    if (handle == 0)
        IPC_LEAVE(SYS_EMFILE);

    // Oldest first; the backlog's reference moves to the handle
    IpcEnd* end = port->backlog[0];
    port->backlog_count--;
    for (uint32_t i = 0; i < port->backlog_count; i++)
        port->backlog[i] = port->backlog[i + 1];

    ipc_handle_install(t, handle, &end->obj);
    IPC_LEAVE((int32_t)handle);
}

int32_t ipc_sys_send(const IpcCaller* c, uint32_t pipe_handle, uint32_t user_buf, uint32_t length, uint32_t attach) {
    if (length == 0 || length > SYS_IPC_MSG_MAX)
        return SYS_EINVAL;

    // Copy in before taking the lock
    IpcMsg* m = (IpcMsg*)malloc(sizeof(IpcMsg) + length);
    if (m == NULL)
        return SYS_ENOMEM;
    if (!ipc_plat_user_read(c->space, m->data, user_buf, length)) {
        free(m);
        return SYS_EFAULT;
    }
    m->next   = NULL;
    m->length = length;
    m->sender = c->pid;
    m->attach = NULL;

    int32_t result = SYS_OK;
    ipc_plat_lock();
    IpcProc* t = ipc_proc(c);
    IpcHandle* h = (t != NULL) ? ipc_handle_typed(t, pipe_handle, SYS_IPC_TYPE_PIPE) : NULL;
    IpcHandle* a = (t != NULL && attach != 0) ? ipc_handle_get(t, attach) : NULL;

    if (t == NULL) {
        result = SYS_ENOMEM;
    } else if (h == NULL || (attach != 0 && a == NULL)) {
        result = SYS_EBADF;
    } else {
        IpcEnd*  end  = (IpcEnd*)h->obj;
        IpcPipe* pipe = end->pipe;
        uint32_t peer = end->side ^ 1;

        if (a != NULL && a->obj->type != SYS_IPC_TYPE_PIPE && a->obj->type != SYS_IPC_TYPE_SHM)
            result = SYS_EINVAL;    // Ports and input stay with their owner
        else if (a != NULL && a->obj->type == SYS_IPC_TYPE_PIPE && ((IpcEnd*)a->obj)->pipe == pipe)
            result = SYS_EINVAL;    // An end through its own pipe
        else if (a != NULL && a->obj->type == SYS_IPC_TYPE_PIPE &&
                 ((IpcEnd*)a->obj)->pipe->queue[((IpcEnd*)a->obj)->side].ends != 0)
            result = SYS_EBUSY;     // It holds ends itself: could close a cycle (see the top)
        else if (pipe->closed[peer])
            result = SYS_EPIPE;
        else if (pipe->queue[peer].count >= SYS_IPC_QUEUE_MAX ||
                 ipc_queued + length > SYS_IPC_GLOBAL_BYTES)
            result = SYS_EAGAIN;
        else {
            if (a != NULL) {
                ipc_ref(a->obj);
                m->attach = a->obj;
            }
            ipc_queue_push(&pipe->queue[peer], m);
            m = NULL;
            ipc_plat_wake_all();
        }
    }
    ipc_plat_unlock();

    if (m != NULL)
        free(m);
    return result;
}

int32_t ipc_sys_recv(const IpcCaller* c, uint32_t pipe_handle, uint32_t user_buf, uint32_t size, uint32_t user_info) {
    IPC_ENTER(c, t);
    IpcHandle* h = ipc_handle_typed(t, pipe_handle, SYS_IPC_TYPE_PIPE);
    if (h == NULL)
        IPC_LEAVE(SYS_EBADF);

    IpcEnd*   end  = (IpcEnd*)h->obj;
    IpcPipe*  pipe = end->pipe;
    IpcQueue* q    = &pipe->queue[end->side];
    IpcMsg*   m    = q->head;
    if (m == NULL)
        IPC_LEAVE(pipe->closed[end->side ^ 1] ? SYS_EPIPE : SYS_EAGAIN);

    SysIpcMsgInfo info = { m->length, 0, m->sender, 0 };

    if (size < m->length) {
        if (user_info != 0 && !ipc_plat_user_write(c->space, user_info, &info, sizeof(info)))
            IPC_LEAVE(SYS_EFAULT);
        IPC_LEAVE(SYS_ERANGE);
    }

    uint32_t slot = 0;
    if (m->attach != NULL) {
        slot = ipc_handle_free(t, 0);
        if (slot == 0)
            IPC_LEAVE(SYS_EMFILE);
        info.handle = (int32_t)slot;
    }

    // Both copies before anything changes: a fault leaves the message queued
    if (user_info != 0 && !ipc_plat_user_write(c->space, user_info, &info, sizeof(info)))
        IPC_LEAVE(SYS_EFAULT);
    if (!ipc_plat_user_write(c->space, user_buf, m->data, m->length))
        IPC_LEAVE(SYS_EFAULT);

    ipc_queue_pop(q);
    if (m->attach != NULL) {
        ipc_handle_install(t, slot, m->attach);     // The message's reference moves over
        m->attach = NULL;
    }
    int32_t length = (int32_t)m->length;
    ipc_msg_free(m);

    ipc_plat_wake_all();    // The sender has room again
    IPC_LEAVE(length);
}

//
// Waiting
//

// What `h` is ready for, or a negative SYS_E* when the caller may not wait on it
static int32_t ipc_ready(const IpcCaller* c, IpcHandle* h) {
    switch (h->obj->type) {
        case SYS_IPC_TYPE_PIPE: {
            if (!(c->security & SYS_SEC_IPC))
                return SYS_EPERM;
            IpcEnd*  end  = (IpcEnd*)h->obj;
            IpcPipe* pipe = end->pipe;
            uint32_t peer = end->side ^ 1;
            if (pipe->closed[peer])
                return SYS_IPC_READABLE | SYS_IPC_WRITABLE | SYS_IPC_HANGUP;

            int32_t ready = 0;
            if (pipe->queue[end->side].count > 0)
                ready |= SYS_IPC_READABLE;
            if (pipe->queue[peer].count < SYS_IPC_QUEUE_MAX && ipc_queued < SYS_IPC_GLOBAL_BYTES)
                ready |= SYS_IPC_WRITABLE;
            return ready;
        }

        case SYS_IPC_TYPE_PORT:
            if (!(c->security & SYS_SEC_IPC))
                return SYS_EPERM;
            return (((IpcPort*)h->obj)->backlog_count > 0) ? SYS_IPC_READABLE : 0;

        case SYS_IPC_TYPE_SHM:
            if (!(c->security & SYS_SEC_IPC))
                return SYS_EPERM;
            return 0;

        case SYS_IPC_TYPE_INPUT:
            if (!(c->security & SYS_SEC_INPUT))
                return SYS_EPERM;
            return (((IpcInput*)h->obj)->sync_pending || ipc_plat_input_pending()) ? SYS_IPC_READABLE : 0;
    }
    return 0;
}

int32_t ipc_sys_wait(const IpcCaller* c, uint32_t user_items, uint32_t count, uint32_t timeout_ms) {
    if (count == 0 || count > SYS_IPC_WAIT_MAX)
        return SYS_EINVAL;

    SysIpcWaitItem items[SYS_IPC_WAIT_MAX];
    if (!ipc_plat_user_read(c->space, items, user_items, count * sizeof(SysIpcWaitItem)))
        return SYS_EFAULT;

    // Registered before the first check: no wake can slip past
    IpcSleeper sleeper;
    ipc_plat_sleep_begin(&sleeper);

    ipc_plat_lock();
    IpcProc* t = ipc_proc(c);
    if (t == NULL) {
        ipc_plat_unlock();
        ipc_plat_sleep_end(&sleeper);
        return SYS_ENOMEM;
    }
    t->users++;

    uint32_t start  = ipc_plat_time_ms();
    int32_t  result = 0;
    while (1) {
        // Lock held here
        if (t->dying) {
            result = SYS_EPIPE;
            break;
        }

        result = 0;
        for (uint32_t i = 0; i < count; i++) {
            IpcHandle* h = (items[i].handle > 0) ? ipc_handle_get(t, (uint32_t)items[i].handle) : NULL;
            if (h == NULL) {
                result = SYS_EBADF;
                break;
            }
            int32_t ready = ipc_ready(c, h);
            if (ready < 0) {
                result = ready;
                break;
            }
            items[i].ready = (uint16_t)ready & (items[i].events | SYS_IPC_HANGUP);
            if (items[i].ready != 0)
                result++;
        }
        if (result != 0 || timeout_ms == 0)
            break;

        uint32_t waited = ipc_plat_time_ms() - start;
        if (timeout_ms != SYS_IPC_WAIT_FOREVER && waited >= timeout_ms)
            break;

        ipc_plat_unlock();
        ipc_plat_sleep(&sleeper, (timeout_ms == SYS_IPC_WAIT_FOREVER) ? SYS_IPC_WAIT_FOREVER
                                                                       : timeout_ms - waited);
        ipc_plat_lock();
    }

    t->users--;
    ipc_plat_unlock();
    ipc_plat_sleep_end(&sleeper);

    if (result >= 0 && !ipc_plat_user_write(c->space, user_items, items, count * sizeof(SysIpcWaitItem)))
        return SYS_EFAULT;
    return result;
}

//
// Shared memory
//

int32_t ipc_sys_shm_create(const IpcCaller* c, uint32_t size) {
    if (size == 0 || size > SYS_SHM_MAX)
        return SYS_EINVAL;
    uint32_t pages = (size + IPC_PAGE_SIZE - 1) / IPC_PAGE_SIZE;
    uint32_t bytes = pages * IPC_PAGE_SIZE;

    IPC_ENTER(c, t);
    if (ipc_shm_bytes + bytes > SYS_SHM_TOTAL_MAX)
        IPC_LEAVE(SYS_ENOMEM);

    uint32_t handle = ipc_handle_free(t, 0);
    if (handle == 0)
        IPC_LEAVE(SYS_EMFILE);

    IpcShm* shm = (IpcShm*)malloc(sizeof(IpcShm));
    if (shm == NULL)
        IPC_LEAVE(SYS_ENOMEM);
    shm->kva = ipc_plat_alloc_pages(pages);
    if (shm->kva == NULL) {
        free(shm);
        IPC_LEAVE(SYS_ENOMEM);
    }
    shm->obj.type  = SYS_IPC_TYPE_SHM;
    shm->obj.refs  = 1;
    shm->obj.dead_next = NULL;
    shm->pages     = pages;
    ipc_shm_bytes += bytes;

    ipc_handle_install(t, handle, &shm->obj);
    IPC_LEAVE((int32_t)handle);
}

// First gap of `bytes` in [IPC_MAP_BASE, IPC_MAP_END) not used by this
// process's other mappings, 0 if none
static uint32_t ipc_map_place(IpcProc* t, uint32_t bytes) {
    uint32_t candidate = IPC_MAP_BASE;
    bool moved = true;
    while (moved) {
        moved = false;
        if (candidate > IPC_MAP_END || IPC_MAP_END - candidate < bytes)
            return 0;
        for (uint32_t i = 0; i < SYS_IPC_HANDLE_MAX; i++) {
            IpcHandle* h = &t->handles[i];
            if (h->obj == NULL || h->map_addr == 0)
                continue;
            uint32_t start = h->map_addr;
            uint32_t end   = start + ((IpcShm*)h->obj)->pages * IPC_PAGE_SIZE;
            if (candidate < end && start < candidate + bytes) {
                candidate = end;
                moved = true;
            }
        }
    }
    return candidate;
}

int32_t ipc_sys_shm_map(const IpcCaller* c, uint32_t handle) {
    IPC_ENTER(c, t);
    IpcHandle* h = ipc_handle_typed(t, handle, SYS_IPC_TYPE_SHM);
    if (h == NULL)
        IPC_LEAVE(SYS_EBADF);
    if (h->map_addr != 0)
        IPC_LEAVE((int32_t)h->map_addr);

    IpcShm* shm = (IpcShm*)h->obj;
    uint32_t addr = ipc_map_place(t, shm->pages * IPC_PAGE_SIZE);
    if (addr == 0 || !ipc_plat_map(t->space, shm->kva, shm->pages, &addr))
        IPC_LEAVE(SYS_ENOMEM);

    h->map_addr = addr;
    IPC_LEAVE((int32_t)addr);
}

int32_t ipc_sys_shm_unmap(const IpcCaller* c, uint32_t handle) {
    IPC_ENTER(c, t);
    IpcHandle* h = ipc_handle_typed(t, handle, SYS_IPC_TYPE_SHM);
    if (h == NULL)
        IPC_LEAVE(SYS_EBADF);
    if (h->map_addr == 0)
        IPC_LEAVE(SYS_EINVAL);

    ipc_plat_unmap(t->space, h->map_addr, ((IpcShm*)h->obj)->pages);
    h->map_addr = 0;
    IPC_LEAVE(SYS_OK);
}

int32_t ipc_sys_info(const IpcCaller* c, uint32_t handle, uint32_t user_info) {
    IPC_ENTER(c, t);
    IpcHandle* h = ipc_handle_get(t, handle);
    if (h == NULL)
        IPC_LEAVE(SYS_EBADF);

    SysIpcInfo info;
    memset(&info, 0, sizeof(info));
    info.type = h->obj->type;

    switch (h->obj->type) {
        case SYS_IPC_TYPE_PIPE: {
            IpcEnd*  end  = (IpcEnd*)h->obj;
            IpcPipe* pipe = end->pipe;
            if (pipe->closed[end->side ^ 1])
                info.flags |= SYS_IPC_INFO_PEER_CLOSED;
            info.size    = pipe->queue[end->side].bytes;
            info.pending = pipe->queue[end->side].count;
            break;
        }
        case SYS_IPC_TYPE_PORT:
            info.pending = ((IpcPort*)h->obj)->backlog_count;
            break;
        case SYS_IPC_TYPE_SHM:
            info.size    = ((IpcShm*)h->obj)->pages * IPC_PAGE_SIZE;
            info.address = h->map_addr;
            if (h->map_addr != 0)
                info.flags |= SYS_IPC_INFO_MAPPED;
            break;
        case SYS_IPC_TYPE_INPUT:
            break;
    }

    if (!ipc_plat_user_write(c->space, user_info, &info, sizeof(info)))
        IPC_LEAVE(SYS_EFAULT);
    IPC_LEAVE(SYS_OK);
}

//
// Input
//

int32_t ipc_sys_input_open(const IpcCaller* c) {
    IPC_ENTER(c, t);
    if (ipc_input_owner != NULL)
        IPC_LEAVE(SYS_EBUSY);

    uint32_t handle = ipc_handle_free(t, 0);
    if (handle == 0)
        IPC_LEAVE(SYS_EMFILE);

    IpcInput* input = (IpcInput*)malloc(sizeof(IpcInput));
    if (input == NULL)
        IPC_LEAVE(SYS_ENOMEM);
    memset(input, 0, sizeof(IpcInput));
    input->obj.type     = SYS_IPC_TYPE_INPUT;
    input->obj.refs     = 1;
    input->sync_pending = true;

    // Whatever was queued for the kernel console is stale now
    ipc_plat_input_flush();
    ipc_input_owner = input;
    ipc_grab = true;
    ipc_plat_input_grab_changed(true);

    ipc_handle_install(t, handle, &input->obj);
    IPC_LEAVE((int32_t)handle);
}

int32_t ipc_sys_input_read(const IpcCaller* c, uint32_t handle, uint32_t user_events, uint32_t max) {
    if (max > SYS_INPUT_READ_MAX)
        max = SYS_INPUT_READ_MAX;

    IPC_ENTER(c, t);
    IpcHandle* h = ipc_handle_typed(t, handle, SYS_IPC_TYPE_INPUT);
    if (h == NULL)
        IPC_LEAVE(SYS_EBADF);
    if (max == 0)
        IPC_LEAVE(0);

    // Check the whole buffer first: events taken off the queues can't go back
    SysInputEvent events[SYS_INPUT_READ_MAX];
    memset(events, 0, sizeof(events));
    if (!ipc_plat_user_write(c->space, user_events, events, max * sizeof(SysInputEvent)))
        IPC_LEAVE(SYS_EFAULT);

    IpcInput* input = (IpcInput*)h->obj;
    uint32_t n = 0;
    if (input->sync_pending) {
        ipc_plat_input_state(&events[0]);
        events[0].type    = SYS_INPUT_MOUSE;
        events[0].key     = 0;
        events[0].time_ms = ipc_plat_time_ms();
        input->sync_pending = false;
        n = 1;
    }
    while (n < max && ipc_plat_input_pop(&events[n]))
        n++;

    if (n > 0 && !ipc_plat_user_write(c->space, user_events, events, n * sizeof(SysInputEvent)))
        IPC_LEAVE(SYS_EFAULT);
    IPC_LEAVE((int32_t)n);
}

bool ipc_input_grabbed(void) {
    return ipc_grab;
}

void ipc_input_irq(void) {
    if (ipc_grab)
        ipc_plat_wake_all();
}

//
// Process teardown
//

void ipc_process_exit(uint32_t pid) {
    // Normally one pass. A thread of the dying process that makes a new call
    // while the first table is being torn down gets a fresh table: go again.
    while (1) {
        ipc_plat_lock();
        IpcProc* t = NULL;
        for (IpcProc** link = &ipc_procs; *link != NULL; link = &(*link)->next) {
            if ((*link)->pid == pid) {
                t = *link;
                *link = t->next;
                break;
            }
        }
        if (t == NULL) {
            ipc_plat_unlock();
            return;
        }
        t->dying = true;
        ipc_plat_wake_all();
        ipc_plat_unlock();

        // Let any thread sleeping in SYS_IPC_WAIT on this table leave
        uint32_t start = ipc_plat_time_ms();
        while (1) {
            ipc_plat_lock();
            uint32_t users = t->users;
            ipc_plat_unlock();
            if (users == 0)
                break;
            if (ipc_plat_time_ms() - start >= IPC_EXIT_WAIT_MS)
                return;     // Stuck: leak the table rather than free it under a sleeper
            ipc_plat_sleep_ms(1);
        }

        ipc_plat_lock();
        ipc_close_all(t, true);
        ipc_plat_unlock();
        free(t);
    }
}

