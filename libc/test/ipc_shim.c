/*
 * Test-only: the kernel's IPC and input syscalls on a development machine.
 *
 * Built from the kernel's own ipc/ipc.c with the platform layer it needs
 * (ipc/ipc_platform.h) emulated here:
 *   - several "processes" in one: __test_ipc_set_pid() switches which
 *     process id (and address space) the next calls come from
 *   - user memory is host memory; shared memory is "mapped" at its own
 *     host address
 *   - a fake millisecond clock; a wait that would sleep runs the test's
 *     sleep hook (if any) and advances the clock instead
 *   - an input queue the test fills with __test_input_push()
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/syscall/syscall.h>
#include <kernel/ipc/ipc.h>
#include <kernel/ipc/ipc_platform.h>

// From linux_shim.c: the emulated security mask and the real pid
uint32_t __test_sec_mask(void);
uint32_t __test_real_pid(void);

static uint32_t current_pid = 0;           // 0 = the real pid
static uint32_t fake_now    = 1000;
static uint32_t wake_count  = 0;
static uint32_t grab_changes = 0;
static void   (*sleep_hook)(void) = NULL;

static uint32_t caller_pid(void) {
    return current_pid ? current_pid : __test_real_pid();
}

//
// Test controls
//

void __test_ipc_set_pid(uint32_t pid)            { current_pid = pid; }
void __test_ipc_set_sleep_hook(void (*fn)(void)) { sleep_hook = fn; }
uint32_t __test_ipc_now(void)                    { return fake_now; }
uint32_t __test_ipc_wakes(void)                  { return wake_count; }
uint32_t __test_ipc_grab_changes(void)           { return grab_changes; }
bool __test_ipc_grabbed(void)                    { return ipc_input_grabbed(); }
void __test_ipc_exit(uint32_t pid)               { ipc_process_exit(pid); }

#define INPUT_QUEUE 64
static SysInputEvent input_queue[INPUT_QUEUE];
static uint32_t input_head = 0, input_count = 0;
static SysInputEvent input_now = { 0, 0, 100, 50, 0, 0, 0, 0 };

void __test_input_set_state(int16_t x, int16_t y, uint8_t buttons, uint8_t mods) {
    input_now.x = x;
    input_now.y = y;
    input_now.buttons = buttons;
    input_now.mods = mods;
}

// Queue an event, as the keyboard / mouse IRQ would. False when full.
bool __test_input_push(const SysInputEvent* ev) {
    if (input_count == INPUT_QUEUE)
        return false;
    input_queue[(input_head + input_count) % INPUT_QUEUE] = *ev;
    input_count++;
    if (ev->type == SYS_INPUT_MOUSE) {
        input_now.x = ev->x;
        input_now.y = ev->y;
        input_now.buttons = ev->buttons;
    }
    ipc_input_irq();
    return true;
}

uint32_t __test_input_queued(void) { return input_count; }

// ipc.c's malloc / free (renamed at compile time): live allocation count
static uint32_t allocs_live = 0;
uint32_t __test_ipc_allocs_live(void) { return allocs_live; }

void* __test_ipc_malloc(size_t size) {
    void* p = malloc(size);
    if (p != NULL)
        allocs_live++;
    return p;
}

void* __test_ipc_realloc(void* p, size_t size) {
    void* q = realloc(p, size);
    if (p == NULL && q != NULL)
        allocs_live++;
    return q;
}

void __test_ipc_free(void* p) {
    if (p != NULL)
        allocs_live--;
    free(p);
}

//
// Platform layer
//

void ipc_plat_lock(void)   {}
void ipc_plat_unlock(void) {}

// Host pointers are all "user memory"; only the first page counts as unmapped
static bool user_ok(uint32_t addr, uint32_t length) {
    return addr >= 4096 && (uint64_t)addr + length <= 0x100000000ULL;
}

bool ipc_plat_user_read(void* space, void* dst, uint32_t addr, uint32_t length) {
    (void)space;
    if (!user_ok(addr, length))
        return false;
    memcpy(dst, (const void*)(uintptr_t)addr, length);
    return true;
}

bool ipc_plat_user_write(void* space, uint32_t addr, const void* src, uint32_t length) {
    (void)space;
    if (!user_ok(addr, length))
        return false;
    memcpy((void*)(uintptr_t)addr, src, length);
    return true;
}

static uint32_t pages_live = 0;
uint32_t __test_ipc_pages_live(void) { return pages_live; }

void* ipc_plat_alloc_pages(uint32_t pages) {
    void* p = malloc((size_t)pages * 4096);
    if (p != NULL) {
        memset(p, 0, (size_t)pages * 4096);
        pages_live += pages;
    }
    return p;
}

void ipc_plat_free_pages(void* kva, uint32_t pages) {
    // Poison: a test still reading through an old mapping sees garbage
    memset(kva, 0xA5, (size_t)pages * 4096);
    free(kva);
    pages_live -= pages;
}

static uint32_t maps_live = 0;
uint32_t __test_ipc_maps_live(void) { return maps_live; }

bool ipc_plat_map(void* space, void* kva, uint32_t pages, uint32_t* addr) {
    (void)space; (void)pages;
    *addr = (uint32_t)(uintptr_t)kva;
    maps_live++;
    return true;
}

void ipc_plat_unmap(void* space, uint32_t addr, uint32_t pages) {
    (void)space; (void)addr; (void)pages;
    maps_live--;
}

uint32_t ipc_plat_time_ms(void) { return fake_now; }
void ipc_plat_sleep_ms(uint32_t ms) { fake_now += ms; }

// One flag per sleeper, the way the kernel's Event remembers a signal
typedef struct Sleeper {
    volatile bool signaled;
    struct Sleeper* next;
} Sleeper;

_Static_assert(sizeof(Sleeper) <= sizeof(IpcSleeper), "IpcSleeper storage too small");

static Sleeper* sleepers = NULL;

void ipc_plat_sleep_begin(IpcSleeper* storage) {
    Sleeper* s = (Sleeper*)storage;
    s->signaled = false;
    s->next = sleepers;
    sleepers = s;
}

void ipc_plat_sleep(IpcSleeper* storage, uint32_t timeout_ms) {
    Sleeper* s = (Sleeper*)storage;
    if (s->signaled) {
        s->signaled = false;
        return;
    }
    if (sleep_hook != NULL) {
        // "Meanwhile, elsewhere": the hook acts as another process or an IRQ.
        // It runs as whatever pid it sets; put ours back afterwards.
        uint32_t pid = current_pid;
        void (*hook)(void) = sleep_hook;
        sleep_hook = NULL;          // One shot, so a hook can't loop forever
        hook();
        current_pid = pid;
        fake_now += 1;
        s->signaled = false;
        return;
    }
    if (timeout_ms == SYS_IPC_WAIT_FOREVER) {
        // Single-threaded: nobody else can ever wake us
        static const char msg[] = "ipc_shim: wait forever with nothing to wake it\n";
        extern int32_t __test_syscall(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
        __test_syscall(SYS_PRINT, (uint32_t)(uintptr_t)msg, sizeof(msg) - 1, 0, 0, 0);
        abort();
    }
    fake_now += timeout_ms;
}

void ipc_plat_sleep_end(IpcSleeper* storage) {
    for (Sleeper** link = &sleepers; *link != NULL; link = &(*link)->next) {
        if (*link == (Sleeper*)storage) {
            *link = (*link)->next;
            break;
        }
    }
}

// Nothing here is ever killed mid-call
bool ipc_plat_cancelled(void) {
    return false;
}

void ipc_plat_wake_all(void) {
    wake_count++;
    for (Sleeper* s = sleepers; s != NULL; s = s->next)
        s->signaled = true;
}

void ipc_plat_input_state(SysInputEvent* ev) {
    ev->x = input_now.x;
    ev->y = input_now.y;
    ev->buttons = input_now.buttons;
    ev->mods = input_now.mods;
    ev->wheel = 0;
}

bool ipc_plat_input_pop(SysInputEvent* ev) {
    if (input_count == 0)
        return false;
    *ev = input_queue[input_head];
    input_head = (input_head + 1) % INPUT_QUEUE;
    input_count--;
    ev->mods = input_now.mods;
    return true;
}

bool ipc_plat_input_pending(void) {
    return input_count > 0;
}

void ipc_plat_input_flush(void) {
    input_head = 0;
    input_count = 0;
}

void ipc_plat_input_grab_changed(bool grabbed) {
    (void)grabbed;
    grab_changes++;
}

//
// Terminals: what sys_spawn_tty and the descriptor 0-2 calls do, for the
// current pid
//

static IpcCaller test_caller(void) {
    uint32_t pid = caller_pid();
    IpcCaller c = { pid, (void*)(uintptr_t)(0x1000u + pid), __test_sec_mask() };
    return c;
}

// Give process `child` the caller's pipe end `handle` as its terminal
int32_t __test_tty_attach(uint32_t child, uint32_t handle) {
    IpcCaller c = test_caller();
    void* tty = NULL;
    int32_t r = ipc_tty_take(&c, handle, &tty);
    if (r != SYS_OK)
        return r;
    r = ipc_process_start(child, (void*)(uintptr_t)(0x1000u + child), tty) ? SYS_OK : SYS_ENOMEM;
    ipc_tty_drop(tty);
    return r;
}

int32_t __test_stdio_read(void* buf, uint32_t size) {
    IpcCaller c = test_caller();
    return ipc_stdio_read(&c, (uint32_t)(uintptr_t)buf, size);
}

int32_t __test_stdio_write(const void* buf, uint32_t length) {
    IpcCaller c = test_caller();
    return ipc_stdio_write(&c, (uint32_t)(uintptr_t)buf, length);
}

//
// Syscall entry (linux_shim.c routes SYS_IPC_* .. SYS_INPUT_READ here,
// after the same security check as the kernel's syscall_table)
//

int32_t __test_ipc_syscall(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    (void)a5;
    uint32_t pid = caller_pid();
    IpcCaller c = { pid, (void*)(uintptr_t)(0x1000u + pid), __test_sec_mask() };

    switch (n) {
        case SYS_IPC_CLOSE:       return ipc_sys_close(&c, a1);
        case SYS_IPC_PIPE:        return ipc_sys_pipe(&c, a1);
        case SYS_IPC_PORT_CREATE: return ipc_sys_port_create(&c, a1);
        case SYS_IPC_CONNECT:     return ipc_sys_connect(&c, a1);
        case SYS_IPC_ACCEPT:      return ipc_sys_accept(&c, a1);
        case SYS_IPC_SEND:        return ipc_sys_send(&c, a1, a2, a3, a4);
        case SYS_IPC_RECV:        return ipc_sys_recv(&c, a1, a2, a3, a4);
        case SYS_IPC_WAIT:        return ipc_sys_wait(&c, a1, a2, a3);
        case SYS_SHM_CREATE:      return ipc_sys_shm_create(&c, a1);
        case SYS_SHM_MAP:         return ipc_sys_shm_map(&c, a1);
        case SYS_SHM_UNMAP:       return ipc_sys_shm_unmap(&c, a1);
        case SYS_IPC_INFO:        return ipc_sys_info(&c, a1, a2);
        case SYS_INPUT_OPEN:      return ipc_sys_input_open(&c);
        case SYS_INPUT_READ:      return ipc_sys_input_read(&c, a1, a2, a3);
        case SYS_TTY_RESIZE:      return ipc_sys_tty_resize(&c, a1, a2, a3);
        case SYS_IOCTL:
            // As the kernel's sys_ioctl
            if (a1 >= SYS_FD_MAX) return SYS_EBADF;
            if (a1 >= SYS_FD_FIRST) return SYS_ENOTTY;
            return ipc_stdio_ioctl(&c, a2, a3);
        case SYS_INPUT_WARP: {
            // As the kernel's sys_input_warp: the IPC layer checks the handle
            int32_t r = ipc_sys_input_read(&c, a1, a4, 0);
            if (r < 0) return r;
            input_now.x = (int16_t)a2;
            input_now.y = (int16_t)a3;
            return SYS_OK;
        }
    }
    return SYS_ENOSYS;
}

