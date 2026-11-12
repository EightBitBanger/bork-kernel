#ifndef DWM_INTERNAL_H
#define DWM_INTERNAL_H

/*
 * State and helpers shared by the DWM's own modules. Not for programs.
 *
 *   window.c       window table, stacking order, geometry, focus
 *   event.c        the event queue and delivery
 *   input.c        raw keyboard / mouse input -> events, dragging, resizing
 *   compositor.c   dirty rectangles -> painted, presented screen regions
 *   icons.c        desktop icons, selection, icon drag, selection rectangle
 *   menu.c         context menus
 *   timer.c        timers
 *   notify.c       notifications
 *   consent.c      the administrator prompt (consent.h)
 *   clipboard.c    the system clipboard
 *   drag.c         file drag and drop: the paths, the drop, the plate
 *   taskbar.c      the taskbar, its buttons, minimizing
 *   tray.c         the system tray: program icons on the taskbar
 *   startmenu.c    the start menu and its search box
 *   switcher.c     the Alt+Tab window list
 *   search.c       walking the folders for the start menu's search
 *   text.c         text in the UI font
 *   dwm.c          start-up, the main loop, shutdown
 */

#include <stdbool.h>
#include <stdint.h>
#include <os/gfx.h>
#include <os/input.h>

#include "clipboard.h"
#include "drag.h"
#include "event.h"
#include "icons.h"
#include "menu.h"
#include "notify.h"
#include "taskbar.h"
#include "text.h"
#include "tray.h"
#include "window.h"
#include "../resources/imagelib.h"

typedef struct dwm_window dwm_window_t;

struct dwm_window {
    dwm_handle_t  handle;           // 0: free slot
    uint32_t      style;
    char          title[DWM_TITLE_MAX];
    char          drop_folder[DWM_DROP_FOLDER_MAX];     // dwm_set_drop_folder, "": not known

    int32_t       x, y;             // Frame top-left (screen)
    int32_t       width, height;    // Client size
    int32_t       min_width, min_height;

    bool          visible;
    bool          minimized;        // Hidden by its taskbar button (taskbar.c)
    bool          doomed;           // Destroy requested while painting
    bool          destroying;       // DESTROY is being delivered

    dwm_proc_t    proc;
    void*         user;

    dwm_window_t* below;            // Stacking order, bottom to top
    dwm_window_t* above;
};

// Where a point lands on a window
typedef enum {
    HIT_NONE = 0,
    HIT_CLIENT,
    HIT_TITLEBAR,
    HIT_CLOSE,
    HIT_MINIMIZE,
    HIT_BORDER,
    HIT_GRIP,
} dwm_hit_t;

//
// window.c
//

dwm_window_t* window_get(dwm_handle_t handle);      // NULL if no such window
dwm_window_t* window_bottom(void);                  // Stacking order, for painting
dwm_window_t* window_top_visible_at(int32_t x, int32_t y);
dwm_hit_t     window_hit(const dwm_window_t* w, int32_t x, int32_t y);

int32_t       window_border(const dwm_window_t* w);
int32_t       window_title_height(const dwm_window_t* w);
gfx_rect_t    window_frame(const dwm_window_t* w);
gfx_rect_t    window_client(const dwm_window_t* w);
gfx_rect_t    window_titlebar(const dwm_window_t* w);   // Empty without one
gfx_rect_t    window_close_box(const dwm_window_t* w);  // Empty without one
gfx_rect_t    window_minimize_box(const dwm_window_t* w);   // Empty without one
bool          window_can_minimize(const dwm_window_t* w);   // Titlebar, not NOMINIMIZE
gfx_rect_t    window_grip(const dwm_window_t* w);       // Empty unless RESIZABLE
bool          window_focused(const dwm_window_t* w);

// Call a window's procedure (or the desktop's, for DWM_DESKTOP). False if
// there is none or it did not handle the event.
bool          window_call(dwm_handle_t handle, const dwm_event_t* ev);

// Destroy windows whose destroy came in while painting
void          window_reap(void);

// Destroy every window (shutdown)
void          window_destroy_all(void);

//
// event.c
//

bool          event_pending(void);
void          event_dispatch(void);     // Everything queued now (not what it queues)
void          event_flush(void);        // Drop everything queued

//
// dwm.c
//

bool          dwm_warp_pointer(int32_t x, int32_t y);       // Move the kernel's pointer (false: can't)

//
// input.c
//

void          input_route(const input_event_t* ev);        // One raw input event
void          input_window_gone(dwm_handle_t window);       // Drop drag / capture / hover
void          input_set_pointer(int32_t x, int32_t y);      // Where the pointer starts
void          input_cancel(void);                           // End a drag / capture (a menu opened)
bool          input_lock_pointer(dwm_handle_t window, bool on);    // WM_REQ_POINTER_LOCK: false if refused
void          input_recenter(void);                         // After a round of input: pointer back to a lock

// The window the last left or right press in a client area went to
// (DWM_DESKTOP: none, or it was used up), for a program asking to drag
// files from it
dwm_handle_t  input_press_window(void);
// That press becomes a file drag (the paths are drag.c's already): the
// window gets its MOUSE_UP now, the drop comes where the button comes up.
// When the button came up just before, the drop happens at the pointer at
// once. False (nothing happens) when the press ended too long ago.
bool          input_start_file_drag(void);

//
// icons.c
//

void          icons_select_only(dwm_icon_t icon);           // 0: deselect all
void          icons_toggle(dwm_icon_t icon);
void          icons_open(dwm_icon_t icon);                  // Post ICON_OPEN
void          icons_open_selection(void);
bool          icons_default_key(const dwm_event_t* ev);     // Enter, Ctrl+A
void          icons_notify(void);                           // SELECTION_CHANGED, once per round
bool          icons_edit_key(uint16_t key, uint8_t mods);   // While renaming: true (every key)
bool          icons_edit_click(int32_t x, int32_t y, uint8_t mods, uint8_t clicks);   // A press in the rename box
void          icons_edit_drag(int32_t x);                   // ... moving with the left button held

void          icons_drag_start(void);                       // The selected icons
void          icons_drag_to(int32_t dx, int32_t dy);        // Offset from the start
void          icons_drag_end(void);
void          icons_drag_cancel(void);                      // Back where the drag started

// The topmost drop target icon (dwm_icon_set_drop_target) under (x, y), or
// 0. skip_selected: not one of the selected icons (they are being dragged).
dwm_icon_t    icons_drop_target_at(int32_t x, int32_t y, bool skip_selected);
void          icons_drop_hover(dwm_icon_t icon);            // Highlight it (0: none)
int           icons_selection_other_drive(const char* folder);  // 1 any selected on another drive, 0 none, -1 not known

void          icons_band_start(int32_t x, int32_t y, bool additive);
void          icons_band_to(int32_t x, int32_t y);
void          icons_band_end(void);

void          icons_paint(gfx_surface_t* s, gfx_rect_t area);

//
// menu.c
//

// The mouse while a menu is open: buttons that went down / up, those held
void          menu_mouse(int32_t x, int32_t y, bool moved, uint8_t pressed, uint8_t released, uint8_t held);
void          menu_key(uint16_t key);                       // A key while a menu is open
bool          menu_default(const dwm_event_t* ev);          // Unhandled CONTEXT_MENU
void          menu_window_gone(dwm_handle_t window);        // Destroyed or hidden
void          menu_close_for_user(const void* user);        // Close it if it is that menu (its `user`)
void          menu_paint(gfx_surface_t* s, gfx_rect_t area);

//
// timer.c
//

int32_t       timer_next_timeout(void);                     // ms to the next timer, -1: none
void          timer_fire_due(void);                         // Post TIMER for each one due
void          timer_window_gone(dwm_handle_t window);

//
// notify.c
//

// owner: 0 for the DWM itself, else the posting program's pid (it gets at
// most a few showing at once)
dwm_handle_t  notify_show(dwm_notify_kind_t kind, const char* title, const char* message,
                          uint32_t timeout_ms, uint32_t owner);
void          notify_start(void);                           // dwm_init done: show what was kept
void          notify_stop(void);                            // Shutdown (windows already gone)
void          notify_tick(void);                            // Timeouts and sliding, once per round
int32_t       notify_next_timeout(void);                    // ms to the next step, -1: none

//
// text.c
//

void          text_shutdown(void);                          // Drop the glyph cache

//
// clipboard.c
//

// dwm_clip_set for a program (owner = its pid)
bool          clip_set_from(const dwm_clip_info_t* info, const void* data, uint32_t owner);

// A new shared memory handle holding a copy of the data, for one program
// (the caller closes it). 0 when there is no data, -1 with errno.
int           clip_share(void);

//
// drag.c
//

// Replace what is dragged with `info` (DWM_CLIP_FILES) and its paths
// (copied). False with errno EINVAL (not files, a line not an absolute
// path) or ENOMEM.
bool          drag_set_from(const dwm_clip_info_t* info, const void* data, uint32_t owner);
uint32_t      drag_serial(void);                            // 0: nothing dragged yet
size_t        drag_path_count(void);
int           drag_other_drive(const char* folder);
void          input_drag_refresh(void);     // A drop folder changed: the plate again (nothing when no drag)       // Dragged paths: 1 any on another drive, 0 none, -1 not known
// A new shared memory handle holding a copy of drop `serial`'s paths (the
// caller closes it); -1 with errno (ENOENT: that drop is gone)
int           drag_share(uint32_t serial);
// Drop what is dragged at (x, y), screen: DWM_EVENT_DROP to what is there
void          drag_drop_at(int32_t x, int32_t y, uint8_t mods, uint8_t button);
// The plate by the pointer (text NULL: hide it), near (x, y)
void          drag_badge(const char* text, int32_t x, int32_t y);
void          drag_paint(gfx_surface_t* s, gfx_rect_t area);

//
// taskbar.c
//

gfx_rect_t    taskbar_rect(void);                           // Screen
gfx_rect_t    taskbar_start_rect(void);                     // The Start button
gfx_rect_t    taskbar_start_hit_rect(void);                 // Where a click hits it (to the screen edges)
bool          taskbar_contains(int32_t x, int32_t y);
void          taskbar_tick(void);                           // Follow the windows, once per round
int32_t       taskbar_next_timeout(void);                   // ms to the next uptime minute
void          taskbar_hover(int32_t x, int32_t y);          // The pointer moved (highlight)
void          taskbar_press(int32_t x, int32_t y, uint8_t button);
void          taskbar_release(int32_t x, int32_t y, uint8_t button);
void          taskbar_cancel(void);                         // Forget a press
bool          taskbar_pressing(void);                       // A button went down on it, still held
void          taskbar_paint(gfx_surface_t* s, gfx_rect_t area);
void          taskbar_shutdown(void);

// `image` shrunk to fit size x size (kept for the next call), centered there
void          small_image_draw(gfx_surface_t* s, const image_t* image, int32_t x, int32_t y, int32_t size);

//
// startmenu.c
//

// The mouse, before anything else sees it: true when the start menu took it
bool          start_mouse(int32_t x, int32_t y, bool moved, uint8_t pressed, uint8_t released,
                          uint8_t held, uint8_t mods, uint8_t clicks);
void          start_key(uint16_t key, uint8_t mods);        // A key while it is open
// The wheel at (x, y) while it is open: true when the start menu took it
bool          start_wheel(int32_t x, int32_t y, int16_t notches, uint8_t mods);
void          start_tick(void);                             // Search on, once per round
bool          start_busy(void);                             // Searching: don't sleep
void          start_paint(gfx_surface_t* s, gfx_rect_t area);

//
// switcher.c
//

bool          switcher_open(void);
void          switcher_start(bool back);                    // Alt+Tab (back: Alt+Shift+Tab); again: next
void          switcher_key(uint16_t key, uint8_t mods);     // A key while it is open
// The mouse while it is open: a press calls it off, an event without Alt
// held chooses. False when it is not open.
bool          switcher_mouse(uint8_t mods, uint8_t pressed);
void          switcher_choose(void);                        // Bring the picked window to the front
void          switcher_cancel(void);
void          switcher_paint(gfx_surface_t* s, gfx_rect_t area);

//
// compositor.c: see compositor.h
//

#endif


