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
// the space above the taskbar), the oldest one at the bottom is dismissed to
// make room.
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

// Show a notification. `message` is word wrapped (up to 4 lines, '\n' starts
// a new line). Returns its window handle, or 0 if it could not be created.
WindowHandle dwm_notify(const char* title, const char* message, uint32_t timeout_ms);

// Same, from any thread. Returns false if it could not be queued.
bool dwm_notify_post(const char* title, const char* message, uint32_t timeout_ms);

// Dismiss one notification (as if it was clicked), or all of them
void dwm_notify_dismiss(WindowHandle handle);
void dwm_notify_dismiss_all(void);

// Notifications currently showing (not counting ones sliding out)
uint32_t dwm_notify_count(void);

#endif
