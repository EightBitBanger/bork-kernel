#ifndef DWM_TRAY_H
#define DWM_TRAY_H

/*
 * System tray: small icons programs put on the taskbar, left of the uptime.
 *
 * Programs ask for them over the window server (WM_REQ_TRAY_*, see
 * os/wm_proto.h); server.c checks the request, maps the picture the program
 * sent and hands it here. Every icon has an owner (the server's connection,
 * an opaque pointer here) and the pid of the process that added it:
 *
 *   - only that process can change or remove it (EPERM)
 *   - tray_remove_owner() takes all of an owner's icons away at once; the
 *     server calls it when the connection closes, which the kernel does when
 *     the process exits or crashes
 *   - each process has at most DWM_TRAY_PER_PROCESS icons, however many
 *     connections it opens, and the tray at most DWM_TRAY_MAX
 *
 * The picture is copied (and fit into DWM_TRAY_ICON_PX square) when it
 * comes, so the program's memory is never read again after the request.
 * Tooltips are copied too, with control characters turned into spaces.
 *
 * The taskbar (taskbar.c) owns the strip: it gives the tray the part of it
 * the icons go in (tray_layout), forwards presses and releases on them, and
 * calls tray_paint. A click (press and release on the same icon) goes to the
 * click handler the server set, which sends WM_EVENT_TRAY to the owner.
 * Within DWM_TRAY_MENU_MS of a click the owner may open one menu above that
 * icon (tray_menu); its choice comes back through the command handler.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>

#include "menu.h"

#define DWM_TRAY_ICON_PX        16      // Drawn size, a side
#define DWM_TRAY_SOURCE_MAX     64      // Largest picture accepted, a side
#define DWM_TRAY_TIP_MAX        64      // Bytes, terminator included
#define DWM_TRAY_PER_PROCESS    4
#define DWM_TRAY_MAX            24
#define DWM_TRAY_MENU_MS        3000    // A menu may follow a click this long
#define DWM_TRAY_MENU_MAX       10      // Items in a tray menu

typedef struct {
    uint8_t    button;          // INPUT_BTN_*
    uint8_t    clicks;          // 2: the second click of a double click
    int32_t    x, y;            // The pointer, screen
    gfx_rect_t rect;            // The icon, screen
} dwm_tray_click_t;

typedef void (*dwm_tray_click_fn)(void* owner, uint32_t id, const dwm_tray_click_t* click);
typedef void (*dwm_tray_command_fn)(void* owner, uint32_t id, uint32_t command);

// Where clicks and menu choices go (the window server)
void     tray_set_handlers(dwm_tray_click_fn click, dwm_tray_command_fn command);

// A new icon: `pixels` is w x h 0xAARRGGBB, row after row (copied). `tip`
// may be NULL. 0, or EINVAL (bad size, no pixels), EEXIST (the owner has
// that id), EMFILE (the process has DWM_TRAY_PER_PROCESS), ENOSPC (full),
// ENOMEM.
int      tray_add(void* owner, uint32_t pid, uint32_t id, const uint32_t* pixels, int32_t w, int32_t h,
                  const char* tip);

// Change an icon's picture (pixels NULL: keep it) and / or tooltip (tip
// NULL: keep it). 0, or ENOENT (no such icon of this owner), EPERM (another
// process added it), EINVAL.
int      tray_modify(void* owner, uint32_t pid, uint32_t id, const uint32_t* pixels, int32_t w, int32_t h,
                     const char* tip);

int      tray_remove(void* owner, uint32_t pid, uint32_t id);  // 0, ENOENT, EPERM
void     tray_remove_owner(void* owner);                        // Every icon it has (and its menu)

// Open a menu above icon `id` for its owner (DWM_TRAY_MENU_MAX items at
// most, copied; ids are the owner's). Allowed once per click, within
// DWM_TRAY_MENU_MS of it: EPERM otherwise. ENOENT, EINVAL as above.
int      tray_menu(void* owner, uint32_t pid, uint32_t id, const dwm_menu_item_t* items, size_t count);

// Keep a menu for icon `id` (copied; `count` 0 forgets it). A right click
// on the icon then opens it straight away, here, the way the taskbar opens
// a window's menu, without waiting for the program to answer the click.
// The choice comes back through the command handler as for tray_menu.
// 0, or ENOENT, EPERM, EINVAL.
int      tray_set_menu(void* owner, uint32_t pid, uint32_t id, const dwm_menu_item_t* items, size_t count);

size_t   tray_count(void);
uint32_t tray_serial(void);                 // Changes whenever what the tray shows does
uint32_t tray_places_serial(void);          // Changes only when icons come or go (indexes move)
int32_t  tray_width(void);                  // Room the icons want on the strip (0: none)

// For taskbar.c
void       tray_layout(gfx_rect_t area);                    // The icons' part of the strip
int        tray_icon_at(int32_t x, int32_t y);              // Index, -1: none
gfx_rect_t tray_icon_rect(int index);                       // Screen; empty for a bad index

// Icons whose left press opens the tray panel (startmenu.c) at once, and
// what the panel lists for them. One list is kept, the last one sent.
#define DWM_TRAY_PANEL_ROWS_MAX 64
#define DWM_TRAY_PANEL_HEAD_MAX 24      // Bytes, terminator included
#define DWM_TRAY_PANEL_TEXT_MAX 36
#define DWM_TRAY_PANEL_SIDE_MAX 12

#define DWM_PANEL_ROW_LOCK      0x01
#define DWM_PANEL_ROW_MUTED     0x02
#define DWM_PANEL_ROW_NOTE      0x04
#define DWM_PANEL_ROW_CLICK     0x08
#define DWM_PANEL_ROW_ACTIVE    0x10
#define DWM_PANEL_NO_LEVEL      0xFF
#define DWM_PANEL_ACCEPT        0xFFFF0001u
#define DWM_PANEL_CANCEL        0xFFFF0002u
#define DWM_PANEL_REFRESH       0xFFFF0003u     // The refresh button in the heading
#define DWM_PANEL_DISCONNECT    0xFFFF0004u     // The Disconnect button
// tray_panel_rows' flags
#define DWM_PANEL_BUSY          0x0001          // The status in the accent colour
#define DWM_PANEL_HAS_REFRESH   0x0002          // A refresh button left of the title
#define DWM_PANEL_HAS_DISCONNECT 0x0004         // A Disconnect button along the bottom
#define DWM_TRAY_TEXT_MAX       72      // A prompt's answer, terminator included

typedef struct {
    char    text[DWM_TRAY_PANEL_TEXT_MAX];
    char    side[DWM_TRAY_PANEL_SIDE_MAX];
    uint8_t level;                      // 0 .. 4, DWM_PANEL_NO_LEVEL
    uint8_t flags;                      // DWM_PANEL_ROW_*
} dwm_tray_row_t;

typedef struct {
    char           title[DWM_TRAY_PANEL_HEAD_MAX];
    char           status[DWM_TRAY_PANEL_HEAD_MAX];
    bool           busy;
    bool           refresh;             // A refresh button left of the title
    bool           disconnect;          // A Disconnect button along the bottom
    size_t         count;               // Rows that came (of the list's total)
    dwm_tray_row_t rows[DWM_TRAY_PANEL_ROWS_MAX];
    char           prompt[DWM_TRAY_TIP_MAX];    // "": none
    bool           secret;
    bool           connect;             // A Connect button under the box (pressing it: ACCEPT)
    uint32_t       prompt_serial;       // Changes with each new prompt (the box starts empty)
} dwm_tray_panel_t;

int        tray_set_panel(void* owner, uint32_t pid, uint32_t id);  // 0, ENOENT, EPERM
bool       tray_icon_panel(int index);
bool       tray_icon_key(int index, void** owner, uint32_t* id);   // Who it belongs to

// Part of an icon's list: `first` 0 starts it over with the heading, the
// rest follow on in order. `flags` (DWM_PANEL_BUSY / _HAS_*) go with the
// heading (`first` 0). Copied. 0, or ENOENT, EPERM, EINVAL.
int        tray_panel_rows(void* owner, uint32_t pid, uint32_t id, const char* title, const char* status,
                           uint32_t flags, size_t first, size_t total, const dwm_tray_row_t* rows, size_t n);
const dwm_tray_panel_t* tray_panel_get(void* owner, uint32_t id);  // NULL: nothing sent

// A question with a text box in the panel ("" / NULL: none), `connect`: a
// Connect button under the box. 0, ENOENT, EPERM.
int        tray_panel_prompt(void* owner, uint32_t pid, uint32_t id, const char* question, bool secret,
                             bool connect);
// What was typed (accepted), handed over once and forgotten: 0, ENOENT
// (nothing waiting), EPERM
int        tray_panel_take_text(void* owner, uint32_t pid, uint32_t id, char* out, size_t size);

// From the panel (startmenu.c): a row clicked, a button pressed, or the prompt answered
// (`text` with DWM_PANEL_ACCEPT, kept for tray_panel_take_text). The
// prompt goes away on ACCEPT and CANCEL.
typedef void (*dwm_tray_panel_fn)(void* owner, uint32_t id, uint32_t param);
void       tray_set_panel_handler(dwm_tray_panel_fn fn);
void       tray_panel_event(void* owner, uint32_t id, uint32_t param, const char* text);
void       tray_hover(int index);                           // -1: none (shows / hides the tooltip)
void       tray_click(int index, uint8_t button, int32_t x, int32_t y);
void       tray_paint(gfx_surface_t* s, gfx_rect_t area);  // The icons
void       tray_paint_tip(gfx_surface_t* s, gfx_rect_t area);  // The tooltip, above the strip
void       tray_shutdown(void);

#endif
