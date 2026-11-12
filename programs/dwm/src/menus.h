#ifndef DESKTOP_MENUS_H
#define DESKTOP_MENUS_H

/*
 * Desktop context menus from the registry.
 *
 * Each menu is a key under MENUS_KEY, one subkey per line, the lines in
 * the order of their key names ("00", "01", ... so a line can go between
 * two others as "015"). Each line holds two string values:
 *
 *     label     what the menu shows ("New folder")
 *     command   what choosing it does: one of the desktop's own commands
 *               ("newfolder", main.c's menu_commands), "separator" for a
 *               dividing line (label unused), or "run <program>" to start
 *               <program> from <home>/sys or <home>/bin on the icon the
 *               menu opened on (the desktop folder for the desktop menu)
 *
 *     hkey_root/software/shell/menus/desktop/00   label "Paste"  command "paste"
 *     hkey_root/software/shell/menus/icon/00      label "Open"   command "open"
 *
 * "desktop" is the menu for a right-click on the desktop itself, "icon"
 * the one for a right-click on an icon. "boot install" and "boot repair"
 * write the default lines (kernel/console/commands/boot_registry.h); the
 * desktop has the same lines built in for a registry without them.
 */

#include <stddef.h>

#include "dwm/dwm.h"

#define MENUS_KEY           "hkey_root/software/shell/menus"
#define MENUS_COMMAND_MAX   128         // Bytes, terminator included

typedef struct {
    char label[DWM_MENU_LABEL_MAX];
    char command[MENUS_COMMAND_MAX];
} menus_line_t;

// The lines of menu `name` ("desktop", "icon"), at most `max` of them, in
// order. A line without a command is left out, a long label is cut. Returns
// how many, or -1 with errno when the menu isn't in the registry (ENOENT)
// or can't be read.
int menus_read(const char* name, menus_line_t* out, size_t max);

#endif
