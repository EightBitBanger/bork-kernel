#ifndef EXPLORER_H
#define EXPLORER_H

/*
 * Explorer: the file browser, as a user-space program on the custom libc.
 *
 * It talks to the window manager through os/wm.h: its window, the dwm's
 * colors and the dwm's context menus. The rest it draws itself with
 * os/gfx.h: the toolbar, the icon grid, the rename box, the confirmation /
 * properties dialogs and the status bar.
 *
 * Layout values are the kernel version's, so it looks the same.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>
#include <os/wm_proto.h>

#define MAX_ITEMS               256     // Entries shown per folder
#define MAX_WINDOWS             8       // Explorer windows per process
#define MAX_TITLE_LEN           64
#define MAX_RENAME_WIDTH        140
#define NAME_LINES_MAX          4       // A selected item's full name, wrapped

// Icon slots (resource names in <home>/sys/images)
#define ICON_FOLDER             0       // "icon_folder"
#define ICON_FILE               1       // "icon_file"
#define ICON_DOCUMENT           2       // "icon_document"
#define ICON_SYSTEM             3       // "icon_system"
#define ICON_STORAGE            4       // "icon_storage"
#define ICON_COUNT              5

// Window
#define WINDOW_WIDTH            480
#define WINDOW_HEIGHT           350
#define WINDOW_MIN_WIDTH        200
#define WINDOW_MIN_HEIGHT       140

// Back button
#define BACK_BTN_X              4
#define BACK_BTN_Y              4
#define BACK_BTN_CONTAINER_X    (5 + BACK_BTN_X)
#define BACK_BTN_CONTAINER_Y    (3 + BACK_BTN_Y)
#define BACK_BTN_CONTAINER_W    18
#define BACK_BTN_CONTAINER_H    16
#define BACK_BTN_BORDER_X       (4 + BACK_BTN_X)
#define BACK_BTN_BORDER_Y       (2 + BACK_BTN_Y)
#define BACK_BTN_BORDER_W       20
#define BACK_BTN_BORDER_H       18
#define BACK_BTN_SPRITE_X       (4 + BACK_BTN_X)
#define BACK_BTN_SPRITE_Y       (0 + BACK_BTN_Y)

// Path field
#define PATH_FIELD_X            0
#define PATH_FIELD_Y            4
#define PATH_FIELD_BG_X         (35 + PATH_FIELD_X)
#define PATH_FIELD_BG_Y         (3 + PATH_FIELD_Y)
#define PATH_FIELD_BG_H         16
#define PATH_FIELD_BORDER_X     (34 + PATH_FIELD_X)
#define PATH_FIELD_BORDER_Y     (2 + PATH_FIELD_Y)
#define PATH_FIELD_BORDER_H     18
#define PATH_TEXT_X             (40 + PATH_FIELD_X)
#define PATH_TEXT_Y             (7 + PATH_FIELD_Y)

// Divider under the toolbar; the grid scrolls below it
#define NAV_DIVIDER_Y           30
#define GRID_TOP                (NAV_DIVIDER_Y + 1)

// Grid
#define NAV_X                   5
#define NAV_Y                   38
#define NAV_Y_OFF               7
#define ITEM_WIDTH              72
#define ITEM_HEIGHT             65
#define ITEM_TEXT_HEIGHT_OFF    44
#define FALLBACK_ICON_SIZE      32      // Drawn box when an icon file is missing

// Pixels the pointer moves with the left button down on an item before it
// becomes a drag (os/wm.h wm_drag_files)
#define DRAG_THRESHOLD          4

// Dialogs, status bar (context menus are the dwm's). The status bar is
// always there: a message while one shows, else what is selected (a file's
// size) or how many items the folder holds
#define NOTICE_H                18
#define NOTICE_MS               3000
#define NOTICE_LONG_MS          5000

// Colors: the dwm's theme (wm_get_theme), the same one the kernel version
// read with dwm_get_theme(). Kept current on WM_EVENT_THEME_CHANGED; starts
// as the dwm's defaults in case the dwm can't be asked.
extern wm_theme_t explorer_theme;

#endif

