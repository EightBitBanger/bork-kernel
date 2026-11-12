#ifndef DWM_TIMER_H
#define DWM_TIMER_H

/*
 * Timers: DWM_EVENT_TIMER for a window (or the desktop) after a delay,
 * once or every `interval_ms`.
 *
 *     dwm_timer_start(win, 1, 1000, true);        // every second
 *     ...
 *     case DWM_EVENT_TIMER:
 *         if (ev->param[0] == 1) { update the clock; dwm_window_invalidate(win, NULL); }
 *
 * A timer is named by (window, id): starting it again restarts it with the
 * new interval. A window's timers stop when it is destroyed. A repeating
 * timer that falls behind (a slow procedure) fires once and catches up
 * instead of firing for every missed interval.
 *
 * Time is milliseconds since boot (os_time_ms(), time.h).
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "event.h"

#define DWM_MAX_TIMERS  64

// False when DWM_MAX_TIMERS are running, the window does not exist, or
// interval_ms is 0
bool dwm_timer_start(dwm_handle_t window, uint32_t id, uint32_t interval_ms, bool repeat);
void dwm_timer_stop(dwm_handle_t window, uint32_t id);
bool dwm_timer_running(dwm_handle_t window, uint32_t id);

// Milliseconds since boot
uint64_t dwm_time_ms(void);

// The wall clock (os_clock_ms, the machine's clock chip): milliseconds since
// 1970-01-01 00:00:00 into *ms. False (ms untouched) on a machine without
// one. No time zones: this is the time the chip keeps, shown as it is.
bool dwm_clock_ms(uint64_t* ms);

// The same, as a date and time of day (tm_year since 1900, tm_mon 0-11,
// tm_wday 0 = Sunday, as time.h)
bool dwm_clock_now(struct tm* out);

#endif
