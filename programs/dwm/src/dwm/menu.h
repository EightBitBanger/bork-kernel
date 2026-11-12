#ifndef DWM_MENU_H
#define DWM_MENU_H

/*
 * Context menus.
 *
 * A menu is a table of items. Each item has a label and, when chosen,
 * either runs its `action` or (with no action) sends DWM_EVENT_MENU_COMMAND
 * with its `id` to the window the menu was opened for.
 *
 *     static const dwm_menu_item_t items[] = {
 *         { .label = "Copy",  .id = CMD_COPY },
 *         { .label = "Paste", .id = CMD_PASTE, .state = paste_state },
 *         DWM_MENU_SEP,
 *         { .label = "Close", .action = close_action },
 *     };
 *     static const dwm_menu_t menu = DWM_MENU_INIT(items, NULL);
 *
 * Right-clicking (press and release, no other button) sends
 * DWM_EVENT_CONTEXT_MENU to the window under the pointer, or to the desktop
 * procedure. Left unhandled, the DWM opens the default menu: the one set
 * with dwm_set_desktop_menu() for the desktop, dwm_set_window_menu() for
 * every window. A window with menus of its own opens them itself:
 *
 *     case DWM_EVENT_CONTEXT_MENU:
 *         dwm_menu_show(win, &menu, ev->x, ev->y);
 *         return true;
 *     case DWM_EVENT_MENU_COMMAND:
 *         if (ev->param[0] == CMD_COPY) ...
 *
 * dwm_menu_show() can be called at any other time too (a button that drops
 * a menu down, say). The items are copied when the menu opens, so a table
 * can be built on the stack, and editing a table changes the menu from its
 * next opening. `state` lets an item depend on where the menu opened: it
 * returns flags to add (HIDDEN, DISABLED, CHECKED).
 *
 * An item with a `submenu` shows ">" at the menu's right edge and opens
 * that menu beside it when the pointer rests on it (or on a click, Right
 * or Enter); it is not chosen itself. Submenus nest up to DWM_MENU_DEPTH
 * levels. One with nothing to show leaves its item dimmed.
 *
 *     static const dwm_menu_item_t sort_items[] = { { .label = "Name", .id = CMD_NAME }, ... };
 *     static const dwm_menu_t sort_menu = DWM_MENU_INIT(sort_items, NULL);
 *     ... { .label = "Sort By", .submenu = &sort_menu }, ...
 *
 * One menu (with its open submenus) is open at a time. While it is, it has
 * the mouse and the keys: Up / Down (Tab) move, Right opens a submenu,
 * Left closes one, Enter or Space chooses, Esc closes the innermost level
 * and a click outside every level closes them all (that click goes nowhere
 * else). It closes by itself if its window is destroyed or hidden.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event.h"
#include "icons.h"

#define DWM_MENU_MAX_ITEMS  32      // Shown per menu (the rest are left out)
#define DWM_MENU_LABEL_MAX  48      // Bytes, terminator included
#define DWM_MENU_DEPTH      4       // Levels open at once: the menu and 3 submenus
#define DWM_MENU_POOL       96      // Items in a menu and all its submenus together

// Item flags
#define DWM_MENU_SEPARATOR  0x0001  // A dividing line (label, id, action unused)
#define DWM_MENU_DISABLED   0x0002  // Shown dimmed, can't be chosen
#define DWM_MENU_CHECKED    0x0004  // Check mark in front of the label
#define DWM_MENU_HIDDEN     0x0008  // Left out (useful from `state`)

// Where and for whom a menu opened
typedef struct {
    dwm_handle_t window;            // DWM_DESKTOP for the desktop
    dwm_icon_t   icon;              // Desktop: the icon under the opening point (0: none)
    int32_t      x, y;              // Opening point (client coordinates; screen for the desktop)
    uint32_t     id;                // The item's id (in action and state)
    void*        user;              // The menu's `user`
} dwm_menu_context_t;

typedef void     (*dwm_menu_action_t)(const dwm_menu_context_t* ctx);
typedef uint32_t (*dwm_menu_state_t)(const dwm_menu_context_t* ctx);

typedef struct dwm_menu dwm_menu_t;

typedef struct {
    const char*       label;
    uint32_t          id;           // MENU_COMMAND param[0] (and ctx->id)
    uint32_t          flags;        // DWM_MENU_*
    dwm_menu_action_t action;       // NULL: send MENU_COMMAND instead
    dwm_menu_state_t  state;        // NULL: just `flags`
    const dwm_menu_t* submenu;      // Non-NULL: opens this beside it (id, action unused)
} dwm_menu_item_t;

struct dwm_menu {
    const dwm_menu_item_t* items;
    size_t                 count;
    void*                  user;    // Handed to actions and state as ctx->user
};

#define DWM_MENU_SEP                { .flags = DWM_MENU_SEPARATOR }
#define DWM_MENU_INIT(array, user)  { (array), sizeof(array) / sizeof((array)[0]), (user) }

// Open `menu` for `window` at (x, y): client coordinates, or screen
// coordinates for DWM_DESKTOP. It is moved to stay on the screen. Closes
// any menu already open. False when nothing would show (no items left,
// no such window, hidden window).
bool dwm_menu_show(dwm_handle_t window, const dwm_menu_t* menu, int32_t x, int32_t y);

void dwm_menu_close(void);
bool dwm_menu_is_open(void);

// Default menus for CONTEXT_MENU left unhandled (NULL: none). Kept by
// pointer: the table must stay valid.
void              dwm_set_desktop_menu(const dwm_menu_t* menu);
void              dwm_set_window_menu(const dwm_menu_t* menu);
const dwm_menu_t* dwm_get_desktop_menu(void);
const dwm_menu_t* dwm_get_window_menu(void);

#endif
