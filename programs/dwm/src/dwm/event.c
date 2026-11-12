/*
 * Event queue and delivery (event.h).
 *
 * One ring buffer for every window. The main loop hands out what is queued
 * at the start of event_dispatch(); events those procedures post wait for
 * the next round, so a procedure that keeps posting to itself cannot stall
 * painting or input.
 */

#include <string.h>

#include "internal.h"

#define QUEUE_SIZE  256

static dwm_event_t queue[QUEUE_SIZE];
static size_t      head  = 0;
static size_t      count = 0;

bool dwm_post(dwm_handle_t window, const dwm_event_t* ev) {
    if (ev == NULL)
        return false;

    dwm_event_t e = *ev;
    e.window = window;

    // Mouse moves pile up fast: only the newest one matters
    if (e.type == DWM_EVENT_MOUSE_MOVE && count > 0) {
        dwm_event_t* last = &queue[(head + count - 1) % QUEUE_SIZE];
        if (last->type == DWM_EVENT_MOUSE_MOVE && last->window == window && last->buttons == e.buttons) {
            *last = e;
            return true;
        }
    }

    if (count == QUEUE_SIZE)
        return false;
    queue[(head + count) % QUEUE_SIZE] = e;
    count++;
    return true;
}

bool dwm_post_simple(dwm_handle_t window, uint16_t type, uint32_t param0, uint32_t param1) {
    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type     = type;
    ev.param[0] = param0;
    ev.param[1] = param1;
    return dwm_post(window, &ev);
}

// One event, with default handling
static bool deliver(const dwm_event_t* ev) {
    bool handled = window_call(ev->window, ev);
    if (handled)
        return true;

    switch (ev->type) {
        case DWM_EVENT_CLOSE:
            dwm_window_destroy(ev->window);
            break;
        case DWM_EVENT_CONTEXT_MENU:
            handled = menu_default(ev);
            break;
        case DWM_EVENT_KEY:
            if (ev->window == DWM_DESKTOP)
                handled = icons_default_key(ev);
            break;
    }
    return handled;
}

bool dwm_send(dwm_handle_t window, const dwm_event_t* ev) {
    if (ev == NULL)
        return false;
    dwm_event_t e = *ev;
    e.window = window;
    return deliver(&e);
}

bool event_pending(void) {
    return count > 0;
}

void event_dispatch(void) {
    size_t n = count;
    while (n-- > 0 && count > 0) {
        dwm_event_t ev = queue[head];
        head = (head + 1) % QUEUE_SIZE;
        count--;
        deliver(&ev);       // Windows destroyed since: window_call finds nothing
    }
}

void event_flush(void) {
    head = count = 0;
}
