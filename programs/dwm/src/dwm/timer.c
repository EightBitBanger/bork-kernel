/*
 * Timers (timer.h). A small table, scanned linearly: there are few timers
 * and they are looked at once per round of the main loop.
 */

#include <string.h>
#include <os/os.h>

#include "internal.h"
#include "timer.h"

typedef struct {
    bool         used;
    bool         repeat;
    dwm_handle_t window;
    uint32_t     id;
    uint32_t     interval;
    uint64_t     due;
} dwm_timer_t;

static dwm_timer_t timers[DWM_MAX_TIMERS];

uint64_t dwm_time_ms(void) {
    return os_time_ms();
}

bool dwm_clock_ms(uint64_t* ms) {
    return os_clock_ms(ms) == 0;
}

bool dwm_clock_now(struct tm* out) {
    uint64_t ms;
    if (!dwm_clock_ms(&ms))
        return false;
    time_t t = (time_t)(ms / 1000u);
    return gmtime_r(&t, out) != NULL;
}

static dwm_timer_t* find(dwm_handle_t window, uint32_t id) {
    for (size_t i = 0; i < DWM_MAX_TIMERS; i++) {
        if (timers[i].used && timers[i].window == window && timers[i].id == id)
            return &timers[i];
    }
    return NULL;
}

bool dwm_timer_start(dwm_handle_t window, uint32_t id, uint32_t interval_ms, bool repeat) {
    if (interval_ms == 0 || (window != DWM_DESKTOP && !dwm_window_exists(window)))
        return false;

    dwm_timer_t* t = find(window, id);
    for (size_t i = 0; t == NULL && i < DWM_MAX_TIMERS; i++) {
        if (!timers[i].used)
            t = &timers[i];
    }
    if (t == NULL)
        return false;

    t->used     = true;
    t->repeat   = repeat;
    t->window   = window;
    t->id       = id;
    t->interval = interval_ms;
    t->due      = dwm_time_ms() + interval_ms;
    return true;
}

void dwm_timer_stop(dwm_handle_t window, uint32_t id) {
    dwm_timer_t* t = find(window, id);
    if (t != NULL)
        memset(t, 0, sizeof(*t));
}

bool dwm_timer_running(dwm_handle_t window, uint32_t id) {
    return find(window, id) != NULL;
}

int32_t timer_next_timeout(void) {
    uint64_t now = dwm_time_ms();
    bool any = false;
    uint64_t next = 0;
    for (size_t i = 0; i < DWM_MAX_TIMERS; i++) {
        if (timers[i].used && (!any || timers[i].due < next)) {
            next = timers[i].due;
            any = true;
        }
    }
    if (!any)
        return -1;
    if (next <= now)
        return 0;
    uint64_t wait = next - now;
    return (wait > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int32_t)wait;
}

void timer_fire_due(void) {
    uint64_t now = dwm_time_ms();
    for (size_t i = 0; i < DWM_MAX_TIMERS; i++) {
        dwm_timer_t* t = &timers[i];
        if (!t->used || t->due > now)
            continue;

        dwm_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type     = DWM_EVENT_TIMER;
        ev.param[0] = t->id;
        ev.time_ms  = (uint32_t)now;
        dwm_post(t->window, &ev);

        if (t->repeat) {
            t->due += t->interval;
            if (t->due <= now)
                t->due = now + t->interval;     // Fell behind: catch up, don't burst
        } else {
            memset(t, 0, sizeof(*t));
        }
    }
}

void timer_window_gone(dwm_handle_t window) {
    for (size_t i = 0; i < DWM_MAX_TIMERS; i++) {
        if (timers[i].used && timers[i].window == window)
            memset(&timers[i], 0, sizeof(timers[i]));
    }
}
