
// DWM thread, DWM lock and the two cross-thread mailboxes.
//
// Everything the DWM owns (window list, icons, dirty rects, context menus,
// drag state, draw target, theme, message queue contents) is guarded by ONE
// lock, dwm_lock. It is recursive for the owning thread, so a window
// procedure running inside the frame can call any public DWM function
// without deadlocking on itself (the old dwm_mutex was not recursive).
//
// The DWM thread is started by dwm_start() and holds dwm_lock for the whole
// of a frame. Other threads get into the DWM in one of three ways:
//
//   1. dwm_post_message()     lock-free (IRQs off for a few instructions),
//                             safe from any thread. Wakes the DWM.
//   2. dwm_invoke(fn, arg)    runs fn(arg) on the DWM thread next frame,
//                             with dwm_lock held. Use this from worker
//                             threads instead of touching DWM state.
//   3. Public API calls       take dwm_lock themselves (DWM_GUARD). Allowed
//                             from the kernel thread (program launch), but
//                             never while holding a lock that the DWM
//                             thread takes after dwm_lock (filecopy, VFS,
//                             clipboard).
//
// Going the other way, DWM code never calls kernel_event_send() directly
// (that would need kernel_lock while dwm_lock is held, which inverts the lock
// order). It queues with dwm_kernel_event_send(); the DWM thread delivers
// the queue after it releases dwm_lock.

#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/kernel.h>
#include <kernel/kernel_lock.h>
#include <kernel/events.h>
#include <kernel/mutex.h>
#include <kernel/malloc.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/boot/x86/interrupt.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/watchdog.h>
#include <kernel/util/string.h>
#include <kernel/ipc/ipc.h>
#include <kernel/console/display.h>
#include <kernel/console/mouse.h>
#include <kernel/util/list.h>

#define DWM_FRAME_MS            16
#define DWM_KEVENT_ARG_MAX      256
#define DWM_INVOKE_MAX_PENDING  512     // Back-pressure: drop beyond this

extern ThreadBlock* current_thread;

// The underlying mutex. Only dwm_lock()/dwm_unlock() touch it.
mutex_t dwm_mutex;

static uint32_t     dwm_lock_depth = 0;     // Only read/written by the owner
static ThreadBlock* dwm_thread     = NULL;

// ==========================================
// dwm_lock (recursive)
// ==========================================

void dwm_lock(void) {
    ThreadBlock* self = current_thread;

    // Before the scheduler runs there is only one context: nothing to lock
    if (self == NULL) return;

    // dwm_mutex.owner can only equal `self` if this thread set it, so this
    // unlocked read is safe
    if (dwm_mutex.locked && dwm_mutex.owner == self) {
        dwm_lock_depth++;
        return;
    }

    mutex_lock(&dwm_mutex);     // Ownership may arrive by direct handoff
    dwm_lock_depth = 1;
}

void dwm_unlock(void) {
    ThreadBlock* self = current_thread;
    if (self == NULL) return;
    if (!dwm_mutex.locked || dwm_mutex.owner != self) return;  // Not ours

    if (--dwm_lock_depth == 0)
        mutex_unlock(&dwm_mutex);
}

bool dwm_lock_held(void) {
    return current_thread != NULL && dwm_mutex.locked && dwm_mutex.owner == current_thread;
}

bool dwm_on_thread(void) {
    return dwm_thread != NULL && current_thread == dwm_thread;
}

void dwm_wake(void) {
    // Posting from the DWM thread itself needs no wake: the frame drains the
    // queue until empty, and an extra signal would make the next
    // event_wait return at once (busy loop)
    if (!dwm_on_thread())
        event_signal(&input_event);
}

// ==========================================
// Invoke queue (any thread -> DWM thread)
// ==========================================

struct DWMInvoke {
    struct DWMInvoke* next;
    DWMInvokeFn       fn;
    void*             arg;
};

static struct DWMInvoke* invoke_head = NULL;
static struct DWMInvoke* invoke_tail = NULL;
static uint32_t          invoke_count = 0;

bool dwm_invoke(DWMInvokeFn fn, void* arg) {
    if (fn == NULL) return false;

    struct DWMInvoke* node = (struct DWMInvoke*)malloc(sizeof(struct DWMInvoke));
    if (node == NULL) return false;
    node->next = NULL;
    node->fn   = fn;
    node->arg  = arg;

    uint32_t flags = irq_save();
    if (invoke_count >= DWM_INVOKE_MAX_PENDING) {
        irq_restore(flags);
        free(node);
        return false;
    }
    if (invoke_tail != NULL) invoke_tail->next = node;
    else                     invoke_head = node;
    invoke_tail = node;
    invoke_count++;
    irq_restore(flags);

    dwm_wake();
    return true;
}

// DWM thread, dwm_lock held. Runs what was queued before this call; work
// queued by the callbacks themselves waits for the next frame.
static void dwm_run_invokes(void) {
    uint32_t flags = irq_save();
    struct DWMInvoke* list = invoke_head;
    invoke_head  = NULL;
    invoke_tail  = NULL;
    invoke_count = 0;
    irq_restore(flags);

    while (list != NULL) {
        struct DWMInvoke* next = list->next;
        list->fn(list->arg);
        free(list);
        list = next;
    }
}

// ==========================================
// Kernel event outbox (DWM -> kernel thread)
// ==========================================

struct DWMKernelEvent {
    struct DWMKernelEvent* next;
    uint32_t type;
    char     a[DWM_KEVENT_ARG_MAX];
    char     b[DWM_KEVENT_ARG_MAX];
};

static struct DWMKernelEvent* kev_head = NULL;
static struct DWMKernelEvent* kev_tail = NULL;

void dwm_kernel_event_send(uint32_t type, const char* a, const char* b) {
    struct DWMKernelEvent* ev = (struct DWMKernelEvent*)malloc(sizeof(struct DWMKernelEvent));
    if (ev == NULL) return;

    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    if (a != NULL) strncpy(ev->a, a, DWM_KEVENT_ARG_MAX - 1);
    if (b != NULL) strncpy(ev->b, b, DWM_KEVENT_ARG_MAX - 1);

    uint32_t flags = irq_save();
    if (kev_tail != NULL) kev_tail->next = ev;
    else                  kev_head = ev;
    kev_tail = ev;
    irq_restore(flags);

    // Queued from outside the DWM thread: make sure it gets flushed soon
    dwm_wake();
}

// DWM thread, dwm_lock NOT held (kernel_lock comes first in the lock order)
static void dwm_flush_kernel_events(void) {
    uint32_t flags = irq_save();
    struct DWMKernelEvent* list = kev_head;
    kev_head = NULL;
    kev_tail = NULL;
    irq_restore(flags);

    if (list == NULL) return;

    kernel_lock_acquire();
    while (list != NULL) {
        struct DWMKernelEvent* next = list->next;
        kernel_event_send(list->type, list->a, list->b);
        free(list);
        list = next;
    }
    kernel_lock_release();

    event_signal(&kernel_wakeup_event);
}

// ==========================================
// The DWM thread
// ==========================================

// ==========================================
// Input grab (SYS_INPUT_OPEN)
// ==========================================
//
// While a process holds the keyboard and mouse it also owns the screen: the
// DWM leaves both input queues alone and draws nothing (dwm_update is
// skipped), but still runs invokes and delivers kernel events. When the
// grab ends, the IPC layer calls dwm_input_released(), and the next frame
// repaints everything over whatever the process left on the screen.

// DWM thread, dwm_lock held
static void dwm_repaint_all(void* arg) {
    (void)arg;
    for (struct list_node* node = workspace.window_head; node != NULL; node = node->next) {
        struct WindowObject* window = (struct WindowObject*)node->data;
        window->flags |= (DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);
    }

    // The process read every mouse event while it held the grab: pick up
    // where the mouse is now, so the cursor and hover state are right
    Point p = mouse_get_position();
    context.window_context.mouse.x = p.x;
    context.window_context.mouse.y = p.y;
    context.window_context.left_button_pressed  = mouse_get_button(MOUSE_BUTTON_LEFT);
    context.window_context.right_button_pressed = mouse_get_button(MOUSE_BUTTON_RIGHT);
    input.mouse_last = context.window_context.mouse;
    input.last_left_button_pressed  = context.window_context.left_button_pressed;
    input.last_right_button_pressed = context.window_context.right_button_pressed;

    dwm_invalidate_region(0, 0, (int16_t)display_get_width(), (int16_t)display_get_height());
}

// Called by the IPC layer (any thread, IPC lock held) when the grab ends
void dwm_input_released(void) {
    dwm_invoke(dwm_repaint_all, NULL);
}

static void dwm_thread_main(void) {
    while (1) {
        event_wait(&input_event, DWM_FRAME_MS);

        dwm_lock();

        bool grabbed = ipc_input_grabbed();

        uint16_t key;
        while (!grabbed && input_key_pop(&key)) {
            // A desktop icon rename takes the keyboard until it ends
            if (dwm_desktop_rename_key(key))
                continue;
            dwm_post_message(dwm_window_get_focus(), DWM_EVENT_KEYBOARD, key, 0);
        }

        dwm_run_invokes();
        if (!grabbed)
            dwm_update();

        dwm_unlock();

        // Outside dwm_lock: takes kernel_lock
        dwm_flush_kernel_events();

        wd_pet();
    }
}

ThreadBlock* dwm_start(ThreadPriority priority) {
    if (dwm_thread != NULL) return dwm_thread;

    uint32_t flags = irq_save();
    dwm_thread = thread_create(dwm_thread_main, priority);
    if (dwm_thread != NULL)
        wd_watch(dwm_thread, "dwm", 3000, 15000);
    irq_restore(flags);

    return dwm_thread;
}

ThreadBlock* dwm_get_thread(void) {
    return dwm_thread;
}

