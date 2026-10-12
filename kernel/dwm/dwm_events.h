#ifndef WINDOW_EVENTS_H
#define WINDOW_EVENTS_H

#include <stdint.h>

typedef uint16_t wEvent;

// Window events

#define DWM_EVENT_REDRAW                    0x0001
#define DWM_EVENT_MOUSE                     0x0002
#define DWM_EVENT_KEYBOARD                  0x0004

#define DWM_EVENT_CLOSE                     0x0008
#define DWM_EVENT_SHOW                      0x0010
#define DWM_EVENT_HIDE                      0x0020
#define DWM_EVENT_MINIMIZE                  0x0040
#define DWM_EVENT_DESTROY                   0x0080

#define DWM_EVENT_RESIZE                    0x0100
#define DWM_EVENT_REFRESH                   0x0200
#define DWM_EVENT_DESKTOP_REFRESH           0x0400

#define DWM_EVENT_CONTEXT_MENU              0x0800

#define DWM_EVENT_FOCUS_GAINED              0x1000   // Window became the focused window
#define DWM_EVENT_FOCUS_LOST                0x2000   // Window lost focus (another window or the desktop was clicked)

// Mouse capture: after a left click in a window, the pointer stays with that
// window until the button is released. wparam packs the position relative to
// the window surface as SIGNED 16-bit values (y << 16 | x), because the
// pointer may leave the window while the button is held.
#define DWM_EVENT_MOUSE_MOVE                0x4000   // Pointer moved while captured (lparam = button state)
#define DWM_EVENT_MOUSE_UP                  0x8000   // Left button released; capture ends


// Event status

#define DWM_STATE_MOUSE_BTN_LEFT      0x00000001
#define DWM_STATE_MOUSE_BTN_RIGHT     0x00000002
#define DWM_STATE_MOUSE_DOUBLE_CLK    0x00000004

#endif
