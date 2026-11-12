#ifndef DWM_WINDOW_H
#define DWM_WINDOW_H

/*
 * Windows.
 *
 * A window is a frame the DWM draws (border, titlebar with the title, a
 * minimize box and a close box, optionally a resize grip) around a client
 * area its procedure paints. The minimize box hides the window to its
 * taskbar button (dwm_window_minimize); clicking the button brings it back. Windows are stacked: clicking one raises it and gives it the
 * keyboard focus. TOPMOST windows always stay above the others.
 *
 * The DWM keeps no picture of a window. When part of the screen needs
 * redrawing (the window moved, something above it moved away,
 * dwm_window_invalidate), the compositor asks the procedure to paint that
 * part again with DWM_EVENT_PAINT. A window's appearance must therefore come
 * from its state, and painting must be quick and must not create or destroy
 * windows.
 *
 *     dwm_window_desc_t desc = {
 *         .title = "Hello", .x = DWM_POS_DEFAULT, .y = DWM_POS_DEFAULT,
 *         .width = 320, .height = 200, .style = DWM_STYLE_RESIZABLE,
 *         .proc = hello_proc, .user = &hello_state,
 *     };
 *     dwm_handle_t win = dwm_window_create(&desc);
 *
 * Handles are never reused while a program could still hold an old one:
 * calls on a destroyed window's handle do nothing (or return false / 0).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>

#include "event.h"

#define DWM_MAX_WINDOWS     4095    // The window table grows to this
#define DWM_TITLE_MAX       128     // Bytes, terminator included

#define DWM_TITLEBAR_HEIGHT 20      // At least: more for a large UI font (text.h)
#define DWM_BORDER_WIDTH    1
#define DWM_GRIP_SIZE       12      // Resize grip in the bottom-right corner

// Position the DWM picks (cascading from the top-left)
#define DWM_POS_DEFAULT     INT32_MIN

// Styles
#define DWM_STYLE_TOPMOST       0x0001  // Always above other windows
#define DWM_STYLE_NOBORDER      0x0002  // No frame at all (border and titlebar)
#define DWM_STYLE_NOTITLEBAR    0x0004  // Border only: no title, close box or dragging
#define DWM_STYLE_NOCLOSEBOX    0x0008  // No close box
#define DWM_STYLE_RESIZABLE     0x0010  // Resize grip in the bottom-right corner
#define DWM_STYLE_HIDDEN        0x0020  // Created hidden (dwm_window_show to show)
#define DWM_STYLE_NOFOCUS       0x0040  // Never takes the focus when created
#define DWM_STYLE_NOMINIMIZE    0x0080  // No minimize box, and can't be minimized

typedef struct {
    const char*  title;
    int32_t      x, y;              // Frame top-left (screen), or DWM_POS_DEFAULT
    int32_t      width, height;     // Client area size
    int32_t      min_width;         // Smallest client size when resizing (0: 64 x 32)
    int32_t      min_height;
    uint32_t     style;             // DWM_STYLE_*
    dwm_proc_t   proc;
    void*        user;              // Handed to every call of proc
} dwm_window_desc_t;

// Create a window. Sends CREATE, then (unless HIDDEN or NOFOCUS) raises and
// focuses it. Returns its handle, or 0 when DWM_MAX_WINDOWS are open, there
// is no memory for another, or the description is invalid (no proc, size <= 0).
dwm_handle_t dwm_window_create(const dwm_window_desc_t* desc);

// Destroy now: DESTROY is sent first. During painting the destroy is done
// right after the frame.
void dwm_window_destroy(dwm_handle_t window);

// Ask to close: queues CLOSE (which destroys the window unless handled)
void dwm_window_close(dwm_handle_t window);

bool dwm_window_exists(dwm_handle_t window);

void        dwm_window_set_title(dwm_handle_t window, const char* title);
const char* dwm_window_get_title(dwm_handle_t window);      // "" if no such window

// Frame position (screen coordinates of its top-left corner)
void dwm_window_move(dwm_handle_t window, int32_t x, int32_t y);

// Client area size (clamped to the window's minimum)
void dwm_window_resize(dwm_handle_t window, int32_t width, int32_t height);

// Frame and client rectangles in screen coordinates (either may be NULL)
bool dwm_window_get_rect(dwm_handle_t window, gfx_rect_t* frame, gfx_rect_t* client);

// Client size (0, 0 if no such window)
void dwm_window_get_size(dwm_handle_t window, int32_t* width, int32_t* height);

void dwm_window_show(dwm_handle_t window, bool visible);
bool dwm_window_visible(dwm_handle_t window);

// Repaint `rect` of the client area (client coordinates), or all of it (NULL)
void dwm_window_invalidate(dwm_handle_t window, const gfx_rect_t* rect);

// Stacking and focus
void         dwm_window_raise(dwm_handle_t window);
void         dwm_window_focus(dwm_handle_t window);     // DWM_DESKTOP: no window
dwm_handle_t dwm_window_get_focus(void);

void*        dwm_window_user(dwm_handle_t window);
uint32_t     dwm_window_style(dwm_handle_t window);
size_t       dwm_window_count(void);

// The topmost visible window whose frame contains (x, y), or DWM_DESKTOP
dwm_handle_t dwm_window_at(int32_t x, int32_t y);

#endif
