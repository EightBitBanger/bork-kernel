/*
 * Raw keyboard and mouse input -> DWM events.
 *
 * Mouse buttons, in order of who gets them:
 *
 *   menu          a context menu is open (menu.h): it gets everything, and
 *                 a press outside it closes it and goes nowhere else
 *   dragging      a titlebar or resize-grip drag in progress: moves or
 *                 resizes the window until the left button is released
 *   close box     pressed on the close box: CLOSE when released over it
 *   capture       a button went down in a client area: that window gets
 *                 every move and release until no button is held
 *   pointer lock  a focused window took the mouse (WM_REQ_POINTER_LOCK):
 *                 it gets every move and button, the cursor is hidden and
 *                 put back in the window's middle after each round of
 *                 input; x y count on from where the lock began, past the
 *                 window's edges. Losing the focus, a menu or the start
 *                 menu ends it.
 *   hover         otherwise the window under the pointer gets MOUSE_MOVE,
 *                 and MOUSE_ENTER / MOUSE_LEAVE as the pointer crosses
 *                 client areas
 *
 * A press on any part of a window raises and focuses it first. A press on
 * the desktop takes the focus away from every window and goes to the
 * desktop procedure (param[0]: the icon under the pointer). The left
 * button also works the icons (icons.h):
 *
 *   on an icon    select it (Ctrl: toggle it); moving more than a few
 *                 pixels drags every selected icon; a double click opens it.
 *                 Let go over a window or a drop target icon, the icons go
 *                 back and the desktop gets ICONS_DROPPED (drag.h). The
 *                 right button drags them too (ev.button says which: the
 *                 desktop then asks Move or Copy), and a right drag is no
 *                 right click: no CONTEXT_MENU follows it
 *   elsewhere     deselect all (not with Ctrl) and draw a selection
 *                 rectangle until the button is released
 *
 * The wheel goes to the window under the pointer, focused or not (its
 * client area or frame; not the taskbar or the desktop), as MOUSE_WHEEL in
 * client coordinates. While a window has the mouse captured or locked it
 * goes there instead; over an open start menu or tray panel, to it (it
 * scrolls its list, startmenu.c); while a menu, the Alt+Tab list, an
 * administrator prompt or a drag has the mouse, nowhere.
 *
 * A right click (the right button pressed alone and released over the same
 * window, or the desktop) queues CONTEXT_MENU after its MOUSE_UP.
 *
 * A program can turn a left or right press in its window into a file drag
 * (input_start_file_drag, from wm_drag_files): the window's buttons come up
 * at once, the DWM keeps the pointer, and that button coming up drops the
 * files on what is under it (drag.c), DROP's `button` saying which. Escape cancels it, as it does an
 * icon drag (the icons go back).
 *
 * The taskbar (taskbar.h) is above every window: a press on it goes to it
 * and nowhere else, and its release too (MODE_TASKBAR). An open start menu
 * looks at every mouse event first; a press outside it closes it and then
 * goes on as usual (except on the Start button, which only closes it).
 *
 * An administrator prompt (consent.h) comes before all of the above: while
 * one is up it gets every mouse and key event and nothing else does, a
 * pointer lock can't start, and buttons still held when it closes go
 * nowhere.
 *
 * Keys go to the Alt+Tab list while it is open (switcher.c); Alt+Tab opens
 * it and Alt+F4 closes the focused window (as its close box would), both
 * closing a menu first. Then to an open menu, else (Ctrl+Esc opens and
 * closes it) the start menu while it is open, else the focused window,
 * else the desktop procedure (or an icon's rename box, while there is one).
 * KEY_ALT_UP only ever goes to the list.
 *
 * While an icon is being renamed, a press in its box goes to the box and
 * nowhere else: it moves the box's cursor (a double click selects the whole
 * name, dragging with the left button selects). Any other press keeps the
 * new name first, then does what it always does.
 */

#include <stdio.h>          // snprintf
#include <string.h>

#include "internal.h"
#include "compositor.h"
#include "consent.h"
#include "timer.h"

#define DOUBLE_CLICK_MS     500
#define DOUBLE_CLICK_SLOP   4       // Pixels the pointer may move between the clicks

#define CURSOR_NORMAL       "cur_pointer"
#define CURSOR_RESIZE       "cur_angle"

typedef enum {
    MODE_NONE = 0,
    MODE_MOVE,              // Titlebar drag
    MODE_RESIZE,            // Grip drag
    MODE_CLOSE,             // Close box pressed, waiting for the release
    MODE_MINIMIZE,          // Minimize box pressed, waiting for the release
    MODE_CAPTURE,           // Client area owns the mouse
    MODE_ICON_PRESS,        // Left button down on an icon, not moved far yet
    MODE_ICON_DRAG,         // Dragging the selected icons
    MODE_BAND,              // Selection rectangle on the desktop
    MODE_EDIT_SELECT,       // Left button down in the rename box: dragging selects
    MODE_FILE_DRAG,         // A program's files are being dragged (drag.c)
    MODE_TASKBAR,           // A button went down on the taskbar
} input_mode_t;

// click_icon for presses in the rename box: matches no real icon, so a click
// there and one on an icon never make a double click together
#define EDIT_CLICK_ICON     ((dwm_icon_t)-2)
// ... and for presses in the start menu (its search box takes double clicks)
#define START_CLICK_ICON    ((dwm_icon_t)-3)

#define DRAG_THRESHOLD      4       // Pixels before a press on an icon becomes a drag
#define DRAG_LATE_MS        1000    // A program's drag may arrive this long after its press ended

// Each button that changed is handled in this order
static const uint8_t button_order[3] = { INPUT_BTN_LEFT, INPUT_BTN_RIGHT, INPUT_BTN_MIDDLE };

static struct {
    int32_t      x, y;              // Pointer, screen coordinates
    uint8_t      buttons;
    uint8_t      mods;
    bool         seen_mouse;        // A first mouse event arrived

    input_mode_t mode;
    dwm_handle_t target;            // Window being dragged / captured / closed
    int32_t      grab_x, grab_y;    // Pointer at the start of a drag
    int32_t      start_x, start_y;  // Window position (move) or size (resize) then

    dwm_handle_t hover;             // Client area under the pointer

    dwm_icon_t   reselect;          // Icon to select alone if the press ends without a drag
    dwm_handle_t press_window;      // Client area the last left / right press went to (input_press_window)
    uint8_t      press_button;      // ... with which button
    uint64_t     press_released;    // dwm_time_ms() when that press ended (0: still down)
    uint8_t      drag_button;       // The button an icon or file drag (or a band) is made with

    bool         swallow;           // A press closed a menu: ignore buttons until all are up
    bool         ctx_armed;         // The right button went down alone ...
    dwm_handle_t ctx_target;        // ... over this window (DWM_DESKTOP: the desktop)

    // Double clicks (on a window, the desktop, or an icon: click_icon)
    dwm_handle_t click_window;
    dwm_icon_t   click_icon;
    uint8_t      click_button;
    uint32_t     click_time;
    int32_t      click_x, click_y;
} in;

static struct {
    dwm_handle_t window;            // Has the mouse locked (DWM_DESKTOP: none)
    int32_t      vx, vy;            // Where the pointer would be, client coordinates
} lock;

#define RECENTER_SLOP       16      // Pixels the locked pointer may stray before it is put back

static void lock_end(void) {
    if (lock.window == DWM_DESKTOP)
        return;
    lock.window = DWM_DESKTOP;
    compositor_show_cursor(true);
    compositor_move_cursor(in.x, in.y);
}

static void fill_mouse(dwm_event_t* ev, uint16_t type, const dwm_window_t* w, uint32_t time) {
    memset(ev, 0, sizeof(*ev));
    ev->type    = type;
    ev->buttons = in.buttons;
    ev->mods    = in.mods;
    ev->time_ms = time;
    if (w != NULL) {
        gfx_rect_t client = window_client(w);
        ev->x = in.x - client.x;
        ev->y = in.y - client.y;
    } else {
        ev->x = in.x;
        ev->y = in.y;
    }
}

static void post_mouse_icon(dwm_handle_t target, uint16_t type, uint8_t button, uint8_t clicks,
                            uint32_t time, dwm_icon_t icon) {
    dwm_window_t* w = window_get(target);
    if (target != DWM_DESKTOP && w == NULL)
        return;
    dwm_event_t ev;
    fill_mouse(&ev, type, w, time);
    ev.button   = button;
    ev.clicks   = clicks;
    ev.param[0] = icon;
    dwm_post(target, &ev);
}

static void post_mouse(dwm_handle_t target, uint16_t type, uint8_t button, uint8_t clicks, uint32_t time) {
    post_mouse_icon(target, type, button, clicks, time, 0);
}

void input_set_pointer(int32_t x, int32_t y) {
    in.x = x;
    in.y = y;
    in.seen_mouse = true;
    compositor_move_cursor(x, y);
}

void input_window_gone(dwm_handle_t window) {
    if (in.target == window) {
        in.mode = MODE_NONE;
        in.target = DWM_DESKTOP;
    }
    if (in.hover == window)
        in.hover = DWM_DESKTOP;
    if (in.click_window == window)
        in.click_window = DWM_DESKTOP;
    if (in.ctx_target == window)
        in.ctx_armed = false;
    if (in.press_window == window)
        in.press_window = DWM_DESKTOP;
    if (lock.window == window)
        lock_end();
}

//
// Dragging files: the plate by the pointer says what letting go does
//

// The window last told where a drag is over it (DWM_EVENT_DRAG_OVER)
static struct {
    dwm_handle_t window;            // DWM_DESKTOP: none
    int32_t      x, y;
} drag_over;

static void send_drag_over(dwm_handle_t window, int32_t x, int32_t y) {
    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = DWM_EVENT_DRAG_OVER;
    ev.x       = x;
    ev.y       = y;
    ev.mods    = in.mods;
    ev.button  = in.drag_button;
    ev.time_ms = (uint32_t)dwm_time_ms();
    dwm_post(window, &ev);
}

// The drag left the window it was over (or ended): it hears so as a
// DRAG_OVER far off its client area, so it goes back to its own folder
static void drag_over_leave(void) {
    dwm_handle_t window = drag_over.window;
    drag_over.window = DWM_DESKTOP;
    if (window != DWM_DESKTOP && window_get(window) != NULL)
        send_drag_over(window, INT32_MIN, INT32_MIN);
}

// Tell `w` the drag is at the pointer, once per spot
static void post_drag_over(dwm_window_t* w) {
    gfx_rect_t client = window_client(w);
    int32_t x = in.x - client.x, y = in.y - client.y;
    if (drag_over.window == w->handle && drag_over.x == x && drag_over.y == y)
        return;
    if (drag_over.window != w->handle)
        drag_over_leave();
    drag_over.window = w->handle;
    drag_over.x = x;
    drag_over.y = y;
    send_drag_over(w->handle, x, y);
}

// The last part of a name ("/mnt/usb0/Docs" -> "Docs", "/" stays)
static const char* folder_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

static void drag_feedback(void) {
    bool icons = (in.mode == MODE_ICON_DRAG);
    if (!icons && in.mode != MODE_FILE_DRAG) {
        drag_badge(NULL, 0, 0);
        icons_drop_hover(0);
        return;
    }

    if (taskbar_contains(in.x, in.y)) {
        drag_badge(NULL, 0, 0);         // Nothing happens there
        icons_drop_hover(0);
        drag_over_leave();
        return;
    }

    size_t n = icons ? dwm_icon_selection(NULL, 0) : drag_path_count();
    dwm_window_t* window = window_top_visible_at(in.x, in.y);
    bool over_window = window != NULL;
    dwm_icon_t onto = over_window ? 0 : icons_drop_target_at(in.x, in.y, icons);
    icons_drop_hover(onto);
    if (over_window)
        post_drag_over(window);     // It may answer with the folder under the pointer
    else
        drag_over_leave();

    // Where they would land: from another drive they are copied, not moved
    const char* folder = (onto != 0) ? dwm_icon_path(onto)
                       : dwm_drop_folder(over_window ? window->handle : DWM_DESKTOP);
    bool other_drive = (icons ? icons_selection_other_drive(folder) : drag_other_drive(folder)) == 1;
    // A right drag asks on the drop, its menu's first choice named here
    // (Ctrl doesn't change that menu, so it only counts for a left drag)
    bool ctrl = (in.drag_button != INPUT_BTN_RIGHT) && (in.mods & INPUT_MOD_CTRL);
    const char* verb = (ctrl || other_drive) ? "Copy" : "Move";

    char text[DWM_ICON_LABEL_MAX + 16];
    if (onto != 0)
        snprintf(text, sizeof(text), "%s to %s", verb, dwm_icon_label(onto));
    else if (over_window && folder[0] != '\0')
        snprintf(text, sizeof(text), "%s to %s", verb, folder_name(folder));
    else if (over_window)
        snprintf(text, sizeof(text), "%s %u item%s", verb, (unsigned)n, n == 1 ? "" : "s");
    else if (!icons)
        snprintf(text, sizeof(text), "%s to the desktop", verb);
    else
        text[0] = '\0';         // Just moving icons around
    drag_badge(text, in.x, in.y);
}

void input_drag_refresh(void) {
    drag_feedback();
}

static void end_drag_feedback(void) {
    drag_over_leave();
    drag_badge(NULL, 0, 0);
    icons_drop_hover(0);
}

dwm_handle_t input_press_window(void) {
    return in.press_window;
}

bool input_start_file_drag(void) {
    dwm_handle_t source = in.press_window;
    in.press_window = DWM_DESKTOP;          // One drag per press

    uint8_t button = in.press_button;
    if (in.mode != MODE_CAPTURE || in.target != source || !(in.buttons & button)) {
        // The left button came up just before the program asked: drop where
        // it was let go. Much later it's not a drag any more. (The right
        // button's release was a right click by now: its menu is open.)
        if (button != INPUT_BTN_LEFT || in.press_released == 0 ||
            dwm_time_ms() - in.press_released > DRAG_LATE_MS)
            return false;
        drag_drop_at(in.x, in.y, in.mods, button);
        return true;
    }

    // The window sees its buttons come up; the pointer is the drag's now
    uint32_t time = (uint32_t)dwm_time_ms();
    for (int i = 0; i < 3; i++) {
        if (in.buttons & button_order[i])
            post_mouse(source, DWM_EVENT_MOUSE_UP, button_order[i], 0, time);
    }
    in.mode        = MODE_FILE_DRAG;
    in.target      = DWM_DESKTOP;
    in.drag_button = button;
    in.ctx_armed   = false;             // A right drag is no right click
    drag_feedback();
    return true;
}

void input_cancel(void) {
    uint32_t time = (uint32_t)dwm_time_ms();
    switch (in.mode) {
        case MODE_CAPTURE:
            // The window still sees its buttons come up
            for (int i = 0; i < 3; i++) {
                if (in.buttons & button_order[i])
                    post_mouse(in.target, DWM_EVENT_MOUSE_UP, button_order[i], 0, time);
            }
            break;
        case MODE_ICON_DRAG:
            icons_drag_end();
            break;
        case MODE_BAND:
            icons_band_end();
            break;
        case MODE_TASKBAR:
            taskbar_cancel();
            break;
        default:
            break;
    }
    end_drag_feedback();
    in.mode      = MODE_NONE;
    in.reselect  = 0;
    in.ctx_armed = false;
    lock_end();
}
//
// Pointer lock
//

static bool lock_valid(void) {
    return lock.window != DWM_DESKTOP && window_get(lock.window) != NULL &&
           dwm_window_get_focus() == lock.window && !dwm_menu_is_open() && !dwm_start_is_open() &&
           !consent_active() && !switcher_open();
}

bool input_lock_pointer(dwm_handle_t window, bool on) {
    if (!on) {
        if (lock.window == window)
            lock_end();
        return true;
    }
    // Never under an admin prompt: the recentering warps the pointer, which
    // a program could otherwise use to park it on Allow
    dwm_window_t* w = window_get(window);
    if (w == NULL || dwm_window_get_focus() != window || dwm_menu_is_open() || dwm_start_is_open() ||
        consent_active())
        return false;
    if (in.mode != MODE_NONE && !(in.mode == MODE_CAPTURE && in.target == window))
        return false;           // A drag of some kind is on
    in.mode = MODE_NONE;        // Its buttons come up through the lock

    gfx_rect_t client = window_client(w);
    lock.window = window;
    lock.vx = in.x - client.x;
    lock.vy = in.y - client.y;
    compositor_show_cursor(false);
    input_recenter();
    return true;
}

void input_recenter(void) {
    if (lock.window == DWM_DESKTOP)
        return;
    if (!lock_valid()) {
        lock_end();
        return;
    }
    gfx_rect_t client = window_client(window_get(lock.window));
    int32_t cx = client.x + client.w / 2, cy = client.y + client.h / 2;
    int32_t dx = in.x - cx, dy = in.y - cy;
    if (dx < RECENTER_SLOP && -dx < RECENTER_SLOP && dy < RECENTER_SLOP && -dy < RECENTER_SLOP)
        return;
    if (dwm_warp_pointer(cx, cy)) {
        in.x = cx;
        in.y = cy;
        compositor_move_cursor(cx, cy);
    }
}

static void post_locked(uint16_t type, uint8_t button, uint32_t time) {
    dwm_event_t ev;
    fill_mouse(&ev, type, NULL, time);
    ev.x      = lock.vx;
    ev.y      = lock.vy;
    ev.button = button;
    ev.clicks = type == DWM_EVENT_MOUSE_DOWN ? 1 : 0;
    dwm_post(lock.window, &ev);
}

static void route_locked(const input_event_t* ev) {
    uint32_t time = ev->time_ms;
    int32_t dx = ev->x - in.x, dy = ev->y - in.y;
    in.x = ev->x;
    in.y = ev->y;
    in.mods = ev->mods;
    lock.vx += dx;
    lock.vy += dy;
    if (dx != 0 || dy != 0)
        post_locked(DWM_EVENT_MOUSE_MOVE, 0, time);

    uint8_t changed = (uint8_t)(in.buttons ^ ev->buttons);
    for (int i = 0; i < 3; i++) {
        uint8_t b = button_order[i];
        if (!(changed & b))
            continue;
        in.buttons ^= b;
        post_locked((ev->buttons & b) ? DWM_EVENT_MOUSE_DOWN : DWM_EVENT_MOUSE_UP, b, time);
    }
}

//
// Hover and cursor shape
//

static void update_hover(uint32_t time) {
    taskbar_hover(in.x, in.y);
    dwm_window_t* w = taskbar_contains(in.x, in.y) ? NULL : window_top_visible_at(in.x, in.y);
    dwm_hit_t hit = window_hit(w, in.x, in.y);

    dwm_handle_t now = (hit == HIT_CLIENT) ? w->handle : DWM_DESKTOP;
    if (now != in.hover) {
        if (in.hover != DWM_DESKTOP)
            post_mouse(in.hover, DWM_EVENT_MOUSE_LEAVE, 0, 0, time);
        in.hover = now;
        if (now != DWM_DESKTOP)
            post_mouse(now, DWM_EVENT_MOUSE_ENTER, 0, 0, time);
    }

    // Resize arrow over a grip (or while resizing), the pointer elsewhere
    bool resize = (in.mode == MODE_RESIZE) || (in.mode == MODE_NONE && hit == HIT_GRIP);
    const image_t* shape = resize ? imagelib_get(CURSOR_RESIZE) : NULL;
    compositor_set_cursor(shape ? shape : imagelib_get(CURSOR_NORMAL));
}

//
// Buttons
//

static uint8_t click_count_on(dwm_handle_t window, dwm_icon_t icon, uint8_t button, uint32_t time) {
    bool repeat = window == in.click_window && icon == in.click_icon && button == in.click_button &&
                  time - in.click_time <= DOUBLE_CLICK_MS &&
                  in.x - in.click_x <= DOUBLE_CLICK_SLOP && in.click_x - in.x <= DOUBLE_CLICK_SLOP &&
                  in.y - in.click_y <= DOUBLE_CLICK_SLOP && in.click_y - in.y <= DOUBLE_CLICK_SLOP;

    if (repeat) {
        in.click_window = DWM_DESKTOP;      // A third click starts over
        in.click_icon   = (dwm_icon_t)-1;
        return 2;
    }
    in.click_window = window;
    in.click_icon   = icon;
    in.click_button = button;
    in.click_time   = time;
    in.click_x      = in.x;
    in.click_y      = in.y;
    return 1;
}

static uint8_t click_count(dwm_handle_t window, uint8_t button, uint32_t time) {
    return click_count_on(window, 0, button, time);
}

// A press on the desktop: icons with the left button, then the desktop proc
static void press_desktop(uint8_t button, uint32_t time) {
    dwm_window_focus(DWM_DESKTOP);

    dwm_icon_t icon = dwm_icon_at(in.x, in.y);
    bool ctrl = (in.mods & INPUT_MOD_CTRL) != 0;
    uint8_t clicks = click_count_on(DWM_DESKTOP, icon, button, time);

    if (button == INPUT_BTN_LEFT) {
        in.target      = DWM_DESKTOP;
        in.grab_x      = in.x;
        in.grab_y      = in.y;
        in.reselect    = 0;
        in.drag_button = INPUT_BTN_LEFT;

        if (icon != 0 && clicks == 2 && !ctrl) {
            icons_select_only(icon);
            icons_open(icon);
        } else if (icon != 0) {
            if (ctrl)
                icons_toggle(icon);
            else if (!dwm_icon_selected(icon))
                icons_select_only(icon);
            else
                in.reselect = icon;     // Keep the group for a drag; alone on a plain click
            if (dwm_icon_selected(icon))
                in.mode = MODE_ICON_PRESS;
        } else {
            if (!ctrl)
                icons_select_only(0);
            icons_band_start(in.x, in.y, ctrl);
            in.mode = MODE_BAND;
        }
    } else if (button == INPUT_BTN_RIGHT && icon != 0) {
        if (!dwm_icon_selected(icon))
            icons_select_only(icon);    // Right-click acts on what is under it
        // ... and moving with it held drags them (released: Move / Copy menu)
        in.target      = DWM_DESKTOP;
        in.grab_x      = in.x;
        in.grab_y      = in.y;
        in.reselect    = 0;
        in.drag_button = INPUT_BTN_RIGHT;
        in.mode        = MODE_ICON_PRESS;
    }

    post_mouse_icon(DWM_DESKTOP, DWM_EVENT_MOUSE_DOWN, button, clicks, time, icon);
}

static void press(uint8_t button, uint32_t time) {
    if (dwm_icon_editing() != 0) {
        if (in.mode == MODE_EDIT_SELECT)
            return;         // Other buttons while selecting in the box
        if (window_top_visible_at(in.x, in.y) == NULL) {
            // Count the clicks only for a press that lands in the box
            dwm_handle_t saved_window = in.click_window;
            dwm_icon_t   saved_icon   = in.click_icon;
            uint8_t      saved_button = in.click_button;
            uint32_t     saved_time   = in.click_time;
            int32_t      saved_x = in.click_x, saved_y = in.click_y;
            uint8_t clicks = click_count_on(DWM_DESKTOP, EDIT_CLICK_ICON, button, time);
            if (icons_edit_click(in.x, in.y, in.mods, clicks)) {
                in.ctx_armed = false;
                if (button == INPUT_BTN_LEFT && in.mode == MODE_NONE)
                    in.mode = MODE_EDIT_SELECT;
                return;
            }
            // Not in the box: as if this press was never counted
            in.click_window = saved_window;
            in.click_icon   = saved_icon;
            in.click_button = saved_button;
            in.click_time   = saved_time;
            in.click_x      = saved_x;
            in.click_y      = saved_y;
        }
        dwm_icon_edit_end(true);
    }

    // The right button alone may become a context menu when it comes up
    if (button == INPUT_BTN_RIGHT && in.buttons == INPUT_BTN_RIGHT) {
        in.ctx_armed  = true;
        in.ctx_target = dwm_window_at(in.x, in.y);
    } else {
        in.ctx_armed = false;
    }

    // Another button while one is captured: same window
    if (in.mode == MODE_CAPTURE) {
        post_mouse(in.target, DWM_EVENT_MOUSE_DOWN, button, click_count(in.target, button, time), time);
        return;
    }
    if (in.mode != MODE_NONE)
        return;         // Dragging: other buttons do nothing

    if (taskbar_contains(in.x, in.y)) {
        in.ctx_armed = false;           // Its own menus, on the release
        in.mode = MODE_TASKBAR;
        taskbar_press(in.x, in.y, button);  // Start opens its menu, which ends the mode
        return;
    }

    dwm_window_t* w = window_top_visible_at(in.x, in.y);
    if (button == INPUT_BTN_LEFT || button == INPUT_BTN_RIGHT)
        in.press_window = DWM_DESKTOP;      // Set again below for a client area
    if (w == NULL) {
        press_desktop(button, time);
        return;
    }

    dwm_handle_t handle = w->handle;
    dwm_window_raise(handle);
    if (!(w->style & DWM_STYLE_NOFOCUS))
        dwm_window_focus(handle);

    dwm_hit_t hit = window_hit(w, in.x, in.y);
    bool left = (button == INPUT_BTN_LEFT);

    if (left && hit == HIT_TITLEBAR) {
        in.mode = MODE_MOVE;
        in.start_x = w->x;
        in.start_y = w->y;
    } else if (left && hit == HIT_GRIP) {
        in.mode = MODE_RESIZE;
        in.start_x = w->width;
        in.start_y = w->height;
    } else if (left && hit == HIT_CLOSE) {
        in.mode = MODE_CLOSE;
    } else if (left && hit == HIT_MINIMIZE) {
        in.mode = MODE_MINIMIZE;
    } else if (hit == HIT_CLIENT) {
        in.mode = MODE_CAPTURE;
        if (left || button == INPUT_BTN_RIGHT) {
            in.press_window   = handle;
            in.press_button   = button;
            in.press_released = 0;
        }
        post_mouse(handle, DWM_EVENT_MOUSE_DOWN, button, click_count(handle, button, time), time);
    } else {
        return;         // Border, or another button on the titlebar
    }

    in.target = handle;
    in.grab_x = in.x;
    in.grab_y = in.y;
}

// An icon drag ends: over a window or a drop target icon the icons go back
// and the desktop moves the files (ICONS_DROPPED); elsewhere they stay put
static void drop_icons(uint32_t time) {
    end_drag_feedback();
    if (taskbar_contains(in.x, in.y)) {
        icons_drag_end();               // Kept above it (icons_drag_to)
        return;
    }
    dwm_window_t* w = window_top_visible_at(in.x, in.y);
    dwm_icon_t onto = (w == NULL) ? icons_drop_target_at(in.x, in.y, true) : 0;
    if (w == NULL && onto == 0) {
        icons_drag_end();
        return;
    }
    icons_drag_cancel();
    dwm_event_t ev;
    fill_mouse(&ev, DWM_EVENT_ICONS_DROPPED, NULL, time);
    ev.button   = in.drag_button;
    ev.param[0] = (w != NULL) ? w->handle : DWM_DESKTOP;
    ev.param[1] = onto;
    dwm_post(DWM_DESKTOP, &ev);
}

static void release_mode(uint8_t button, uint32_t time) {
    switch (in.mode) {
        case MODE_MOVE:
        case MODE_RESIZE:
            if (button == INPUT_BTN_LEFT)
                in.mode = MODE_NONE;
            return;

        case MODE_CLOSE:
            if (button == INPUT_BTN_LEFT) {
                dwm_window_t* w = window_get(in.target);
                if (w != NULL && window_hit(w, in.x, in.y) == HIT_CLOSE &&
                    window_top_visible_at(in.x, in.y) == w)
                    dwm_window_close(in.target);
                in.mode = MODE_NONE;
            }
            return;

        case MODE_MINIMIZE:
            if (button == INPUT_BTN_LEFT) {
                dwm_window_t* w = window_get(in.target);
                if (w != NULL && window_hit(w, in.x, in.y) == HIT_MINIMIZE &&
                    window_top_visible_at(in.x, in.y) == w)
                    dwm_window_minimize(in.target);
                in.mode = MODE_NONE;
            }
            return;

        case MODE_CAPTURE:
            post_mouse(in.target, DWM_EVENT_MOUSE_UP, button, 0, time);
            if (button == in.press_button && in.target == in.press_window)
                in.press_released = dwm_time_ms();
            if (in.buttons == 0)
                in.mode = MODE_NONE;
            return;

        case MODE_EDIT_SELECT:
            if (button == INPUT_BTN_LEFT)
                in.mode = MODE_NONE;
            return;

        case MODE_FILE_DRAG:
            if (button != in.drag_button)
                return;
            end_drag_feedback();
            in.mode = MODE_NONE;
            if (!taskbar_contains(in.x, in.y))      // Let go on the taskbar: called off
                drag_drop_at(in.x, in.y, in.mods, button);
            return;

        case MODE_TASKBAR:
            taskbar_release(in.x, in.y, button);
            if (in.buttons == 0)
                in.mode = MODE_NONE;
            return;

        case MODE_ICON_PRESS:
        case MODE_ICON_DRAG:
        case MODE_BAND:
            if (button != in.drag_button)
                return;
            if (in.mode == MODE_ICON_PRESS && in.reselect != 0)
                icons_select_only(in.reselect);     // A plain click on a selected icon
            else if (in.mode == MODE_ICON_DRAG)
                drop_icons(time);
            else if (in.mode == MODE_BAND)
                icons_band_end();
            in.mode = MODE_NONE;
            in.reselect = 0;
            post_mouse_icon(DWM_DESKTOP, DWM_EVENT_MOUSE_UP, button, 0, time, dwm_icon_at(in.x, in.y));
            return;

        case MODE_NONE: {
            // A press that went to the desktop (or the frame) ends here
            if (taskbar_contains(in.x, in.y))
                return;
            if (window_top_visible_at(in.x, in.y) == NULL)
                post_mouse_icon(DWM_DESKTOP, DWM_EVENT_MOUSE_UP, button, 0, time, dwm_icon_at(in.x, in.y));
            return;
        }
    }
}

// CONTEXT_MENU. param[0]: the icon under the pointer (desktop), or where on
// the window it was (DWM_CONTEXT_CLIENT / DWM_CONTEXT_FRAME).
static void post_context_menu(dwm_handle_t target, uint32_t time) {
    uint32_t where = 0;
    if (target == DWM_DESKTOP) {
        where = dwm_icon_at(in.x, in.y);
    } else {
        dwm_window_t* w = window_get(target);
        if (w == NULL)
            return;
        where = (window_hit(w, in.x, in.y) == HIT_CLIENT) ? DWM_CONTEXT_CLIENT : DWM_CONTEXT_FRAME;
    }
    post_mouse_icon(target, DWM_EVENT_CONTEXT_MENU, INPUT_BTN_RIGHT, 0, time, where);
}

static void release(uint8_t button, uint32_t time) {
    release_mode(button, time);

    if (button == INPUT_BTN_RIGHT && in.ctx_armed && in.buttons == 0) {
        in.ctx_armed = false;
        if (dwm_window_at(in.x, in.y) == in.ctx_target)
            post_context_menu(in.ctx_target, time);
    }
}

//
// Motion
//

static void motion(uint32_t time) {
    switch (in.mode) {
        case MODE_MOVE: {
            // Not so far down that the titlebar goes under the taskbar
            int32_t y = in.start_y + (in.y - in.grab_y);
            dwm_window_t* w = window_get(in.target);
            if (w != NULL) {
                int32_t lowest = dwm_work_area().h - window_border(w) - window_title_height(w);
                if (y > lowest)
                    y = lowest;
            }
            dwm_window_move(in.target, in.start_x + (in.x - in.grab_x), y);
            break;
        }
        case MODE_RESIZE:
            dwm_window_resize(in.target, in.start_x + (in.x - in.grab_x), in.start_y + (in.y - in.grab_y));
            break;
        case MODE_CAPTURE:
            post_mouse(in.target, DWM_EVENT_MOUSE_MOVE, 0, 0, time);
            break;
        case MODE_CLOSE:
        case MODE_MINIMIZE:
        case MODE_TASKBAR:
            break;
        case MODE_ICON_PRESS: {
            int32_t dx = in.x - in.grab_x, dy = in.y - in.grab_y;
            if (dx > DRAG_THRESHOLD || -dx > DRAG_THRESHOLD || dy > DRAG_THRESHOLD || -dy > DRAG_THRESHOLD) {
                in.mode = MODE_ICON_DRAG;
                in.reselect = 0;
                in.ctx_armed = false;       // A right drag is no right click
                icons_drag_start();
                icons_drag_to(dx, dy);
                drag_feedback();
            }
            break;
        }
        case MODE_ICON_DRAG:
            icons_drag_to(in.x - in.grab_x, in.y - in.grab_y);
            drag_feedback();
            break;
        case MODE_FILE_DRAG:
            drag_feedback();
            break;
        case MODE_BAND:
            icons_band_to(in.x, in.y);
            break;
        case MODE_EDIT_SELECT:
            icons_edit_drag(in.x);
            break;
        case MODE_NONE: {
            if (taskbar_contains(in.x, in.y))
                break;
            dwm_window_t* w = window_top_visible_at(in.x, in.y);
            if (window_hit(w, in.x, in.y) == HIT_CLIENT)
                post_mouse(w->handle, DWM_EVENT_MOUSE_MOVE, 0, 0, time);
            else if (w == NULL)
                post_mouse_icon(DWM_DESKTOP, DWM_EVENT_MOUSE_MOVE, 0, 0, time, dwm_icon_at(in.x, in.y));
            break;
        }
    }
}

//
// Wheel
//

// After the move and buttons of the same event, so x y are current
static void wheel(int16_t notches, uint32_t time) {
    if (notches == 0)
        return;

    dwm_handle_t target = DWM_DESKTOP;
    if (lock.window != DWM_DESKTOP) {
        target = lock.window;
    } else if (in.mode == MODE_CAPTURE) {
        target = in.target;
    } else if (in.mode == MODE_NONE) {
        if (taskbar_contains(in.x, in.y))
            return;
        dwm_window_t* w = window_top_visible_at(in.x, in.y);
        if (w == NULL)
            return;
        target = w->handle;
    } else {
        return;                     // A drag, a band, a held box: not now
    }

    dwm_window_t* w = window_get(target);
    if (w == NULL)
        return;
    dwm_event_t ev;
    fill_mouse(&ev, DWM_EVENT_MOUSE_WHEEL, w, time);
    if (lock.window != DWM_DESKTOP) {
        ev.x = lock.vx;
        ev.y = lock.vy;
    }
    ev.wheel = notches;
    dwm_post(target, &ev);
}

//
// Entry
//

static void route_mouse(const input_event_t* ev) {
    // An admin prompt has the mouse, all of it
    if (consent_active()) {
        lock_end();
        in.seen_mouse = true;
        in.x = ev->x;
        in.y = ev->y;
        in.mods = ev->mods;
        in.buttons = ev->buttons;
        compositor_move_cursor(in.x, in.y);
        compositor_set_cursor(imagelib_get(CURSOR_NORMAL));
        consent_mouse(in.x, in.y, ev->buttons);
        if (!consent_active() && in.buttons != 0)
            in.swallow = true;      // The click that answered it goes nowhere else
        return;
    }

    // The Alt+Tab list: a click calls it off, Alt let go chooses
    if (switcher_open()) {
        uint8_t pressed = (uint8_t)(ev->buttons & ~in.buttons);
        in.x = ev->x;
        in.y = ev->y;
        in.mods = ev->mods;
        in.buttons = ev->buttons;
        compositor_move_cursor(in.x, in.y);
        switcher_mouse(in.mods, pressed);
        if (in.buttons != 0)
            in.swallow = true;      // That click goes nowhere else
        return;
    }

    if (lock.window != DWM_DESKTOP) {
        if (lock_valid()) {
            route_locked(ev);
            wheel(ev->wheel, ev->time_ms);
            return;
        }
        lock_end();
    }

    uint32_t time = ev->time_ms;
    bool moved = !in.seen_mouse || ev->x != in.x || ev->y != in.y;
    in.seen_mouse = true;

    in.x = ev->x;
    in.y = ev->y;
    in.mods = ev->mods;
    compositor_move_cursor(in.x, in.y);

    // An open menu has the mouse
    if (dwm_menu_is_open()) {
        uint8_t pressed  = (uint8_t)(ev->buttons & ~in.buttons);
        uint8_t released = (uint8_t)(in.buttons & ~ev->buttons);
        uint8_t before = in.buttons;
        in.buttons = ev->buttons;
        compositor_set_cursor(imagelib_get(CURSOR_NORMAL));
        menu_mouse(in.x, in.y, moved, pressed, released, in.buttons);
        if (dwm_menu_is_open())
            return;
        // A right press on a tray icon that closed the menu goes on to the
        // taskbar, which opens that icon's menu on the release: one right
        // click, as when no menu was open
        if (pressed == INPUT_BTN_RIGHT && before == 0 && taskbar_contains(in.x, in.y) &&
            tray_icon_at(in.x, in.y) >= 0) {
            in.buttons = before;    // Seen as a press below
        } else {
            if (in.buttons != 0)
                in.swallow = true;  // The press that closed it goes nowhere else
            return;
        }
    }
    // An open start menu looks first; what it leaves goes on as usual
    if (dwm_start_is_open()) {
        uint8_t pressed  = (uint8_t)(ev->buttons & ~in.buttons);
        uint8_t released = (uint8_t)(in.buttons & ~ev->buttons);
        uint8_t clicks   = 0;
        for (int i = 0; i < 3 && pressed != 0 && clicks == 0; i++) {
            if (pressed & button_order[i])
                clicks = click_count_on(DWM_DESKTOP, START_CLICK_ICON, button_order[i], time);
        }
        if (start_mouse(in.x, in.y, moved, pressed, released, ev->buttons, in.mods, clicks)) {
            if (ev->wheel != 0 && dwm_start_is_open())
                start_wheel(in.x, in.y, ev->wheel, in.mods);
            in.buttons = ev->buttons;
            compositor_set_cursor(imagelib_get(CURSOR_NORMAL));
            taskbar_hover(in.x, in.y);
            if (!dwm_start_is_open() && in.buttons != 0)
                in.swallow = true;      // The press that closed it goes nowhere else
            return;
        }
    }
    if (in.swallow) {
        in.buttons = ev->buttons;
        if (in.buttons == 0)
            in.swallow = false;
        update_hover(time);
        return;
    }

    if (moved)
        motion(time);

    uint8_t changed = (uint8_t)(in.buttons ^ ev->buttons);
    for (int i = 0; i < 3; i++) {
        uint8_t b = button_order[i];
        if (!(changed & b))
            continue;
        if (ev->buttons & b) {
            in.buttons |= b;
            press(b, time);
        } else {
            in.buttons &= (uint8_t)~b;
            release(b, time);
        }
    }

    wheel(ev->wheel, time);
    update_hover(time);
}

static void route_key(const input_event_t* ev) {
    in.mods = ev->mods;

    if (consent_active()) {
        consent_key(ev->key, ev->mods);
        return;
    }

    // Alt+Tab: the window list, which then has the keys until it closes
    if (switcher_open()) {
        switcher_key(ev->key, ev->mods);
        return;
    }
    if ((ev->mods & INPUT_MOD_ALT) && (ev->key == KEY_TAB || ev->key == KEY_F4) && in.mode == MODE_NONE) {
        if (dwm_menu_is_open())
            dwm_menu_close();
        if (dwm_start_is_open())
            dwm_start_close();
        if (ev->key == KEY_TAB) {
            switcher_start((ev->mods & INPUT_MOD_SHIFT) != 0);
        } else {
            // Alt+F4: close the focused window, as its close box would
            dwm_window_t* w = window_get(dwm_window_get_focus());
            if (w != NULL && !(w->style & DWM_STYLE_NOCLOSEBOX))
                dwm_window_close(w->handle);
        }
        return;
    }
    if (ev->key == KEY_ALT_UP)
        return;                     // Only the list wants to know

    if (dwm_menu_is_open()) {
        menu_key(ev->key);
        return;
    }

    // Ctrl+Esc: the start menu (not in the middle of a drag)
    if (ev->key == KEY_ESCAPE && (ev->mods & INPUT_MOD_CTRL) &&
        (in.mode == MODE_NONE || dwm_start_is_open())) {
        if (dwm_start_is_open())
            dwm_start_close();
        else
            dwm_start_open();
        return;
    }
    if (dwm_start_is_open()) {
        start_key(ev->key, ev->mods);
        return;
    }

    // While dragging: Escape calls it off (icons go back), Ctrl shows "Copy"
    if (in.mode == MODE_FILE_DRAG || in.mode == MODE_ICON_DRAG) {
        if (ev->key == KEY_ESCAPE) {
            if (in.mode == MODE_ICON_DRAG)
                icons_drag_cancel();
            end_drag_feedback();
            in.mode = MODE_NONE;
            in.reselect = 0;
        } else {
            drag_feedback();
        }
        return;
    }

    dwm_handle_t focus = dwm_window_get_focus();
    if (focus == DWM_DESKTOP && icons_edit_key(ev->key, ev->mods))
        return;

    dwm_event_t e;
    memset(&e, 0, sizeof(e));
    e.type    = DWM_EVENT_KEY;
    e.key     = ev->key;
    e.mods    = ev->mods;
    e.x       = in.x;
    e.y       = in.y;
    e.buttons = in.buttons;
    e.time_ms = ev->time_ms;
    dwm_post(focus, &e);
}

void input_route(const input_event_t* ev) {
    if (ev->type == INPUT_MOUSE)
        route_mouse(ev);
    else if (ev->type == INPUT_KEY)
        route_key(ev);
}


