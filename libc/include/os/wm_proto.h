#ifndef _OS_WM_PROTO_H
#define _OS_WM_PROTO_H

/*
 * Window manager protocol: what goes over the pipe between a program and
 * the "dwm" process. Programs normally use os/wm.h instead, which speaks
 * this for them; the window manager (src/dwm/server.c) is the other end.
 *
 *   connect      ipc_connect(WM_PORT_NAME). The port name starts "sys.",
 *                so only an administrator process can register it: no
 *                other program can pose as the window manager.
 *   greeting     the dwm sends WM_MSG_WELCOME (protocol version, screen
 *                size) as soon as it accepts the connection.
 *   requests     the program sends wm_request_t, one per message, exactly
 *                sizeof(wm_request_t) bytes. A non-zero seq asks for a
 *                WM_MSG_REPLY with the same seq; seq 0 gets a reply only
 *                for CREATE (which always answers).
 *   events       the dwm sends WM_MSG_EVENT for each window event (input,
 *                focus, moves, resizes, close requests).
 *   pixels       each window has a buffer of 0xAARRGGBB pixels in shared
 *                memory (stride = buffer width), handed to the dwm with
 *                WM_REQ_ATTACH (the shm handle rides along with the
 *                message). The program draws into it and sends
 *                WM_REQ_COMMIT for the part that changed; the dwm then
 *                repaints that part of the window from the buffer.
 *   goodbye      closing the pipe (or the program exiting) destroys every
 *                window the program has.
 *   theme        WM_REQ_THEME is answered with the dwm's colors in the
 *                reply's `theme`; WM_EVENT_THEME_CHANGED (window 0) tells
 *                every program when they change.
 *   font         WM_REQ_FONT is answered with the font programs draw text
 *                in: its size and name in the reply's `font`, the font file
 *                in a fresh shared memory block attached (status ENOENT:
 *                none, use the built-in font). WM_EVENT_THEME_CHANGED is
 *                also sent when it changes.
 *   notify       WM_REQ_NOTIFY (a wm_notify_request_t) shows a notification
 *                in the dwm's corner stack; it needs no window. Replied to
 *                like wm_request_t when seq is non-zero.
 *   clipboard    WM_REQ_CLIP_SET / GET / INFO / CLEAR (a wm_clip_request_t):
 *                one typed item for the whole desktop. SET carries the data
 *                in a shared memory block attached to the message (the dwm
 *                copies it and lets go); the reply to GET carries a fresh
 *                block holding a copy. WM_EVENT_CLIP_CHANGED (window 0,
 *                param = the new serial) tells every program it changed.
 *   files        WM_REQ_FS_CHANGED (a wm_path_request_t): a program that
 *                changed files (copied, moved, deleted) says where; every
 *                program gets WM_EVENT_FS_CHANGED (window 0) to re-read what
 *                it shows, and the desktop re-reads its icons.
 *   drag & drop  WM_REQ_DRAG (a wm_clip_request_t, WM_CLIP_FILES, the paths
 *                in an attached shm block as for CLIP_SET) while the left
 *                (or right) button is down in one of the program's windows: the dwm
 *                takes the pointer (the window gets its MOUSE_UP) and drops
 *                the paths where the button comes up. EPERM: the last left
 *                press wasn't in a window of this program (or is long over). A window let go
 *                over gets WM_EVENT_DROP (param: the drop's serial) and asks
 *                for the paths with WM_REQ_DROP_GET (info.serial = it),
 *                answered as CLIP_GET (ENOENT: not its drop, or replaced).
 *                WM_REQ_DROP_FOLDER (a wm_folder_request_t) tells the dwm
 *                which folder a window's drops go into, for the plate by
 *                the pointer: "Copy" for files from another drive.
 *   pointer      WM_REQ_POINTER_LOCK (window, style 1: lock, 0: let go):
 *                the window, which must have the focus, takes the mouse.
 *                The cursor hides and the dwm keeps it at the window's
 *                middle; every move and button goes to that window, with
 *                x y counting on past its edges (the change between two
 *                MOUSE_MOVEs is how far the mouse went). It ends when the
 *                program lets go, the window loses the focus or goes away,
 *                or a menu opens. EPERM: not focused, or a drag is on.
 *   menus        a window created WM_STYLE_CONTEXT_MENU gets right clicks in
 *                its client area as WM_EVENT_CONTEXT_MENU (instead of the
 *                dwm opening its default window menu there) and answers with
 *                a wm_menu_request_t: the dwm draws the menu and sends
 *                WM_EVENT_MENU_COMMAND with the chosen item's id.
 *   tray         WM_REQ_TRAY_ADD / MODIFY / REMOVE (a wm_tray_request_t):
 *                a small icon on the taskbar, left of the uptime, with a
 *                tooltip. The program picks the icon's id and supplies the
 *                picture (icon_w x icon_h 0xAARRGGBB pixels, at most
 *                WM_TRAY_ICON_MAX a side) in a shared memory block attached
 *                to the message; the dwm copies it, shrinks it to
 *                WM_TRAY_ICON_PX and lets go. Icons belong to the connection
 *                that added them and to the process that sent the ADD: only
 *                that process can change them, and they go when the
 *                connection closes (the program exits, crashes or calls
 *                wm_disconnect). At most WM_TRAY_PER_PROCESS per process
 *                (EMFILE), WM_TRAY_MAX in all (ENOSPC). A click on one sends
 *                WM_EVENT_TRAY (window 0) to its connection. Within
 *                WM_TRAY_MENU_MS of that click the program may answer with
 *                WM_REQ_TRAY_MENU (a wm_menu_request_t, window = the icon's
 *                id): the dwm opens the menu above the icon and sends
 *                WM_EVENT_TRAY_COMMAND with the chosen item's id. One menu
 *                per click (EPERM otherwise), so a program can't keep
 *                popping menus up to take the mouse. After WM_TRAY_PANEL
 *                (a MODIFY flag) a left press on the icon opens the dwm's
 *                tray panel above it instead, still sending WM_EVENT_TRAY;
 *                the program fills the panel with WM_REQ_TRAY_PANEL.
 *   settings     WM_REQ_SETTINGS_CHANGED (a wm_request_t, nothing in it): a
 *                program changed the desktop's settings in the registry
 *                (os/desktop.h: the background color or wallpaper); the
 *                dwm reads them again and repaints. Replied to when seq is
 *                non-zero.
 *
 * Window ids are the dwm's own handles. A program can only touch windows it
 * created over its own connection.
 */

#include <stdint.h>

#define WM_PORT_NAME            "sys.dwm"
#define WM_PROTOCOL_VERSION     1           // Bumped on every protocol change once 0.1.0 is out
#define WM_TITLE_MAX            128         // Bytes, terminator included

// Position the window manager picks (cascading)
#define WM_POS_DEFAULT          INT32_MIN

// Styles: same meaning and values as DWM_STYLE_* (src/dwm/window.h)
#define WM_STYLE_TOPMOST        0x0001      // Always above other windows
#define WM_STYLE_NOBORDER       0x0002      // No frame at all
#define WM_STYLE_NOTITLEBAR     0x0004      // Border only
#define WM_STYLE_NOCLOSEBOX     0x0008      // No close box
#define WM_STYLE_RESIZABLE      0x0010      // Resize grip
#define WM_STYLE_HIDDEN         0x0020      // Created hidden
#define WM_STYLE_NOFOCUS        0x0040      // Never takes the focus
#define WM_STYLE_NOMINIMIZE     0x0080      // No minimize box, can't be minimized
// Protocol only (not a DWM_STYLE_*): right clicks in the client area go to
// the program as WM_EVENT_CONTEXT_MENU, the dwm's window menu stays shut
#define WM_STYLE_CONTEXT_MENU   0x0100
#define WM_STYLE_VALID          0x01FF

// Context menus (wm_menu_request_t). Flags: same values as DWM_MENU_*.
#define WM_MENU_MAX_ITEMS       10
#define WM_MENU_LABEL_MAX       40          // Bytes, terminator included
#define WM_MENU_SEPARATOR       0x0001      // A dividing line (label, id unused)
#define WM_MENU_DISABLED        0x0002      // Dimmed, can't be chosen
#define WM_MENU_CHECKED         0x0004      // Check mark before the label
#define WM_MENU_SUBMENU         0x0008      // Opens a submenu beside it (id unused): its items
                                            // are in the attached wm_menu_tree_t
#define WM_MENU_TREE_MAX        64          // Submenu items in one menu, all levels together
#define WM_MENU_DEPTH           4           // The menu and up to 3 levels of submenus

//
// Program -> dwm
//

enum {
    WM_REQ_CREATE = 1,      // x y w h min_w min_h style title -> REPLY.window
    WM_REQ_DESTROY,         // window
    WM_REQ_ATTACH,          // window, w h = buffer size; shm handle attached
    WM_REQ_COMMIT,          // window, x y w h (client coordinates) changed
    WM_REQ_SET_TITLE,       // window, title
    WM_REQ_MOVE,            // window, x y (frame top-left, screen)
    WM_REQ_RESIZE,          // window, w h (client size)
    WM_REQ_SHOW,            // window, style != 0: show, 0: hide
    WM_REQ_FOCUS,           // window: raise and focus
    WM_REQ_THEME,           // -> REPLY.theme: the dwm's colors (always answered)
    WM_REQ_MENU,            // wm_menu_request_t (not wm_request_t): open a menu
    WM_REQ_NOTIFY,          // wm_notify_request_t (not wm_request_t): a notification
    WM_REQ_CLIP_SET,        // wm_clip_request_t, shm attached when info.size > 0
    WM_REQ_CLIP_GET,        // wm_clip_request_t -> REPLY.clip, shm attached (ENOENT: empty)
    WM_REQ_CLIP_INFO,       // wm_clip_request_t -> REPLY.clip (type NONE: empty)
    WM_REQ_CLIP_CLEAR,      // wm_clip_request_t: info.serial (0: whatever is there)
    WM_REQ_FS_CHANGED,      // wm_path_request_t: files changed at or below path
    WM_REQ_FONT,            // -> REPLY.font, the font file's bytes attached (ENOENT: none)
    WM_REQ_DRAG,            // wm_clip_request_t, WM_CLIP_FILES, shm attached: drag them
    WM_REQ_DROP_GET,        // wm_clip_request_t: info.serial -> REPLY.clip, shm attached
    WM_REQ_POINTER_LOCK,    // window, style 1: take the mouse, 0: let it go
    WM_REQ_TRAY_ADD,        // wm_tray_request_t, icon attached: a new tray icon (EEXIST: id taken)
    WM_REQ_TRAY_MODIFY,     // wm_tray_request_t: new picture (WM_TRAY_ICON) and / or tooltip (WM_TRAY_TIP)
    WM_REQ_TRAY_REMOVE,     // wm_tray_request_t: id
    WM_REQ_TRAY_MENU,       // wm_menu_request_t (window = tray icon id): a menu above the icon
    WM_REQ_TRAY_PANEL,      // wm_tray_panel_request_t: what the icon's tray panel lists
    WM_REQ_TRAY_PROMPT,     // wm_tray_request_t: tip = a question with a text box under it in
                            // the panel ("": none), WM_TRAY_SECRET hides what is typed
    WM_REQ_TRAY_PROMPT_TEXT,// wm_tray_request_t -> REPLY.text: what was typed, once (ENOENT: nothing)
    WM_REQ_SETTINGS_CHANGED,// The desktop settings in the registry changed: the dwm reads them again
    WM_REQ_DROP_FOLDER,     // wm_folder_request_t: where files dropped on window go ("": not known)
};

typedef struct {
    uint32_t type;          // WM_REQ_*
    uint32_t seq;           // Echoed in the reply; 0 = no reply wanted
    uint32_t window;
    int32_t  x, y, w, h;
    int32_t  min_w, min_h;
    uint32_t style;
    char     title[WM_TITLE_MAX];
} wm_request_t;

typedef struct {
    uint32_t id;            // Sent back in WM_EVENT_MENU_COMMAND.param
    uint32_t flags;         // WM_MENU_*
    char     label[WM_MENU_LABEL_MAX];
} wm_menu_entry_t;

// WM_REQ_MENU: open a dwm context menu for `window` at x y (client
// coordinates); the dwm keeps it on the screen. Exactly
// sizeof(wm_menu_request_t) bytes. Replies like wm_request_t (status
// EINVAL when nothing could show: no items, window hidden).
typedef struct {
    uint32_t        type;   // WM_REQ_MENU
    uint32_t        seq;
    uint32_t        window;
    int32_t         x, y;
    uint32_t        count;  // 1 .. WM_MENU_MAX_ITEMS
    wm_menu_entry_t items[WM_MENU_MAX_ITEMS];
} wm_menu_request_t;

// Submenus of a WM_REQ_MENU, in a shared memory block attached to it (only
// when an item has WM_MENU_SUBMENU). Items are numbered across the request
// and the block: 0 .. count-1 the request's own, count + j entry j here.
// Each entry names the item it is a submenu item of (`parent`): one with a
// lower number and WM_MENU_SUBMENU set; a submenu lists its items in the
// order they appear here, at most WM_MENU_MAX_ITEMS each. The dwm refuses
// the menu (EINVAL) when the block breaks these rules.
typedef struct {
    uint32_t parent;
    uint32_t id;
    uint32_t flags;         // WM_MENU_*
    char     label[WM_MENU_LABEL_MAX];
} wm_menu_tree_entry_t;

typedef struct {
    uint32_t             count;     // 1 .. WM_MENU_TREE_MAX
    wm_menu_tree_entry_t items[];
} wm_menu_tree_t;

// Notifications (WM_REQ_NOTIFY). Kinds and STICKY: same values as DWM_NOTIFY_*.
#define WM_NOTIFY_INFO          0
#define WM_NOTIFY_ERROR         1           // The theme's danger colors
#define WM_NOTIFY_STICKY        0           // timeout_ms: stays until clicked
#define WM_NOTIFY_DEFAULT_MS    5000
#define WM_NOTIFY_MAX_MS        60000       // Longer timeouts are cut to this
#define WM_NOTIFY_TITLE_MAX     64          // Bytes, terminator included
#define WM_NOTIFY_TEXT_MAX      256

// Exactly sizeof(wm_notify_request_t) bytes. Each program has at most 3
// showing: a fourth pushes its oldest out. Status EINVAL for a bad kind.
typedef struct {
    uint32_t type;          // WM_REQ_NOTIFY
    uint32_t seq;
    uint32_t kind;          // WM_NOTIFY_*
    uint32_t timeout_ms;    // WM_NOTIFY_STICKY, or how long it shows
    char     title[WM_NOTIFY_TITLE_MAX];
    char     text[WM_NOTIFY_TEXT_MAX];      // Word wrapped, at most 4 lines shown
} wm_notify_request_t;

// Clipboard item types: same values as DWM_CLIP_* (src/dwm/clipboard.h)
#define WM_CLIP_NONE            0
#define WM_CLIP_TEXT            1           // Text bytes, no terminator needed
#define WM_CLIP_IMAGE           2           // width * height 0xAARRGGBB pixels, row after row
#define WM_CLIP_BINARY          3           // Raw bytes; `format` names them
#define WM_CLIP_FILES           4           // Absolute paths, '\n' between them
#define WM_CLIP_USER            0x100       // And up: types programs agree on

#define WM_CLIP_CUT             0x0001      // FILES: move on paste (the paster clears it)
#define WM_CLIP_FORMAT_MAX      32          // Bytes, terminator included
#define WM_CLIP_MAX             (4u << 20)  // Bytes of data

typedef struct {
    uint32_t type;                          // WM_CLIP_*
    uint32_t flags;                         // WM_CLIP_CUT
    uint32_t size;                          // Bytes of data
    uint32_t width, height;                 // IMAGE: pixels
    uint32_t serial;                        // Changes with every set / clear (set by the dwm)
    uint32_t owner;                         // pid that set it, 0: the dwm (set by the dwm)
    char     format[WM_CLIP_FORMAT_MAX];    // Optional name for the data ("audio/wav")
} wm_clip_info_t;

// Exactly sizeof(wm_clip_request_t) bytes, for all four WM_REQ_CLIP_*
typedef struct {
    uint32_t       type;
    uint32_t       seq;
    wm_clip_info_t info;    // SET: describes the attached data. CLEAR: serial.
} wm_clip_request_t;

#define WM_PATH_MAX             400         // Bytes, terminator included

// Exactly sizeof(wm_path_request_t) bytes
typedef struct {
    uint32_t type;          // WM_REQ_FS_CHANGED
    uint32_t seq;
    char     path[WM_PATH_MAX];
} wm_path_request_t;

// Exactly sizeof(wm_folder_request_t) bytes. WM_REQ_DROP_FOLDER: the
// folder files let go over `window` would go into, so the drag plate can
// say "Copy" (not "Move") for files from another drive. Replied to like
// wm_request_t when seq is non-zero (EBADF: not this program's window).
typedef struct {
    uint32_t type;          // WM_REQ_DROP_FOLDER
    uint32_t seq;
    uint32_t window;
    char     path[WM_PATH_MAX];     // Absolute, "": not known
} wm_folder_request_t;

// System tray (WM_REQ_TRAY_*)
#define WM_TRAY_ICON_PX         16          // Drawn size, a side (the picture is fit into it)
#define WM_TRAY_ICON_MAX        64          // Largest picture accepted, a side
#define WM_TRAY_TIP_MAX         64          // Bytes, terminator included
#define WM_TRAY_PER_PROCESS     4
#define WM_TRAY_MAX             24          // On the taskbar at once, every program together
#define WM_TRAY_MENU_MS         3000        // How long after a click WM_REQ_TRAY_MENU is allowed
#define WM_TRAY_MENU_KEEP       1           // WM_REQ_TRAY_MENU's x: keep the menu, the dwm opens it
                                            // itself on every right click (count 0: none kept)

// wm_tray_request_t.flags
#define WM_TRAY_ICON            0x0001      // A picture is attached (ADD: required)
#define WM_TRAY_TIP             0x0002      // `tip` holds the tooltip ("": none)
#define WM_TRAY_PANEL           0x0004      // A left click opens the desktop's tray panel
#define WM_TRAY_SECRET          0x0008      // WM_REQ_TRAY_PROMPT: a password, shown as '*'
#define WM_TRAY_CONNECT         0x0010      // WM_REQ_TRAY_PROMPT: a Connect button under the box
                                            // (a click on it is Enter: WM_PANEL_ACCEPT)
#define WM_TRAY_TEXT_MAX        72          // What a prompt takes, bytes, terminator included

// Exactly sizeof(wm_tray_request_t) bytes. The picture rides in a shared
// memory block of at least icon_w * icon_h * 4 bytes, row after row.
// Status: EINVAL (bad size, no picture for ADD, block too small), EEXIST
// (ADD: this program has that id already), ENOENT (no such icon of this
// program), EPERM (another process added it), EMFILE, ENOSPC.
typedef struct {
    uint32_t type;          // WM_REQ_TRAY_ADD / _MODIFY / _REMOVE
    uint32_t seq;
    uint32_t id;            // The program's own number for the icon
    uint32_t flags;         // WM_TRAY_ICON, WM_TRAY_TIP
    int32_t  icon_w, icon_h;
    char     tip[WM_TRAY_TIP_MAX];
} wm_tray_request_t;

// The tray panel's contents (WM_REQ_TRAY_PANEL): a heading, then a list of
// rows the dwm scrolls. They come WM_TRAY_PANEL_ROWS at a time: a request
// with first == 0 starts the list over (and carries the heading), the rest
// follow on. `total` is the whole list's length (WM_TRAY_PANEL_ROWS_MAX at
// most). Status: 0, ENOENT / EPERM as for the icon, EINVAL.
#define WM_TRAY_PANEL_ROWS      8           // Rows in one request
#define WM_TRAY_PANEL_ROWS_MAX  64          // In the whole list
#define WM_TRAY_PANEL_HEAD_MAX  24          // Bytes, terminator included
#define WM_TRAY_PANEL_TEXT_MAX  36
#define WM_TRAY_PANEL_SIDE_MAX  12

// wm_tray_panel_row_t.flags
#define WM_PANEL_ROW_LOCK       0x01        // A padlock after the level
#define WM_PANEL_ROW_MUTED      0x02        // Text in the muted colour
#define WM_PANEL_ROW_NOTE       0x04        // A message line: muted, no level or padlock
#define WM_PANEL_ROW_CLICK      0x08        // Highlighted under the pointer; a click sends WM_EVENT_TRAY_PANEL
#define WM_PANEL_ROW_ACTIVE     0x10        // Marked as the one in use (a dot before it)

// WM_EVENT_TRAY_PANEL.param: a row's index, or
#define WM_PANEL_ACCEPT         0xFFFF0001u // Enter in the prompt's box: WM_REQ_TRAY_PROMPT_TEXT has it
#define WM_PANEL_CANCEL         0xFFFF0002u // Esc in the box, or the panel closed with it open
#define WM_PANEL_REFRESH        0xFFFF0003u // The refresh button in the heading (WM_PANEL_HAS_REFRESH)
#define WM_PANEL_DISCONNECT     0xFFFF0004u // The Disconnect button (WM_PANEL_HAS_DISCONNECT)

// wm_tray_panel_request_t.flags
#define WM_PANEL_BUSY           0x0001      // The status in the accent colour
#define WM_PANEL_HAS_REFRESH    0x0002      // A refresh button left of the title
#define WM_PANEL_HAS_DISCONNECT 0x0004      // A Disconnect button along the bottom

#define WM_PANEL_NO_LEVEL       0xFF        // wm_tray_panel_row_t.level: no bars

typedef struct {
    char    text[WM_TRAY_PANEL_TEXT_MAX];   // Left (cut to fit, ".." at the end)
    char    side[WM_TRAY_PANEL_SIDE_MAX];   // Right aligned
    uint8_t level;                          // Signal bars lit, 0 .. 4, or WM_PANEL_NO_LEVEL
    uint8_t flags;                          // WM_PANEL_ROW_*
} wm_tray_panel_row_t;

typedef struct {
    uint32_t type;          // WM_REQ_TRAY_PANEL
    uint32_t seq;
    uint32_t id;            // The tray icon
    uint32_t flags;         // WM_PANEL_BUSY, _HAS_REFRESH, _HAS_DISCONNECT (first == 0 only)
    uint16_t first;         // Index of rows[0] in the list
    uint16_t total;
    uint16_t count;         // Rows in this request
    uint16_t reserved;
    char     title[WM_TRAY_PANEL_HEAD_MAX];  // first == 0 only: the heading, left ...
    char     status[WM_TRAY_PANEL_HEAD_MAX]; // ... and right
    wm_tray_panel_row_t rows[WM_TRAY_PANEL_ROWS];
} wm_tray_panel_request_t;

//
// dwm -> program
//

enum {
    WM_EVENT_NONE = 0,
    WM_EVENT_CLOSE,         // Close box (or dwm_window_close): destroy it, or ignore
    WM_EVENT_MOUSE_MOVE,    // x y (client coordinates), buttons held
    WM_EVENT_MOUSE_DOWN,    // button, clicks (2: double click)
    WM_EVENT_MOUSE_UP,      // button
    WM_EVENT_MOUSE_ENTER,
    WM_EVENT_MOUSE_LEAVE,
    WM_EVENT_KEY,           // key (character or KEY_*, os/input.h), mods
    WM_EVENT_FOCUS_GAINED,
    WM_EVENT_FOCUS_LOST,
    WM_EVENT_MOVED,         // rect: new frame rectangle (screen)
    WM_EVENT_RESIZED,       // rect.w rect.h: new client size
    WM_EVENT_SHOW,
    WM_EVENT_HIDE,
    WM_EVENT_DESTROYED,     // The dwm destroyed the window (shutting down)
    WM_EVENT_CONTEXT_MENU,  // Right click in the client area (WM_STYLE_CONTEXT_MENU): x y
    WM_EVENT_MENU_COMMAND,  // A menu item was chosen: param = its id; x y = where it opened
    WM_EVENT_THEME_CHANGED, // window 0: the colors changed (ask again with WM_REQ_THEME)
    WM_EVENT_CLIP_CHANGED,  // window 0: the clipboard changed; param = its new serial
    WM_EVENT_FS_CHANGED,    // window 0: some program changed files: re-read folders shown
    WM_EVENT_DROP,          // Files let go over the window: x y (client coordinates, negative
                            // over the titlebar), mods (Ctrl: copy them), button (RIGHT:
                            // a right drag, ask Move / Copy), param = the drop's serial
                            // (wm_drop_get)
    WM_EVENT_TRAY,          // window 0: a tray icon was clicked. tray = its id, button,
                            // clicks (2: double click), x y = the pointer (screen),
                            // rect = the icon (screen)
    WM_EVENT_TRAY_COMMAND,  // window 0: an item of a tray menu was chosen. tray = the
                            // icon's id, param = the item's id
    WM_EVENT_TRAY_PANEL,    // window 0: in the icon's tray panel: a row clicked (param = its
                            // index), the prompt answered (WM_PANEL_ACCEPT / _CANCEL) or a
                            // button pressed (WM_PANEL_REFRESH / _DISCONNECT)
    WM_EVENT_MOUSE_WHEEL,   // The wheel turned over the window (or the window has the
                            // mouse captured / locked): wheel = notches, + toward the
                            // user (scroll down), - away (up); x y (client coordinates),
                            // buttons, mods (Ctrl: by pages, if the program likes)
    WM_EVENT_DRAG_OVER,     // Files are dragged over the window: x y (client coordinates,
                            // negative over the titlebar), mods, button. Answer with
                            // WM_REQ_DROP_FOLDER (wm_set_drop_folder) for the folder there
                            // and the plate by the pointer names it ("Copy to Documents")
};

typedef struct {
    uint32_t type;          // WM_EVENT_*
    uint32_t window;
    int32_t  x, y;          // Mouse position (client coordinates)
    int32_t  rect_x, rect_y, rect_w, rect_h;
    uint32_t time_ms;
    uint16_t key;
    uint8_t  buttons;       // INPUT_BTN_* held
    uint8_t  button;        // MOUSE_DOWN / MOUSE_UP
    uint8_t  clicks;
    uint8_t  mods;          // INPUT_MOD_*
    int16_t  wheel;         // MOUSE_WHEEL: notches, + toward the user (down)
    uint32_t param;         // MENU_COMMAND, TRAY_COMMAND: the item's id
    uint32_t tray;          // TRAY, TRAY_COMMAND: the tray icon's id
} wm_event_t;

// The dwm's colors (0xAARRGGBB), field for field the dwm's dwm_theme_t
// (src/dwm/theme.h, which documents each one and its default)
typedef struct {
    struct {
        uint32_t background, icon_text, icon_shadow, icon_selected,
                 icon_selected_border, band_fill, band_border;
    } desktop;
    struct {
        uint32_t border, background, title_text, active_low, active_high,
                 inactive_low, inactive_high;
    } frame;
    struct {
        uint32_t background, background_danger, text, text_soft, text_value,
                 text_muted, text_mount, text_danger, accent, divider_soft;
    } client;
    struct {
        uint32_t fill, border, border_active, text, fill_pressed, border_pressed;
    } button;
    struct {
        uint32_t background, border, text, cursor, selection;
    } edit;
    struct {
        uint32_t background, border, separator, highlight, text;
    } menu;
    struct {
        uint32_t gradient_low, gradient_high;
    } taskbar;
    struct {
        uint32_t pie_disksize_used, pie_disksize_free;
    } interface;
} wm_theme_t;

// The program font (WM_REQ_FONT)
#define WM_FONT_NAME_MAX        64          // Bytes, terminator included

typedef struct {
    int32_t  px;                            // Pixel height to draw it at
    uint32_t size;                          // Bytes of the font file attached
    char     name[WM_FONT_NAME_MAX];        // The dwm's name for it ("LiberationMono-Regular")
} wm_font_info_t;

enum {
    WM_MSG_WELCOME = 0x80,  // version, screen_w, screen_h
    WM_MSG_REPLY,           // seq, status, window
    WM_MSG_EVENT,           // event
};

typedef struct {
    uint32_t   kind;        // WM_MSG_*
    uint32_t   seq;         // REPLY
    int32_t    status;      // REPLY: 0, or an errno value (EINVAL, ENOMEM ...)
    uint32_t   window;      // REPLY: the window (CREATE: the new one)
    uint32_t   version;     // WELCOME
    int32_t    screen_w;    // WELCOME
    int32_t    screen_h;
    union {
        wm_event_t event;   // EVENT
        wm_theme_t theme;   // REPLY to WM_REQ_THEME
        wm_clip_info_t clip;// REPLY to WM_REQ_CLIP_GET / WM_REQ_CLIP_INFO
        wm_font_info_t font;// REPLY to WM_REQ_FONT
        char text[WM_TRAY_TEXT_MAX];    // REPLY to WM_REQ_TRAY_PROMPT_TEXT
    };
} wm_message_t;

_Static_assert(sizeof(wm_request_t) <= 512 && sizeof(wm_menu_request_t) <= 512 &&
               sizeof(wm_message_t) <= 512, "protocol messages must fit IPC_MSG_MAX");
_Static_assert(sizeof(wm_notify_request_t) <= 512, "protocol messages must fit IPC_MSG_MAX");
_Static_assert(sizeof(wm_path_request_t) <= 512 && sizeof(wm_path_request_t) != sizeof(wm_request_t) &&
               sizeof(wm_path_request_t) != sizeof(wm_menu_request_t) &&
               sizeof(wm_path_request_t) != sizeof(wm_notify_request_t) &&
               sizeof(wm_path_request_t) != sizeof(wm_clip_request_t),
               "the dwm tells requests apart by size");
_Static_assert(sizeof(wm_clip_request_t) != sizeof(wm_request_t) &&
               sizeof(wm_clip_request_t) != sizeof(wm_menu_request_t) &&
               sizeof(wm_clip_request_t) != sizeof(wm_notify_request_t),
               "the dwm tells requests apart by size");
_Static_assert(sizeof(wm_tray_request_t) <= 512 && sizeof(wm_tray_request_t) != sizeof(wm_request_t) &&
               sizeof(wm_tray_request_t) != sizeof(wm_menu_request_t) &&
               sizeof(wm_tray_request_t) != sizeof(wm_notify_request_t) &&
               sizeof(wm_tray_request_t) != sizeof(wm_clip_request_t) &&
               sizeof(wm_tray_request_t) != sizeof(wm_path_request_t),
               "the dwm tells requests apart by size");
_Static_assert(sizeof(wm_tray_panel_request_t) <= 512 &&
               sizeof(wm_tray_panel_request_t) != sizeof(wm_request_t) &&
               sizeof(wm_tray_panel_request_t) != sizeof(wm_menu_request_t) &&
               sizeof(wm_tray_panel_request_t) != sizeof(wm_notify_request_t) &&
               sizeof(wm_tray_panel_request_t) != sizeof(wm_clip_request_t) &&
               sizeof(wm_tray_panel_request_t) != sizeof(wm_path_request_t) &&
               sizeof(wm_tray_panel_request_t) != sizeof(wm_tray_request_t),
               "the dwm tells requests apart by size");
_Static_assert(sizeof(wm_request_t) != sizeof(wm_menu_request_t) &&
               sizeof(wm_notify_request_t) != sizeof(wm_request_t) &&
               sizeof(wm_notify_request_t) != sizeof(wm_menu_request_t),
               "the dwm tells requests apart by size");

#endif

