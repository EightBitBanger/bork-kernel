#ifndef DWM_EVENT_H
#define DWM_EVENT_H

/*
 * Event system.
 *
 * Every window has a procedure. The DWM calls it with one event at a time:
 *
 *     static bool my_proc(dwm_handle_t win, const dwm_event_t* ev, void* user) {
 *         switch (ev->type) {
 *             case DWM_EVENT_PAINT:
 *                 gfx_draw_text(ev->surface, 8, 8, "hello", dwm_theme()->client.text);
 *                 return true;
 *             case DWM_EVENT_MOUSE_DOWN:
 *                 ...
 *                 dwm_window_invalidate(win, NULL);      // paint again
 *                 return true;
 *         }
 *         return false;                                  // default handling
 *     }
 *
 * Returning false asks for the default behavior. Two events have one:
 * DWM_EVENT_CLOSE destroys the window, so a procedure that wants to stay
 * open (to ask about unsaved changes, say) returns true from CLOSE; and
 * DWM_EVENT_CONTEXT_MENU opens the default context menu (menu.h), so a
 * procedure with menus of its own opens one and returns true.
 *
 * Most events are queued (dwm_post) and delivered in order by the main loop.
 * A few are delivered on the spot, from inside the call that causes them:
 * CREATE (from dwm_window_create), DESTROY (from dwm_window_destroy) and
 * PAINT (from the compositor). dwm_send delivers any event on the spot.
 *
 * Mouse coordinates are relative to the window's client area. After a
 * button goes down in a client area, the window "captures" the mouse: it
 * gets every MOUSE_MOVE and the MOUSE_UP, even outside its area, until the
 * last button is released.
 *
 * Events with no window (DWM_DESKTOP) go to the desktop procedure
 * (dwm_set_desktop_proc): clicks on the desktop (param[0] = the icon under
 * the pointer, 0 if none), keys while no window has the focus, and the
 * icon events. Keys the desktop procedure does not handle get the icon
 * defaults: Enter opens the selected icons, Ctrl+A selects them all.
 */

#include <stdbool.h>
#include <stdint.h>
#include <os/gfx.h>

typedef uint32_t dwm_handle_t;

#define DWM_DESKTOP             0u      // "No window": the desktop

// CONTEXT_MENU param[0] for windows: where the right click was
#define DWM_CONTEXT_CLIENT      0u      // The client area
#define DWM_CONTEXT_FRAME       1u      // Titlebar or border

enum {
    DWM_EVENT_NONE = 0,

    // Lifetime (CREATE and DESTROY are delivered on the spot)
    DWM_EVENT_CREATE,           // After the window exists, before it is shown
    DWM_EVENT_DESTROY,          // Last event: free userdata here
    DWM_EVENT_CLOSE,            // Close box / dwm_window_close. Not handled -> destroy

    // Drawing (delivered on the spot by the compositor)
    DWM_EVENT_PAINT,            // surface: drawing target, origin at the client's
                                // top-left, clipped to it. rect: the part that
                                // needs painting (client coordinates). Narrow
                                // the clip with gfx_intersect_clip only:
                                // gfx_set_clip could reach outside the window.
                                // Don't invalidate, create or destroy windows here.

    // Mouse (x, y: client coordinates)
    DWM_EVENT_MOUSE_MOVE,       // buttons: held
    DWM_EVENT_MOUSE_DOWN,       // button: which one, clicks: 1, or 2 for a double click
    DWM_EVENT_MOUSE_UP,         // button: which one
    DWM_EVENT_MOUSE_ENTER,      // Pointer came over the client area
    DWM_EVENT_MOUSE_LEAVE,      // ... and left it

    // Keyboard (the focused window, else the desktop)
    DWM_EVENT_KEY,              // key: character or KEY_* (os/input.h), mods: INPUT_MOD_*

    // Window state
    DWM_EVENT_FOCUS_GAINED,
    DWM_EVENT_FOCUS_LOST,
    DWM_EVENT_MOVED,            // rect: new frame rectangle (screen coordinates)
    DWM_EVENT_RESIZED,          // rect: new client size in w, h (x = y = 0)
    DWM_EVENT_SHOW,
    DWM_EVENT_HIDE,

    // Timers (dwm/timer.h): param[0] = the timer's id
    DWM_EVENT_TIMER,

    // Desktop icons (dwm/icons.h), all to the desktop procedure
    DWM_EVENT_ICON_OPEN,        // param[0] = icon: double-clicked, or Enter with it selected
    DWM_EVENT_SELECTION_CHANGED,// Some icon was selected or deselected (once per input round)
    DWM_EVENT_ICONS_MOVED,      // A drag of the selected icons ended (save the layout here)
    DWM_EVENT_ICON_RENAMED,     // param[0] = icon: a rename was kept; the text is dwm_icon_edit_result()

    // Context menus (dwm/menu.h)
    DWM_EVENT_CONTEXT_MENU,     // Right click. x, y: where (client coordinates; screen
                                // for the desktop). param[0]: desktop: the icon under
                                // the pointer (0: none); windows: DWM_CONTEXT_CLIENT or
                                // DWM_CONTEXT_FRAME. Not handled -> the default menu
                                // opens (dwm_set_desktop_menu / dwm_set_window_menu).
    // Files (to the desktop procedure): a program said it copied, moved or
    // deleted something (os/wm.h wm_fs_changed). At most one per round.
    DWM_EVENT_FS_CHANGED,

    DWM_EVENT_MENU_COMMAND,     // An item with no action was chosen. param[0]: its id,
                                // param[1]: the menu's icon (desktop), x, y: where the
                                // menu opened (as for CONTEXT_MENU)

    // File drag and drop (dwm/drag.h). mods: as the button came up (Ctrl: copy)
    DWM_EVENT_DROP,             // Files dropped here. x, y: client coordinates (screen for
                                // the desktop). param[0]: desktop: the drop target icon
                                // under the pointer (0: none). param[1]: the drop's
                                // serial, for dwm_drop_data()
    DWM_EVENT_ICONS_DROPPED,    // To the desktop procedure: the selected icons were let go
                                // over a window (param[0] = it) or a drop target icon
                                // (param[0] = DWM_DESKTOP, param[1] = the icon). x, y:
                                // screen. They are back where they were.

    // To the desktop procedure: a program said the desktop settings in the
    // registry changed (os/wm.h wm_settings_changed). At most one per round.
    DWM_EVENT_SETTINGS_CHANGED,

    // Mouse wheel, to the window under the pointer (or the one with the mouse
    // captured or locked): wheel = notches, + toward the user (scroll down).
    // x, y: client coordinates (screen for the desktop), buttons, mods.
    DWM_EVENT_MOUSE_WHEEL,

    // Files are being dragged over the window: x, y where (client
    // coordinates), mods. Sent as the pointer moves; a window can answer
    // with dwm_set_drop_folder() (drag.h) for the folder under it, which the
    // plate by the pointer then names ("Copy to Documents").
    DWM_EVENT_DRAG_OVER,

    // Anything a program defines: DWM_EVENT_USER, DWM_EVENT_USER + 1, ...
    DWM_EVENT_USER = 0x100,
};

typedef struct {
    uint16_t       type;        // DWM_EVENT_*
    dwm_handle_t   window;      // Target (DWM_DESKTOP for the desktop)
    int32_t        x, y;        // Mouse events: position
    uint8_t        buttons;     // INPUT_BTN_* held
    uint8_t        button;      // MOUSE_DOWN / MOUSE_UP: the button that changed
    uint8_t        clicks;      // MOUSE_DOWN: 1, 2 for a double click
    uint8_t        mods;        // INPUT_MOD_* (keys and mouse)
    uint16_t       key;         // KEY: character or KEY_*
    int16_t        wheel;       // MOUSE_WHEEL: notches, + toward the user (down)
    uint32_t       time_ms;     // When it happened (0 if unknown)
    gfx_rect_t     rect;        // PAINT, MOVED, RESIZED (see above)
    gfx_surface_t* surface;     // PAINT only
    uint32_t       param[2];    // USER events: anything
} dwm_event_t;

// Window procedure. `user` is the userdata given at creation.
typedef bool (*dwm_proc_t)(dwm_handle_t window, const dwm_event_t* ev, void* user);

// Queue an event for `window` (ev->window is ignored). False when the queue
// is full. Consecutive MOUSE_MOVEs to one window are merged into the newest.
bool dwm_post(dwm_handle_t window, const dwm_event_t* ev);

// Queue an event with only a type and two parameters (for USER events)
bool dwm_post_simple(dwm_handle_t window, uint16_t type, uint32_t param0, uint32_t param1);

// Deliver an event now (default handling included). Returns what the
// procedure returned; false too when the window does not exist.
bool dwm_send(dwm_handle_t window, const dwm_event_t* ev);

// The procedure that gets DWM_DESKTOP events (NULL: none)
void dwm_set_desktop_proc(dwm_proc_t proc, void* user);

#endif

