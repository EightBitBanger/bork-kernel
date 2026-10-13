
#ifndef _NOTIFICATION_WINDOW_H_
#define _NOTIFICATION_WINDOW_H_

#include <kernel/dwm/dwm.h>

// Desktop notifications ("toasts").
//
// Each notification is a small borderless, topmost window in the bottom
// right corner, just above the taskbar. New notifications stack on top of
// the ones already showing. Clicking one dismisses it: it slides out to the
// right and everything above it slides down into its place. A notification
// can also time out on its own.
//
// Notifications never take keyboard focus. Whatever window (or the desktop)
// had focus keeps it.
//
// When there are too many to fit (more than NT_MAX_VISIBLE, or taller than
// the space above the taskbar), the oldest one is dismissed to make room.
// Timed notifications go first; a sticky one is only pushed out when every
// notification showing is sticky.
//
// Kinds: DWM_NOTIFY_INFO is the normal look. DWM_NOTIFY_ERROR is drawn in
// the theme's error colors (theme.notify.error_*). A sticky notification
// shows a small "x" in its top right corner. An ERROR that is identical
// (title and message) to one already showing is not shown twice, so a
// program that crashes the same way in a loop cannot fill the screen.
//
// Threading:
//   dwm_notify / dwm_notify_dismiss*   take the DWM lock (DWM_GUARD). Fine
//                                      from window procedures and the kernel
//                                      thread, same rules as other public calls.
//   dwm_notify_post                    safe from ANY thread (worker threads,
//                                      the copy/delete threads). It copies its
//                                      arguments and runs dwm_notify on the
//                                      DWM thread through dwm_invoke.

#define DWM_NOTIFY_DEFAULT_MS   5000    // A sensible timeout
#define DWM_NOTIFY_LONG_MS      10000   // Failures: a bit longer to read
#define DWM_NOTIFY_STICKY       0       // Stays until clicked

typedef enum {
    DWM_NOTIFY_INFO  = 0,       // Normal look (what dwm_notify shows)
    DWM_NOTIFY_ERROR = 1,       // Red: theme.notify.error_*
} DWMNotifyKind;

// Show a notification. `message` is word wrapped (up to 4 lines, '\n' starts
// a new line). Returns its window handle, or 0 if it could not be created.
WindowHandle dwm_notify(const char* title, const char* message, uint32_t timeout_ms);

// Same, from any thread. Returns false if it could not be queued.
bool dwm_notify_post(const char* title, const char* message, uint32_t timeout_ms);

// The same two with a kind (dwm_notify / dwm_notify_post are DWM_NOTIFY_INFO)
WindowHandle dwm_notify_ex(DWMNotifyKind kind, const char* title, const char* message, uint32_t timeout_ms);
bool dwm_notify_post_ex(DWMNotifyKind kind, const char* title, const char* message, uint32_t timeout_ms);

// Dismiss one notification (as if it was clicked), or all of them
void dwm_notify_dismiss(WindowHandle handle);
void dwm_notify_dismiss_all(void);

// Notifications currently showing (not counting ones sliding out)
uint32_t dwm_notify_count(void);

#endif

