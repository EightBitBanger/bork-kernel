#ifndef _CONTEXT_MENU_H_
#define _CONTEXT_MENU_H_

#include <stdint.h>

#define DWM_CONTEXT_MENU_DESKTOP     0x01
#define DWM_CONTEXT_MENU_ICON        0x02
#define DWM_CONTEXT_MENU_USER        0x04

// Desktop and icon context menus, defined in the registry.
//
//   hkey_root/software/dwm/menus/<menu>/<order>
//       label   string   Text shown in the menu (15 characters fit)
//       action  string   Name of a registered action (dwm_actions.h)
//
// <menu> is "desktop" or "icon". <order> is the item key's name and decides
// the position: all-digit names sort as numbers ("10", "20", "100"), others
// alphabetically. The defaults are numbered in tens so an item can be slotted
// in between ("15") without renumbering.
//
// An item is skipped when its key lacks REGISTRY_PERMISSION_READ, its label
// is empty, or its action is not registered (so an entry for an action a
// program has not registered yet just stays hidden). At most 16 items show.
//
// Defaults: if a menu's key is missing at boot it is created with the
// built-in items. An existing key is never touched, so edits stick and an
// EMPTY key means an empty menu. Deleting the key brings the defaults back
// on the next boot. If the registry is unavailable, the built-in items are
// used directly.
//
// The menu is read from the registry on every right click, so edits take
// effect immediately.

// Forward declaration so the compiler knows this exists
struct WindowContext;

// Register the built-in actions and create missing menus in the registry.
// Call once from dwm_initiate, after the registry has been loaded.
void dwm_context_menu_init(void);

// Mouse button triggers
void dwm_icon_right_click(struct WindowContext* ctx);
void dwm_desktop_right_click(struct WindowContext* ctx);

// Event callbacks (index = item picked in the open menu)
void dwm_desktop_context_callback(struct WindowContext* ctx, uint16_t index);
void dwm_icon_context_callback(struct WindowContext* ctx, uint16_t index);

// Create a new folder / empty file in the desktop folder at the mouse
bool dwm_desktop_create_folder(void);
bool dwm_desktop_create_file(void);

#endif
