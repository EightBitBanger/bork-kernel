#ifndef DWM_TASKBAR_H
#define DWM_TASKBAR_H

/*
 * Taskbar and start menu.
 *
 * The taskbar is a strip along the bottom of the screen, drawn above every
 * window. From the left:
 *
 *   Start          opens the start menu (so does Ctrl+Esc)
 *   window buttons one per window with a titlebar, in the order they were
 *                  opened. Click: bring it to the front; click the window
 *                  already in front: minimize it (unless NOMINIMIZE, the
 *                  minimize box does the same); click a minimized one:
 *                  restore it. Right-click: the window menu
 *                  (dwm_set_window_menu) for that window.
 *   tray           programs' tray icons (tray.h; any click goes to the
 *                  program, which may answer with a menu), then the
 *                  clock: the time of day from the machine's clock chip
 *                  ("3:07 PM", dwm_clock_now in timer.h), the date in a
 *                  tooltip when hovered; "up 1:23" (hours and minutes
 *                  since boot) on a machine without one. Hovering an icon
 *                  shows its tooltip above the strip.
 *
 * The rest of the screen above it is the work area (dwm_work_area): icons
 * are placed in it, notifications stack up from its bottom, and a window
 * can't be dragged so far down that its titlebar goes under the taskbar.
 *
 * The Start button is a picture, "ui_start" from <home>/sys/images (the
 * word "Start" while it isn't installed). The start menu opens above it,
 * with a search box along its bottom:
 * whatever is typed while the menu is open goes into it, and the list below
 * turns into the files and folders whose names hold the text (every word of
 * it, upper or lower case alike), found by walking the search roots
 * (dwm_start_set_search_roots, "/mnt" by default) a little each round, so
 * the desktop keeps running while it looks. Choosing a result (click, or
 * Up / Down and Enter) hands its path to the open function
 * (dwm_start_set_open). With the box empty the list is the start items
 * (dwm_start_set_items) the desktop gave.
 *
 * Above the search box, against the right edge, is the shut down button
 * (dwm_start_set_shutdown): the picture "ui_shutdown" (24x24) from
 * <home>/sys/images, a drawn power sign while it isn't installed. It takes
 * a click only (down and up on it), never a key.
 *
 *   Esc            clear the search box; when it is empty, close the menu
 *   Up / Down      move through the list (Page Up / Page Down: a page)
 *   Enter          choose the highlighted line (the first when none is)
 *
 * A click outside the menu closes it, and then does what it always does.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>

#include "event.h"

#define DWM_TASKBAR_MIN_HEIGHT  30      // More for a large UI font

#define DWM_START_MAX_ITEMS     32
#define DWM_START_LABEL_MAX     48      // Bytes, terminator included
#define DWM_START_ARG_MAX       256

// Item flags
#define DWM_START_SEPARATOR     0x0001  // A dividing line (the rest unused)

typedef void (*dwm_start_action_t)(const char* arg);

typedef struct {
    const char*        label;
    const char*        image;           // Image-library name drawn small in front (NULL: none)
    const char*        arg;             // Handed to action (copied; NULL: "")
    uint32_t           flags;           // DWM_START_*
    dwm_start_action_t action;
} dwm_start_item_t;

#define DWM_START_SEP   { .flags = DWM_START_SEPARATOR }

// Taskbar height, and the screen above it
int32_t    dwm_taskbar_height(void);
gfx_rect_t dwm_work_area(void);

// Minimize a window to its taskbar button (hidden until its button, or
// dwm_window_show, brings it back). Only windows with a titlebar and
// without DWM_STYLE_NOMINIMIZE.
void dwm_window_minimize(dwm_handle_t window);
bool dwm_window_minimized(dwm_handle_t window);

// What the start menu lists while the search box is empty. Copied (at most
// DWM_START_MAX_ITEMS); takes effect the next time the menu is drawn.
void dwm_start_set_items(const dwm_start_item_t* items, size_t count);

// Called with a search result's absolute path when it is chosen
void dwm_start_set_open(dwm_start_action_t open);

// The shut down button: called (with "") when it is clicked, after the menu
// has closed. NULL: no button (the default).
void dwm_start_set_shutdown(dwm_start_action_t action);

// Folders the search walks (copied, at most 8). NULL / 0: "/mnt".
void dwm_start_set_search_roots(const char* const* roots, size_t count);

void dwm_start_open(void);
// The tray panel: the same place mirrored against the right edge, a little
// smaller, listing what tray icon `index`'s program sent (tray.h)
void dwm_start_open_tray(int index);
bool dwm_start_is_tray(void);        // Open, as the tray panel
// From tray.c: an icon's list changed / the icon went away
void dwm_start_tray_changed(void* owner, uint32_t id);
void dwm_start_tray_gone(void* owner, uint32_t id);
// For tests: where the panel is (empty when closed), the tray panel's
// scrolling (first row showing, and the most it can be) and its buttons
gfx_rect_t dwm_start_rect(void);
void dwm_start_tray_scroll(size_t* top, size_t* max);
// A tray panel button's place (screen; empty when it isn't showing), by
// what pressing it says: DWM_PANEL_REFRESH, _ACCEPT (Connect), _DISCONNECT
gfx_rect_t dwm_start_tray_button(uint32_t param);
void dwm_start_close(void);
bool dwm_start_is_open(void);

#endif
