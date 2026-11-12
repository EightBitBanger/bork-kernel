#ifndef DWM_NOTIFY_H
#define DWM_NOTIFY_H

/*
 * Desktop notifications ("toasts").
 *
 * Each notification is a small borderless, topmost window in the bottom
 * right corner of the screen. New ones stack on top of those already
 * showing. Clicking one dismisses it: it slides out to the right and the
 * ones above slide down into its place. A notification can also time out.
 *
 *     dwm_notify("Saved", "notes.txt", DWM_NOTIFY_DEFAULT_MS);
 *     dwm_notifyf(DWM_NOTIFY_ERROR, "Can't open", "%s: %s", name, strerror(errno));
 *
 * Notifications never take the keyboard focus: whatever had it keeps it.
 *
 * Kinds: DWM_NOTIFY_INFO is the normal look; DWM_NOTIFY_ERROR uses the
 * theme's danger colors. A sticky one (DWM_NOTIFY_STICKY) shows a small "x"
 * and stays until clicked. One identical (kind, title and message) to a
 * notification already showing is not shown twice: the one showing starts
 * its timeout again instead, so something failing in a loop can't fill
 * the screen.
 *
 * When there are too many to fit (more than 5, or taller than the screen),
 * the oldest is dismissed to make room, timed ones before sticky ones.
 * Other programs post notifications too (os/wm.h, wm_notify); each program
 * has at most 3 showing at a time.
 *
 * Keep them short: the title is one line, the message is word wrapped to at
 * most 4 lines ('\n' starts a new one) and ends in "..." when cut.
 *
 * Calls made before dwm_init() has finished are kept (up to 8) and shown
 * once it has; they return 0.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event.h"

#define DWM_NOTIFY_DEFAULT_MS   5000    // A sensible timeout
#define DWM_NOTIFY_LONG_MS      10000   // Errors: a bit longer to read
#define DWM_NOTIFY_STICKY       0       // Stays until clicked

#define DWM_NOTIFY_TITLE_MAX    64      // Bytes kept, terminator included
#define DWM_NOTIFY_TEXT_MAX     256

typedef enum {
    DWM_NOTIFY_INFO  = 0,
    DWM_NOTIFY_ERROR = 1,
} dwm_notify_kind_t;

// Show a notification. Returns its window handle (the one already showing,
// for a duplicate), or 0 when it could not be created or was kept for later.
dwm_handle_t dwm_notify(const char* title, const char* message, uint32_t timeout_ms);
dwm_handle_t dwm_notify_ex(dwm_notify_kind_t kind, const char* title, const char* message,
                           uint32_t timeout_ms);

// printf-style message; times out after DWM_NOTIFY_DEFAULT_MS (INFO) or
// DWM_NOTIFY_LONG_MS (ERROR)
dwm_handle_t dwm_notifyf(dwm_notify_kind_t kind, const char* title, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Dismiss one (as if it was clicked), or all of them
void   dwm_notify_dismiss(dwm_handle_t notification);
void   dwm_notify_dismiss_all(void);

// Notifications showing (not counting ones sliding out)
size_t dwm_notify_count(void);

#endif
