#ifndef _OS_WM_H
#define _OS_WM_H

/*
 * Windows from the window manager ("dwm" process), for any program.
 *
 *     if (wm_connect(2000) < 0) ...                 // wait up to 2 s for the dwm
 *     wm_window_desc_t d = { .title = "Hello", .x = WM_POS_DEFAULT, .y = WM_POS_DEFAULT,
 *                            .width = 320, .height = 200, .style = WM_STYLE_RESIZABLE };
 *     wm_window_t win = wm_window_create(&d);
 *     gfx_surface_t* s = wm_window_surface(win);
 *     gfx_clear(s, GFX_RGB(20, 20, 32));
 *     gfx_draw_text(s, 8, 8, "hello", GFX_COLOR_WHITE);
 *     wm_window_commit(win, NULL);                  // the window appears now
 *
 *     wm_event_t ev;
 *     while (wm_wait_event(&ev, -1) > 0) {
 *         if (ev.type == WM_EVENT_CLOSE) break;
 *         if (ev.type == WM_EVENT_RESIZED) { redraw; wm_window_commit(win, NULL); }
 *     }
 *     wm_disconnect();                              // closes every window
 *
 * Drawing: every window has a gfx surface of its client size (os/gfx.h),
 * private to this program. Draw on it with the usual gfx calls, then
 * wm_window_commit() the part that changed: that copies it into a
 * shared-memory buffer the dwm paints the window from. Nothing reaches the
 * screen without a commit. A window stays hidden until its first commit
 * (unless created WM_STYLE_HIDDEN: then until wm_window_show()).
 *
 * Resizing: when the window changes size (the user dragging its grip, or
 * wm_window_resize), wm_wait_event() makes a new surface of the new size,
 * copies the old picture into it, and then returns WM_EVENT_RESIZED: redraw
 * and commit. Fetch the surface again after that, the old one is gone.
 * Very large windows get a smaller surface (at most SHM_MAX bytes of
 * pixels); the rest of the window shows the theme background.
 *
 * Events come only for this program's windows: input while one has the
 * focus or the pointer, close requests, moves and resizes. WM_EVENT_CLOSE
 * does nothing by itself; destroy the window (or don't).
 *
 * Theme: wm_get_theme() gives the dwm's colors, so a program can draw in
 * the same theme as the desktop. WM_EVENT_THEME_CHANGED (ev.window 0)
 * arrives when they change: fetch them again and repaint.
 *
 * Font: wm_connect() also fetches the font the dwm hands out and sets it
 * for gfx text (gfx_set_font), so gfx_draw_text() draws in it; when the dwm
 * has none, text stays in the built-in font. Measure with gfx_char_width()
 * and gfx_line_height(): the cell size follows the font. When the dwm's
 * font changes, the new one is set before WM_EVENT_THEME_CHANGED is handed
 * to the program (repaint then). A program that sets its own font with
 * gfx_set_font() keeps it.
 *
 * Context menus: a window created WM_STYLE_CONTEXT_MENU gets right clicks
 * in its client area as WM_EVENT_CONTEXT_MENU (x, y) instead of the dwm's
 * default window menu. Open the dwm's own menu there with wm_menu_show();
 * the chosen item comes back as WM_EVENT_MENU_COMMAND, its id in ev.param.
 * Nothing comes back when the menu is dismissed.
 *
 *     case WM_EVENT_CONTEXT_MENU: {
 *         wm_menu_item_t items[] = { { "Copy", CMD_COPY, 0 },
 *                                    { NULL, 0, WM_MENU_SEPARATOR },
 *                                    { "Paste", CMD_PASTE, can_paste ? 0 : WM_MENU_DISABLED } };
 *         wm_menu_show(ev.window, ev.x, ev.y, items, 3);
 *         break;
 *     }
 *     case WM_EVENT_MENU_COMMAND: if (ev.param == CMD_COPY) ...
 *
 * Notifications: wm_notify() pops a short message up in the dwm's corner
 * stack. It needs no window (it connects on first use), so a program with
 * no UI can use it too. Keep it pithy: one-line title, a few words.
 *
 *     wm_notify("Saved", "notes.txt");
 *     wm_notify_ex(WM_NOTIFY_ERROR, "Can't save", strerror(errno), WM_NOTIFY_STICKY);
 *
 * Clipboard: one item for the whole desktop, kept by the dwm, with a type
 * that says what it holds (WM_CLIP_TEXT, _IMAGE, _BINARY with a format name,
 * _FILES as '\n'-separated absolute paths, or WM_CLIP_USER + n for types
 * programs agree on). Like wm_notify, it needs no window.
 *
 *     wm_clip_set_text("hello");
 *     const char* paths[] = { "/mnt/ata0p1/a.txt" };
 *     wm_clip_set_files(paths, 1, true);                // cut: move on paste
 *
 *     wm_clip_info_t info;
 *     char* data = wm_clip_get(&info);                  // NULL / ENOENT: empty
 *     if (data && info.type == WM_CLIP_TEXT) ... data is 0-terminated too
 *     free(data);
 *     if (info.flags & WM_CLIP_CUT) wm_clip_clear(info.serial);   // after moving
 *
 * wm_clip_info() says what is there without copying the data (for enabling
 * a Paste item). WM_EVENT_CLIP_CHANGED (ev.window 0) arrives when it changes.
 * Every program with SEC_IPC can read it: don't put secrets there.
 *
 * Drag and drop of files: while the left (or right) button is down in one
 * of its windows (say an item was pressed and the pointer moved a few
 * pixels), a program hands the dwm the paths with wm_drag_files(). The
 * window gets its MOUSE_UP right away and nothing more for that press: the
 * dwm shows what will happen by the pointer and drops the paths where the
 * button comes up, on the desktop (which moves them into its folder, or
 * onto a folder icon) or on a window. A window that files are let go over
 * gets WM_EVENT_DROP (x, y: where; mods: INPUT_MOD_CTRL asks for a copy,
 * not a move; button: INPUT_BTN_RIGHT for a right-button drag, which
 * should ask "Move here / Copy here" with a menu there first) and reads
 * them with wm_drop_get(). The program does the moving itself
 * (<home>/sys/fileop is there for that). Desktop icons dragged onto a
 * window arrive the same way.
 *
 *     case WM_EVENT_MOUSE_MOVE:
 *         if (pressed_item && far_enough) { wm_drag_files(&path, 1); pressed_item = false; }
 *     case WM_EVENT_DROP: {
 *         char* list = wm_drop_get(&ev, NULL);      // '\n' between the paths
 *         ... move them into the folder under ev.x, ev.y ...
 *         free(list);
 *     }
 *
 * System tray: a program (with or without windows) can put small icons on
 * the taskbar, left of the uptime. It supplies the picture itself: any
 * size up to WM_TRAY_ICON_MAX a side, 0xAARRGGBB, fit into
 * WM_TRAY_ICON_PX square by the dwm. The id is the program's own number.
 * Clicks come as WM_EVENT_TRAY (ev.window 0, ev.tray the id, ev.button,
 * ev.clicks 2 for a double click, ev.rect the icon on the screen). Right
 * after a click (WM_TRAY_MENU_MS) the program may open a menu above the
 * icon with wm_tray_menu(); the choice comes as WM_EVENT_TRAY_COMMAND
 * (ev.tray, ev.param the item's id). The icons go away with the program:
 * wm_tray_remove(), wm_disconnect(), exiting or crashing all take them.
 *
 *     uint32_t pic[16 * 16] = { ... };
 *     wm_tray_add(1, pic, 16, 16, "Backup: idle");
 *     while (wm_wait_event(&ev, -1) > 0) {
 *         if (ev.type == WM_EVENT_TRAY && ev.button == INPUT_BTN_RIGHT) {
 *             wm_menu_item_t items[] = { { "Back up now", CMD_NOW, 0 }, { "Quit", CMD_QUIT, 0 } };
 *             wm_tray_menu(ev.tray, items, 2);
 *         }
 *         if (ev.type == WM_EVENT_TRAY_COMMAND && ev.param == CMD_QUIT) break;
 *     }
 *
 * Needs SEC_IPC and SEC_GFX (the default clearance has both). Not
 * thread-safe. All calls return -1 (or 0 / NULL) with errno on failure;
 * EPIPE means the dwm went away.
 */

#include <stdbool.h>
#include <stdint.h>
#include <os/gfx.h>
#include <os/wm_proto.h>

#define WM_MAX_WINDOWS      128     // Per program (each uses a gfx surface and a shm block)

typedef uint32_t wm_window_t;

typedef struct {
    const char* title;
    int32_t     x, y;               // Frame top-left on screen, or WM_POS_DEFAULT
    int32_t     width, height;      // Client area
    int32_t     min_width;          // Smallest size when resized (0: the dwm's default)
    int32_t     min_height;
    uint32_t    style;              // WM_STYLE_*
} wm_window_desc_t;

// Connect to the dwm. It may still be starting: retry for up to timeout_ms
// (0: try once, < 0: forever). 0, or -1 with errno (ENOENT: no dwm,
// EBUSY: it has no room for another program, EPERM: no SEC_IPC).
int  wm_connect(int32_t timeout_ms);

// Close every window and the connection
void wm_disconnect(void);
bool wm_connected(void);

// Screen size the dwm reported
void wm_screen_size(int32_t* width, int32_t* height);

// The pipe to the dwm, to wait on it together with other handles
// (ipc_wait, IPC_READABLE). -1 when not connected.
int  wm_handle(void);

wm_window_t    wm_window_create(const wm_window_desc_t* desc);  // 0 on error
int            wm_window_destroy(wm_window_t window);
gfx_surface_t* wm_window_surface(wm_window_t window);           // NULL if no such window
void           wm_window_get_size(wm_window_t window, int32_t* width, int32_t* height);

// Show `rect` of the surface (client coordinates; NULL: all of it). Runs
// the pending drawing first; if a drawing command failed (EBADF for a bad
// resource handle, EINVAL, ...) the frame is still shown and this returns
// -1 with that errno.
int wm_window_commit(wm_window_t window, const gfx_rect_t* rect);

int wm_window_set_title(wm_window_t window, const char* title);
int wm_window_move(wm_window_t window, int32_t x, int32_t y);
int wm_window_resize(wm_window_t window, int32_t width, int32_t height);   // RESIZED follows
int wm_window_show(wm_window_t window, bool visible);
int wm_window_focus(wm_window_t window);                                   // Raise and focus

// The next event: 1, 0 when timeout_ms passed first (0: just look,
// < 0: wait forever), -1 with errno.
int wm_wait_event(wm_event_t* ev, int32_t timeout_ms);

// The dwm's current colors. 0, or -1 with errno.
int wm_get_theme(wm_theme_t* out);

typedef struct {
    const char* label;              // At most WM_MENU_LABEL_MAX - 1 bytes shown
    uint32_t    id;                 // Comes back in WM_EVENT_MENU_COMMAND's param
    uint32_t    flags;              // WM_MENU_SEPARATOR / _DISABLED / _CHECKED
} wm_menu_item_t;

// Open a dwm context menu for `window` at (x, y), client coordinates (it is
// kept on the screen). Up to WM_MENU_MAX_ITEMS items, copied at once.
// 0, or -1 with errno (EINVAL: nothing to show / window hidden, EBADF).
int wm_menu_show(wm_window_t window, int32_t x, int32_t y, const wm_menu_item_t* items, size_t count);

// A menu item that may have a submenu: it shows ">" at the menu's right
// edge and, when the pointer rests on it (or on a click, or Right), opens
// those items beside the menu, Windows style; its own id is unused. A
// submenu holds up to WM_MENU_MAX_ITEMS items, and they may have submenus
// too: WM_MENU_DEPTH levels and WM_MENU_TREE_MAX submenu items in all.
typedef struct wm_menu_node {
    const char*                label;
    uint32_t                   id;
    uint32_t                   flags;           // As wm_menu_item_t
    const struct wm_menu_node* submenu;         // NULL: an ordinary item
    size_t                     submenu_count;
} wm_menu_node_t;

// wm_menu_show with submenus. A choice in any of them comes back as
// WM_EVENT_MENU_COMMAND with that item's id, like one in the menu. EINVAL
// also for too many submenu items or levels.
int wm_menu_show_tree(wm_window_t window, int32_t x, int32_t y, const wm_menu_node_t* items, size_t count);

// Clipboard. All connect first if needed; 0 (or the data), or -1 / NULL with
// errno (EINVAL bad description, E2BIG over WM_CLIP_MAX, ENOENT empty).
int   wm_clip_set(const wm_clip_info_t* info, const void* data);    // type, flags, size, width, height, format
int   wm_clip_set_text(const char* text);
int   wm_clip_set_files(const char* const* paths, size_t count, bool cut);
int   wm_clip_set_image(const uint32_t* pixels, int32_t width, int32_t height);
int   wm_clip_set_binary(const char* format, const void* data, size_t size);
int   wm_clip_info(wm_clip_info_t* out);                             // type WM_CLIP_NONE: empty
void* wm_clip_get(wm_clip_info_t* out);     // malloc'd copy with a 0 after it: free() it
int   wm_clip_clear(uint32_t serial);       // Only if it still holds that item (0: anything)
const char* wm_clip_type_name(uint32_t type);   // "text", "an image", "files", ...

// Take the mouse for `window` (lock true) or give it back: for games and
// 3D views that turn with the mouse. The window must have the focus. While
// locked the cursor is hidden and kept in the window, and MOUSE_MOVE keeps
// coming wherever the mouse goes: use the change in x y between events (the
// numbers run on past the window's edges). The dwm lets go by itself when
// the window loses the focus (WM_EVENT_FOCUS_LOST; lock again on
// FOCUS_GAINED) or a menu opens. 0, or -1 with errno (EPERM: not focused or
// a drag is on, EINVAL: a dwm without pointer lock, EBADF).
int   wm_window_lock_pointer(wm_window_t window, bool lock);

// Drag `count` absolute paths from the left (or right) press in one of this
// program's windows (see above). 0: the dwm has the drag (or dropped them
// already, when the left button came up before it heard), -1 with errno
// (EPERM: the last press wasn't in a window of ours, or it ended a while
// ago; EINVAL bad paths, E2BIG).
int   wm_drag_files(const char* const* paths, size_t count);

// The paths of a WM_EVENT_DROP, '\n' between them, malloc'd with a 0 after
// them: free() it. NULL with errno (ENOENT: that drop is gone, EINVAL: not
// a drop event). `out` (may be NULL): type WM_CLIP_FILES, size.
char* wm_drop_get(const wm_event_t* ev, wm_clip_info_t* out);

// The folder files dropped on `window` go into (NULL or "": not known),
// for the plate by the pointer during a drag: files from another drive
// show "Copy" instead of "Move". Set it again whenever the window shows
// another folder, and on WM_EVENT_DRAG_OVER for the folder under the
// pointer (the plate then says "Move to <name>"). 0, or -1 with errno.
int   wm_set_drop_folder(wm_window_t window, const char* folder);

// Tray icons. All connect first if needed (without waiting for the dwm).
// 0, or -1 with errno: EINVAL (no picture, bad size), EEXIST (add: the id
// is taken), ENOENT (no such icon), EMFILE (WM_TRAY_PER_PROCESS already),
// ENOSPC (the tray is full), EPERM (menu: no click on the icon just now).
int   wm_tray_add(uint32_t id, const uint32_t* pixels, int32_t width, int32_t height, const char* tip);
int   wm_tray_set_icon(uint32_t id, const uint32_t* pixels, int32_t width, int32_t height);
int   wm_tray_set_tip(uint32_t id, const char* tip);       // NULL or "": no tooltip
int   wm_tray_remove(uint32_t id);
int   wm_tray_menu(uint32_t id, const wm_menu_item_t* items, size_t count);

// Give the desktop a menu to keep for icon `id`: a right click on the icon
// opens it at once, the way the taskbar opens a window's menu, with no
// round trip to the program (WM_EVENT_TRAY still comes; don't answer it
// with wm_tray_menu). Choices come as WM_EVENT_TRAY_COMMAND. Send it again
// whenever an item's flags change; count 0 forgets it. 0, or -1 with errno.
// A desktop older than the tray panel's buttons doesn't know this.
int   wm_tray_set_menu(uint32_t id, const wm_menu_item_t* items, size_t count);

// A left press on this icon opens the desktop's own tray panel at once (the
// start menu's panel, mirrored against the right edge). WM_EVENT_TRAY still
// comes, as the panel opens. Another click on the icon, a click anywhere
// outside the panel, or Esc closes it. 0, or -1 with errno (EINVAL: a
// desktop without the panel).
int   wm_tray_set_panel(uint32_t id);

// What the panel lists: a heading (title left, status right; `busy` shows
// the status in the accent colour) and `count` rows, which the desktop
// scrolls. Rows past WM_TRAY_PANEL_ROWS_MAX are left out. Sent in pieces;
// 0, or -1 with errno.
int   wm_tray_set_panel_rows(uint32_t id, const char* title, const char* status, bool busy,
                             const wm_tray_panel_row_t* rows, size_t count);

// The same with the heading's flags: WM_PANEL_BUSY, WM_PANEL_HAS_REFRESH (a
// refresh button left of the title) and WM_PANEL_HAS_DISCONNECT (a
// Disconnect button along the bottom). A press on a button sends
// WM_EVENT_TRAY_PANEL with WM_PANEL_REFRESH / WM_PANEL_DISCONNECT. A
// desktop without the buttons refuses the flags (EINVAL).
int   wm_tray_set_panel_ex(uint32_t id, const char* title, const char* status, uint32_t flags,
                           const wm_tray_panel_row_t* rows, size_t count);

// A question at the bottom of the panel with a box to type in (`secret`:
// shown as '*'); NULL or "" takes it away. Enter or Esc in the box sends
// WM_EVENT_TRAY_PANEL (WM_PANEL_ACCEPT / _CANCEL) and takes it away too, as
// does the panel closing (CANCEL). After ACCEPT, wm_tray_prompt_text gets
// what was typed, once: the desktop forgets it then. Clear `out` when done
// with a password.
int   wm_tray_prompt(uint32_t id, const char* question, bool secret);
// The same with flags: WM_TRAY_SECRET, and WM_TRAY_CONNECT for a Connect
// button under the box (pressing it is the same as Enter). A desktop
// without the button refuses WM_TRAY_CONNECT (EINVAL).
int   wm_tray_prompt_ex(uint32_t id, const char* question, uint32_t flags);
int   wm_tray_prompt_text(uint32_t id, char* out, size_t size);

// Say that files changed at or below `path` (a copy, move or delete): every
// program showing folders gets WM_EVENT_FS_CHANGED to re-read them, and the
// desktop re-reads its icons. Connects first if needed. 0, or -1 with errno.
int wm_fs_changed(const char* path);

// Say that the desktop settings in the registry changed (os/desktop.h: the
// background color, the wallpaper): the desktop reads them again and
// repaints. Connects first if needed. 0, or -1 with errno.
int wm_settings_changed(void);

// Show a notification for WM_NOTIFY_DEFAULT_MS. Connects to the dwm first
// if needed (without waiting for it). 0, or -1 with errno (ENOENT: no dwm).
int wm_notify(const char* title, const char* message);

// With a kind (WM_NOTIFY_INFO / WM_NOTIFY_ERROR) and a timeout
// (WM_NOTIFY_STICKY: until clicked; at most WM_NOTIFY_MAX_MS). Longer title
// or message text is cut (WM_NOTIFY_TITLE_MAX / WM_NOTIFY_TEXT_MAX).
int wm_notify_ex(uint32_t kind, const char* title, const char* message, uint32_t timeout_ms);

#endif

