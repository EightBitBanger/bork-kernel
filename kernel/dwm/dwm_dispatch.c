#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/dwm/dwm_dispatch.h>

#include <kernel/arch/x86/irq.h>
#include <kernel/util/timer.h>

// The message queue is the one piece of DWM state any thread may touch
// without dwm_lock. It is protected by disabling interrupts for the few
// instructions a push or pop takes, so posting never blocks and never
// participates in the lock order. (It used to take dwm_mutex, which made
// every post from a window procedure, the copy thread or the kernel thread
// a potential lock-order edge.)

#define DWM_MSG_QUEUE_SIZE   256

static DWMMessage msg_queue[DWM_MSG_QUEUE_SIZE];
static int msg_head = 0;
static int msg_tail = 0;

bool dwm_post_message(WindowHandle hwnd, wEvent message, uint32_t wparam, int32_t lparam) {
    uint32_t now = (uint32_t)timer_get_ms();

    uint32_t flags = irq_save();

    int next_tail = (msg_tail + 1) % DWM_MSG_QUEUE_SIZE;
    if (next_tail == msg_head) {
        irq_restore(flags);
        return false; // Queue overflow
    }

    msg_queue[msg_tail].hwnd    = hwnd;
    msg_queue[msg_tail].message = message;
    msg_queue[msg_tail].wparam  = wparam;
    msg_queue[msg_tail].lparam  = lparam;
    msg_queue[msg_tail].time    = now;

    msg_tail = next_tail;

    irq_restore(flags);

    dwm_wake();
    return true;
}

bool dwm_get_message(DWMMessage* out_msg) {
    uint32_t flags = irq_save();

    if (msg_head == msg_tail) {
        irq_restore(flags);
        return false;
    }

    *out_msg = msg_queue[msg_head];
    msg_head = (msg_head + 1) % DWM_MSG_QUEUE_SIZE;

    irq_restore(flags);
    return true;
}

// DWM thread. The lock is held for the whole dispatch, including the window
// procedure: window state cannot change under a procedure halfway through.
void dwm_dispatch_message(const DWMMessage* msg) {
    DWM_GUARD();

    struct WindowObject* window = dwm_get_window_by_id(msg->hwnd);
    if (window == NULL || window->event_callback == NULL)
        return;

    WindowProcedure callback = window->event_callback;
    WindowHandle win_id = window->id;

    // Handle built-in window mechanics
    if (msg->message == DWM_EVENT_REDRAW) {
        int abs_x, abs_y;
        dwm_get_absolute_position(window, &abs_x, &abs_y);

        // Invalidate region including borders
        dwm_invalidate_region(abs_x - window->border_width,
                              abs_y - window->border_width,
                              window->w + (window->border_width * 2),
                              window->h + (window->border_width * 2));

        window->flags |= (DWM_WFLAG_REDRAW | DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);
    }

    // A veto only counts for the CLOSE it was given in
    if (msg->message == DWM_EVENT_CLOSE)
        window->flags &= ~DWM_WFLAG_KEEP_OPEN;

    callback(win_id, msg->message, msg->wparam, msg->lparam);

    // Clean up window automatically if CLOSE message was dispatched.
    // Re-check: the procedure may already have destroyed it, and a second
    // CLOSE for the same window (double click on a close box, a dialog that
    // also closes itself) must not run DESTROY twice. The procedure can also
    // keep the window open with dwm_window_cancel_close() (e.g. to ask about
    // unsaved changes first).
    if (msg->message == DWM_EVENT_CLOSE) {
        struct WindowObject* still_open = dwm_get_window_by_id(win_id);
        if (still_open == NULL) return;

        if (still_open->flags & DWM_WFLAG_KEEP_OPEN) {
            still_open->flags &= ~DWM_WFLAG_KEEP_OPEN;
            return;
        }

        callback(win_id, DWM_EVENT_DESTROY, 0, 0);
        dwm_destroy_window(win_id);
    }
}
