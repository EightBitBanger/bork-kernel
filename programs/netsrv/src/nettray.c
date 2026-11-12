/*
 * Network tray service (nettray.h)
 *
 * A Wi-Fi icon on the taskbar. A left click opens a list of the networks
 * in range, strongest first, hidden ones as "(hidden)", and starts a fresh
 * scan. The list is the start menu mirrored: against the screen's right
 * edge, on top of the taskbar, one fixed size, with a scroll bar (and the
 * arrow, Page and Home / End keys, and the mouse wheel) for more networks
 * than it has lines.
 * Another click on the icon, Esc, or clicking anywhere else closes it. A
 * right click has Scan now, Disconnect (while connected), and Enable and
 * Disable (the Wi-Fi on or off). The desktop keeps that menu and opens it
 * itself on a right click, the way the taskbar opens a window's menu
 * (wm_tray_set_menu); it is sent again whenever an item's state changes.
 * An older desktop gets it the old way, in answer to the click.
 *
 * Connecting (in the desktop's tray panel): clicking a network joins it.
 * One with a padlock asks for its password first, a hidden one for its
 * name, in a box at the bottom of the panel; the password goes straight to
 * the kernel (WLAN_IOCTL_CONNECT), which turns it into the key, and is
 * wiped here. The panel then follows the connection: "connecting", then
 * "connected" with the address DHCP gave, or a line saying why it failed
 * (a wrong password, out of range, ...). No scans while connected: the
 * radio stays on the network's channel. The panel has buttons too, on
 * desktops that have them: refresh (left of the heading: scan again),
 * Connect under the password box (the same as Enter), and Disconnect along
 * the bottom while connected.
 *
 * Disable (the right-click menu) turns the Wi-Fi off until Enable: it
 * disconnects, stops scanning and lets go of the adapter, so nothing keeps
 * the radio awake (the driver puts the chip to sleep when it isn't
 * scanning or joined). The icon goes grey and the panel says it's off.
 * Only this copy of netsrv remembers it: it is on again after a restart.
 *
 * The icon lights one more arc per signal bar of the strongest network
 * heard (dot only: weak; all three arcs: strong), and goes grey with no
 * adapter, no permission, or the Wi-Fi turned off. While connected it turns blue and its arcs
 * show the connected network's signal instead.
 *
 * One at a time: a second copy finds the "netsrv" port taken (ports are
 * unique system-wide and go away with the process that made them) and
 * stops before it puts anything in the tray.
 *
 * Permissions: the desktop starts this with file_read, gfx, ipc and
 * network only. Started by hand (the desktop's usual mask, without
 * network), the first click asks the user once through the desktop's
 * prompt, takes the network bit, and gives up admin, exec, file writing,
 * the console and elevating in the same step, so nothing it could start
 * or write would carry the network bit.
 *
 * Network names come off the air from anyone: they are shown escaped
 * (netlist.h), so a name can't be mistaken for "(hidden)" or for another.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <os/input.h>
#include <os/ipc.h>
#include <os/os.h>
#include <os/security.h>
#include <os/wm.h>
#include "netlist.h"
#include "nettray.h"
#include "wlan_dev.h"

#define ICON_PX             16

#define SCAN_POLL_MS        200         // Results asked for this often while a scan runs
#define SCAN_TIMEOUT_MS     20000       // Then shown as they are
#define RETRY_MS            1000        // The adapter was still being identified
#define RETRY_MAX           10

// Popup layout, in pixels or (_COLS) character cells
#define PAD                 8
#define BARS_W              13
#define LOCK_W              7
#define GAP                 6
#define NAME_COLS           24
#define LEVEL_COLS          8           // "-100 dBm"
#define SCROLL_W            13          // The scroll bar
#define WHEEL_LINES         3           // Lines a wheel notch scrolls (Ctrl: a page)
#define LIST_ROWS           12          // Lines in the list, fewer on a short screen
#define FOCUS_GRACE_MS      300         // A focus loss this soon after showing is ignored

#define STYLE_POPUP (WM_STYLE_NOBORDER | WM_STYLE_TOPMOST | WM_STYLE_NOMINIMIZE | WM_STYLE_HIDDEN)

// Bits a tray icon never needs: given up as soon as admin lets us
#define SEC_UNNEEDED (SEC_FILE_WRITE | SEC_CONSOLE | SEC_EXEC | SEC_ELEVATE | SEC_INPUT | SEC_GFX_SCREEN)

enum { CMD_SCAN = 1, CMD_EXIT_UNUSED, CMD_DISCONNECT, CMD_ENABLE, CMD_DISABLE };     // Ids kept as they were

#define LINK_POLL_MS        500         // The connection asked about this often while it changes
#define NOTE_MAX            48

typedef enum { ASK_NONE = 0, ASK_NAME, ASK_PASSWORD } ask_t;

typedef enum {
    PROBLEM_NONE = 0,
    PROBLEM_ASK,                // No network bit yet: the first click asks for it
    PROBLEM_NO_PERMISSION,      // No network bit and no way to ask
    PROBLEM_REFUSED,            // The user said no (another click asks again)
    PROBLEM_NO_ADAPTER,
    PROBLEM_CANT_SCAN,          // ENOTTY: a chip without a radio driver yet
    PROBLEM_RADIO,              // The radio would not come up
    PROBLEM_FORMAT,             // A kernel with a newer results format
    PROBLEM_OTHER,              // `error` says
} problem_t;

// The list's fixed size, worked out once from the font. Rows: lines in the
// list area (a message line, when there is one, takes the first).
typedef struct {
    int32_t cw, lh;             // Cell width, line height
    int32_t row_h, head_h;
    int32_t rows;
    int32_t w, h;
    int32_t list_y;             // Top of the list area
    int32_t sb_x;               // Left of the scroll bar
} layout_t;

static struct {
    // The adapter
    int             fd;
    uint32_t        index;
    struct WlanInfo info;

    // Scanning
    bool            scanning;
    bool            scanned;            // At least one scan finished
    uint64_t        poll_at;            // Next results poll (scanning)
    uint64_t        scan_until;
    uint64_t        retry_at;           // 0: no retry waiting
    uint32_t        retries;
    uint8_t         channel;            // Being listened to
    problem_t       problem;
    int             error;
    netlist_t       list;
    struct WlanScanResults raw;

    // One copy only: our port, held while we run (0: none)
    int             instance;

    // Turned off from the menu (Disable): no adapter open, no scans
    bool            radio_off;

    // The icon
    int8_t          icon_bars;          // -1: grey
    bool            icon_blue;          // Drawn as connected
    char            tip[64];

    // The desktop's tray panel (wm_tray_set_panel): when it has one, a left
    // click opens that and the list goes there instead of the popup
    bool            panel;
    bool            panel_sent;         // What it shows is `sent`
    bool            panel_old;          // A desktop without the panel's buttons (it said EINVAL)
    bool            prompt_old;         // ... or without the Connect button under the box
    bool            menu_kept;          // The desktop keeps our right-click menu (wm_tray_set_menu)
    bool            menu_old;           // ... or it couldn't
    uint32_t        menu_sent[5];       // The items' flags it has
    struct {
        char                title[WM_TRAY_PANEL_HEAD_MAX];
        char                status[WM_TRAY_PANEL_HEAD_MAX];
        uint32_t            flags;      // WM_PANEL_*
        size_t              count;
        wm_tray_panel_row_t rows[WM_TRAY_PANEL_ROWS_MAX];
    } sent;
    int16_t         row_entry[WM_TRAY_PANEL_ROWS_MAX];  // Panel row -> list entry, -1: a note

    // Connecting
    struct WlanLink link;               // As the kernel last said
    bool            link_known;         // The adapter can connect (answers WLAN_IOCTL_LINK)
    uint64_t        link_poll_at;       // 0: not polling
    char            note[NOTE_MAX];     // What happened last, "": nothing to say
    ask_t           asking;
    netlist_entry_t chosen;             // The network being asked about

    // The popup: one window of one size, made on the first click and
    // hidden and shown after that
    wm_window_t     popup;
    bool            open;               // On the screen
    int32_t         x, y;               // Where it was put
    int32_t         top;                // First network line shown (scrolling)
    bool            drag;               // The scroll bar's thumb is held
    int32_t         drag_y, drag_top;
    bool            commit_failed;      // Said so once already
    uint64_t        shown_at;
    int32_t         anchor_x, anchor_y, anchor_w, anchor_h;     // The icon clicked
    wm_theme_t      theme;
    bool            have_theme;
    layout_t        lay;
} nt = { .fd = -1, .icon_bars = -1 };

static uint64_t now(void) {
    return os_time_ms();
}

static void popup_draw(void);
static void panel_send(void);
static void poll_link(void);
static bool link_busy(void);
static void short_name(const uint8_t* ssid, uint32_t length, char* out, size_t size);

//
// Permissions
//

// Admin already (started by an admin): take the network bit, drop the rest
static void trim_permissions(void) {
    if (!sec_is_admin())
        return;
    if (!sec_has(SEC_NET | SEC_NET_CONFIG))
        sec_grant(0, SEC_NET | SEC_NET_CONFIG);
    sec_drop((sec_get() & SEC_UNNEEDED) | SEC_ADMIN);
}

// The network bit, asking the user if we must. True when we hold it.
static bool get_permission(void) {
    if (sec_has(SEC_NET | SEC_NET_CONFIG))
        return true;
    if (!sec_has(SEC_ELEVATE)) {
        nt.problem = PROBLEM_NO_PERMISSION;
        return false;
    }
    // Just the network and its settings: the prompt lists those two, and
    // nothing more is granted
    if (sec_elevate_bits("Let the network tray icon list and join Wi-Fi networks",
                         SEC_NET | SEC_NET_CONFIG) < 0) {
        nt.problem = (errno == EACCES || errno == ETIMEDOUT || errno == EBUSY) ? PROBLEM_REFUSED : PROBLEM_NO_PERMISSION;
        return false;
    }
    sec_drop(sec_get() & (SEC_UNNEEDED | SEC_ADMIN));     // Ours to drop, admin or not

    nt.problem = PROBLEM_NONE;
    return true;
}

//
// The adapter
//

static void close_adapter(void) {
    wlan_dev_close(nt.fd);
    nt.fd = -1;
}

// The first wlanN that answers. False with nt.problem set.
static bool open_adapter(void) {
    if (nt.fd >= 0)
        return true;
    if (!sec_has(SEC_NET)) {
        nt.problem = sec_has(SEC_ELEVATE) ? PROBLEM_REFUSED : PROBLEM_NO_PERMISSION;
        return false;
    }
    for (uint32_t i = 0; i < WLAN_ADAPTER_MAX; i++) {
        int fd = wlan_dev_open(i);
        if (fd < 0) {
            if (errno == EPERM) {
                nt.problem = PROBLEM_NO_PERMISSION;
                return false;
            }
            continue;
        }
        struct WlanInfo info;
        memset(&info, 0, sizeof(info));
        if (wlan_dev_info(fd, &info) == 0 && info.state != WLAN_STATE_GONE) {
            info.chip[sizeof(info.chip) - 1] = '\0';
            nt.fd    = fd;
            nt.index = i;
            nt.info  = info;
            poll_link();            // Connected already (the console), or able to connect at all
            return true;
        }
        wlan_dev_close(fd);
    }
    nt.problem = PROBLEM_NO_ADAPTER;
    return false;
}

//
// The tray icon
//

// A Wi-Fi fan: a dot and three arcs over it, `lit` of the four parts lit
// (-1: none, all grey). `blue`: connected.
static void draw_fan(uint32_t* pic, int lit, bool blue) {
    // In half pixels from the dot's middle: (7.5 + 0.5, 13), x left to right, y up
    static const int32_t bands[4][2] = { { 0, 12 }, { 49, 121 }, { 196, 324 }, { 441, 625 } };
    uint32_t on  = blue ? NETTRAY_ICON_BLUE : 0xFFF2F6FFu;
    uint32_t off = blue ? NETTRAY_ICON_BLUE_DIM : 0x58FFFFFFu;
    for (int32_t y = 0; y < ICON_PX; y++) {
        for (int32_t x = 0; x < ICON_PX; x++) {
            int32_t dx = 2 * x + 1 - ICON_PX, dy = 26 - 2 * y;
            int32_t d2 = dx * dx + dy * dy;
            uint32_t c = 0;
            for (int k = 0; k < 4; k++) {
                bool in = d2 >= bands[k][0] && d2 <= bands[k][1];
                if (k > 0)
                    in = in && dy > 0 && (dx < 0 ? -dx : dx) <= dy;     // The 90 degrees above
                if (in)
                    c = (k < lit) ? on : off;
            }
            pic[y * ICON_PX + x] = c;
        }
    }
}

static int8_t best_bars(void) {
    if (nt.fd < 0 || nt.radio_off)
        return -1;
    if (nt.link_known && nt.link.state == WLAN_LINK_CONNECTED)
        return (int8_t)netlist_bars(nt.link.rssi);
    return nt.list.count > 0 ? (int8_t)nt.list.entry[0].bars : 0;
}

static void update_tip(char* out, size_t size) {
    if (nt.radio_off) {
        snprintf(out, size, "Wi-Fi: off (right-click to enable)");
        return;
    }
    switch (nt.problem) {
        case PROBLEM_ASK:           snprintf(out, size, "Wi-Fi: click to list networks"); return;
        case PROBLEM_NO_PERMISSION: snprintf(out, size, "Wi-Fi: no network permission"); return;
        case PROBLEM_REFUSED:       snprintf(out, size, "Wi-Fi: click to allow listing networks"); return;
        case PROBLEM_NO_ADAPTER:    snprintf(out, size, "Wi-Fi: no adapter"); return;
        case PROBLEM_CANT_SCAN:     snprintf(out, size, "Wi-Fi: %s can't scan yet", nt.info.chip); return;
        case PROBLEM_RADIO:         snprintf(out, size, "Wi-Fi: the radio did not come up"); return;
        default: break;
    }
    if (nt.link_known && nt.link.state == WLAN_LINK_CONNECTED) {
        char name[24];
        short_name(nt.link.ssid, nt.link.ssid_length, name, sizeof(name));
        snprintf(out, size, "Wi-Fi: connected to %s", name);
    } else if (nt.scanning && !nt.scanned)
        snprintf(out, size, "Wi-Fi: scanning");
    else if (!nt.scanned)
        snprintf(out, size, "Wi-Fi: click to scan");
    else if (nt.list.count == 0)
        snprintf(out, size, "Wi-Fi: no networks found");
    else
        snprintf(out, size, "Wi-Fi: %u network%s nearby", (unsigned)nt.list.count, nt.list.count == 1 ? "" : "s");
}

// The picture and tooltip, sent only when they changed
static bool connected(void) {
    return nt.fd >= 0 && nt.link_known && nt.link.state == WLAN_LINK_CONNECTED;
}

static void update_icon(void) {
    int8_t bars = best_bars();
    int lit = bars < 0 ? -1 : bars;
    bool blue = connected();
    if (bars != nt.icon_bars || blue != nt.icon_blue) {
        static uint32_t pic[ICON_PX * ICON_PX];
        draw_fan(pic, lit, blue);
        if (wm_tray_set_icon(NETTRAY_ICON_ID, pic, ICON_PX, ICON_PX) == 0) {
            nt.icon_bars = bars;
            nt.icon_blue = blue;
        }
    }
    char tip[sizeof(nt.tip)];
    update_tip(tip, sizeof(tip));
    if (strcmp(tip, nt.tip) != 0 && wm_tray_set_tip(NETTRAY_ICON_ID, tip) == 0)
        strcpy(nt.tip, tip);
}

// The right-click menu as things are now: Scan now, Disconnect, then
// Enable and Disable (the one in effect ticked and greyed)
#define MENU_ITEMS  5
static size_t menu_items(wm_menu_item_t* items) {
    uint32_t scan = (nt.scanning || !sec_has(SEC_NET) || link_busy() || nt.radio_off) ? WM_MENU_DISABLED : 0;
    wm_menu_item_t all[MENU_ITEMS] = {
        { "Scan now",   CMD_SCAN,       scan },
        { "Disconnect", CMD_DISCONNECT, link_busy() && !nt.radio_off ? 0 : WM_MENU_DISABLED },
        { NULL,         0,              WM_MENU_SEPARATOR },
        { "Enable",     CMD_ENABLE,     nt.radio_off ? 0 : (WM_MENU_DISABLED | WM_MENU_CHECKED) },
        { "Disable",    CMD_DISABLE,    nt.radio_off ? (WM_MENU_DISABLED | WM_MENU_CHECKED) : 0 },
    };
    memcpy(items, all, sizeof(all));
    return 5;
}

// Give the desktop the menu to keep, when it changed. Only a desktop that
// took the panel's buttons knows how (older ones would wait for a click).
static void menu_send(void) {
    if (!nt.panel || !nt.panel_sent || nt.panel_old || nt.menu_old)
        return;
    wm_menu_item_t items[MENU_ITEMS];
    size_t n = menu_items(items);
    uint32_t flags[5];
    for (size_t i = 0; i < n; i++)
        flags[i] = items[i].flags;
    if (nt.menu_kept && memcmp(flags, nt.menu_sent, sizeof(flags)) == 0)
        return;
    if (wm_tray_set_menu(NETTRAY_ICON_ID, items, n) < 0) {
        if (errno == EINVAL) {
            nt.menu_old  = true;        // Answer clicks instead
            nt.menu_kept = false;
        }
        return;
    }
    nt.menu_kept = true;
    memcpy(nt.menu_sent, flags, sizeof(flags));
}

// Anything shown changed
static void changed(void) {
    update_icon();
    if (nt.panel) {
        panel_send();
        menu_send();
    }
    if (nt.open)
        popup_draw();
}

//
// Scanning
//

static void scan_failed(int e) {
    nt.scanning = false;
    switch (e) {
        case ENOTTY: nt.problem = PROBLEM_CANT_SCAN; break;
        case EIO:    nt.problem = PROBLEM_RADIO;     break;
        case EPERM:  nt.problem = sec_has(SEC_ELEVATE) ? PROBLEM_REFUSED : PROBLEM_NO_PERMISSION; break;
        case ENOENT:                                 // Unplugged
            close_adapter();
            nt.list.count = 0;
            nt.scanned = false;
            nt.problem = PROBLEM_NO_ADAPTER;
            break;
        default:     nt.problem = PROBLEM_OTHER; nt.error = e; break;
    }
}

static void start_scan(void) {
    if (nt.scanning || nt.radio_off)
        return;
    nt.retry_at = 0;
    nt.problem  = PROBLEM_NONE;
    if (!open_adapter()) {
        changed();
        return;
    }
    if (link_busy()) {
        changed();                  // The radio stays on the network's channel
        return;
    }
    if (wlan_dev_scan(nt.fd) < 0 && errno != EBUSY) {       // EBUSY: one is running, use it
        int e = errno;
        if (e == ENOENT) {
            // Unplugged since: another one may be there
            close_adapter();
            if (open_adapter() && (wlan_dev_scan(nt.fd) == 0 || errno == EBUSY))
                goto running;
            e = (nt.fd >= 0) ? errno : ENOENT;
        }
        if (e == EAGAIN && nt.retries < RETRY_MAX) {
            nt.retries++;
            nt.retry_at = now() + RETRY_MS;     // Still identifying it
        } else {
            scan_failed(e);
        }
        changed();
        return;
    }
running:
    nt.retries    = 0;
    nt.scanning   = true;
    nt.channel    = 0;
    nt.poll_at    = now() + SCAN_POLL_MS;
    nt.scan_until = now() + SCAN_TIMEOUT_MS;
    changed();
}

static void poll_scan(void) {
    if (wlan_dev_results(nt.fd, &nt.raw) < 0) {
        scan_failed(errno);
        changed();
        return;
    }
    if (nt.raw.version != WLAN_SCAN_VERSION) {
        nt.scanning = false;
        nt.problem  = PROBLEM_FORMAT;
        changed();
        return;
    }
    if (nt.raw.state == WLAN_SCAN_FAILED) {
        scan_failed(EIO);
        changed();
        return;
    }
    // A scan starts its table empty. The first one shows what was heard so
    // far, so the list fills in; later ones keep showing the last full list
    // until they finish, so the list doesn't empty and refill on every click.
    bool finished = nt.raw.state != WLAN_SCAN_RUNNING || now() >= nt.scan_until;
    if (finished || !nt.scanned)
        netlist_build(&nt.raw, &nt.list);
    nt.channel = nt.raw.channel;
    if (finished) {
        nt.scanning = false;
        nt.scanned  = true;
    } else {
        nt.poll_at = now() + SCAN_POLL_MS;
    }
    changed();
}

//
// The popup
//

static uint32_t opaque(uint32_t c) {
    return c | 0xFF000000u;
}

// a over b, `alpha` of 255
static uint32_t mix(uint32_t a, uint32_t b, uint32_t alpha) {
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
        out |= ((ca * alpha + cb * (255 - alpha)) / 255) << shift;
    }
    return out;
}


static void theme_load(void) {
    nt.have_theme = wm_get_theme(&nt.theme) == 0;
    if (!nt.have_theme) {
        memset(&nt.theme, 0, sizeof(nt.theme));
        nt.theme.menu.background = 0xFF222222u;
        nt.theme.menu.border     = 0xFF444444u;
        nt.theme.menu.separator  = 0xFF111111u;
        nt.theme.menu.highlight  = 0xFF777777u;
        nt.theme.menu.text       = 0xFFE0E0E0u;
        nt.theme.client.accent   = 0xFF04C004u;
    }
}

// The line saying why there is no list, or NULL
static const char* message(char* buf, size_t size) {
    if (nt.radio_off)
        return "Wi-Fi is off. Right-click the icon to enable";
    switch (nt.problem) {
        case PROBLEM_ASK:           return "Asking for network permission...";
        case PROBLEM_NO_PERMISSION: return "This program has no network permission";
        case PROBLEM_REFUSED:       return "Not allowed. Click the icon to ask again";
        case PROBLEM_NO_ADAPTER:    return "No Wi-Fi adapter plugged in";
        case PROBLEM_CANT_SCAN:     snprintf(buf, size, "%s can't scan yet", nt.info.chip); return buf;
        case PROBLEM_RADIO:         return "The radio did not come up";
        case PROBLEM_FORMAT:        return "The kernel's scan format is newer";
        case PROBLEM_OTHER:         snprintf(buf, size, "Scan failed: %s", strerror(nt.error)); return buf;
        default: break;
    }
    if (nt.list.count > 0)
        return NULL;
    if (nt.scanning || nt.retry_at != 0)
        return "Listening for networks...";
    return nt.scanned ? "No networks found" : "Not scanned yet";
}

// The taskbar's top. The icon sits in the middle of the taskbar's height,
// which ends at the screen's bottom: its top is as far above the icon's
// middle as the bottom is below.
static int32_t taskbar_top(int32_t screen_h) {
    int32_t middle = nt.anchor_y + nt.anchor_h / 2;
    int32_t top = (screen_h > middle) ? 2 * middle - screen_h : nt.anchor_y;
    if (top > nt.anchor_y) top = nt.anchor_y;
    return top > 0 ? top : screen_h;
}

// The size, from the font's cells (measured on a 1 x 1 surface: the window
// is made once, at its final size), and the room above the taskbar
static bool measure(layout_t* l, int32_t screen_h) {
    gfx_surface_t* probe = gfx_surface_create(1, 1);
    if (probe == NULL)
        return false;
    memset(l, 0, sizeof(*l));
    l->cw     = gfx_char_width(probe);
    l->lh     = gfx_line_height(probe);
    gfx_surface_destroy(probe);
    l->row_h  = l->lh + 6 < 16 ? 16 : l->lh + 6;
    l->head_h = l->lh + 10;
    l->list_y = l->head_h + 1;
    l->w      = PAD + BARS_W + GAP + LOCK_W + GAP + NAME_COLS * l->cw + GAP + LEVEL_COLS * l->cw + PAD + SCROLL_W;
    l->sb_x   = l->w - 1 - SCROLL_W;

    int32_t room = taskbar_top(screen_h) - l->list_y - 2;
    l->rows = room / l->row_h;
    if (l->rows > LIST_ROWS) l->rows = LIST_ROWS;
    if (l->rows < 3) l->rows = 3;
    l->h = l->list_y + l->rows * l->row_h + 1;
    return true;
}

// Network lines that fit under the message line (if any)
static int32_t visible_rows(const layout_t* l) {
    char buf[64];
    return l->rows - (message(buf, sizeof(buf)) != NULL ? 1 : 0);
}

static int32_t max_top(const layout_t* l) {
    int32_t over = (int32_t)nt.list.count - visible_rows(l);
    return over > 0 ? over : 0;
}

static void clamp_top(void) {
    int32_t m = max_top(&nt.lay);
    if (nt.top > m) nt.top = m;
    if (nt.top < 0) nt.top = 0;
}

// The scroll bar's parts, client coordinates: arrows at both ends, the
// track between them and the thumb on it
typedef struct {
    gfx_rect_t up, down, track, thumb;
} scrollbar_t;

static scrollbar_t scrollbar(const layout_t* l) {
    scrollbar_t b;
    int32_t y0 = l->list_y, y1 = l->list_y + l->rows * l->row_h;
    b.up    = gfx_rect(l->sb_x, y0, SCROLL_W, SCROLL_W);
    b.down  = gfx_rect(l->sb_x, y1 - SCROLL_W, SCROLL_W, SCROLL_W);
    b.track = gfx_rect(l->sb_x, y0 + SCROLL_W, SCROLL_W, y1 - y0 - 2 * SCROLL_W);
    int32_t count = (int32_t)nt.list.count, vis = visible_rows(l), m = max_top(l);
    int32_t th = count > 0 ? b.track.h * vis / count : b.track.h;
    if (th < 10) th = 10;
    if (th > b.track.h) th = b.track.h;
    int32_t ty = b.track.y + (m > 0 ? (b.track.h - th) * nt.top / m : 0);
    b.thumb = gfx_rect(l->sb_x + 2, ty, SCROLL_W - 4, th);
    return b;
}

static bool inside(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// Signal bars: four, rising left to right, `n` lit
static void draw_bars(gfx_surface_t* s, int32_t x, int32_t bottom, int n, uint32_t on, uint32_t off) {
    for (int i = 0; i < 4; i++) {
        int32_t bh = 3 + 3 * i;
        gfx_fill_rect(s, x + i * 3, bottom - bh, 2, bh, i < n ? on : off);
    }
}

// A small padlock: the network asks for a key
static void draw_lock(gfx_surface_t* s, int32_t x, int32_t y, uint32_t c) {
    gfx_draw_rect(s, x + 1, y, 5, 4, c);
    gfx_fill_rect(s, x, y + 3, 7, 5, c);
}

// A small triangle in an arrow button, pointing up or down
static void draw_arrow(gfx_surface_t* s, gfx_rect_t r, bool up, uint32_t c) {
    int32_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int32_t i = 0; i < 3; i++) {
        int32_t y = up ? cy - 1 + i : cy + 1 - i;
        gfx_draw_hline(s, cx - i, y, 2 * i + 1, c);
    }
}

// Text cut to `cols` cells, ".." at the end when it was longer
static void draw_cut(gfx_surface_t* s, int32_t x, int32_t y, const char* text, int32_t cols, uint32_t c) {
    size_t n = strlen(text);
    if ((int32_t)n <= cols) {
        gfx_draw_text(s, x, y, text, c);
        return;
    }
    gfx_draw_text_n(s, x, y, text, (size_t)(cols - 2), c);
    gfx_draw_text(s, x + (cols - 2) * gfx_char_width(s), y, "..", c);
}

// The heading's two halves: what this is, and how the scan goes
static void heading(char* head, size_t head_size, char* status, size_t status_size) {
    if (nt.fd >= 0 && nt.info.chip[0] != '\0')
        snprintf(head, head_size, "Wi-Fi  wlan%u", (unsigned)nt.index);
    else
        snprintf(head, head_size, "Wi-Fi");
    if (nt.radio_off)
        snprintf(status, status_size, "off");
    else if (nt.link_known && nt.link.state == WLAN_LINK_CONNECTED && nt.link.ip != 0)
        snprintf(status, status_size, "%u.%u.%u.%u", (unsigned)(nt.link.ip >> 24), (unsigned)(nt.link.ip >> 16 & 255),
                 (unsigned)(nt.link.ip >> 8 & 255), (unsigned)(nt.link.ip & 255));
    else if (nt.link_known && nt.link.state == WLAN_LINK_CONNECTED)
        snprintf(status, status_size, "connected");
    else if (nt.link_known && nt.link.state != WLAN_LINK_IDLE)
        snprintf(status, status_size, "connecting");
    else if (nt.scanning && nt.channel != 0)
        snprintf(status, status_size, "scanning ch %u", (unsigned)nt.channel);
    else if (nt.scanning || nt.retry_at != 0)
        snprintf(status, status_size, "scanning");
    else if (nt.scanned && nt.problem == PROBLEM_NONE)
        snprintf(status, status_size, "%u found", (unsigned)nt.list.count);
    else
        status[0] = '\0';
}

// A signal level as shown at the end of a line
static void level_text(const netlist_entry_t* e, char* out, size_t size) {
    if (e->rssi <= WLAN_RSSI_UNKNOWN)
        snprintf(out, size, "?");
    else
        snprintf(out, size, "%d dBm", (int)e->rssi);
}

static void draw_popup(gfx_surface_t* s, const layout_t* l) {
    uint32_t bg     = opaque(nt.theme.menu.background);
    uint32_t text   = opaque(nt.theme.menu.text);
    uint32_t border = opaque(nt.theme.menu.border);
    uint32_t muted  = mix(text, bg, 140);
    uint32_t dim    = mix(text, bg, 60);
    uint32_t accent = opaque(nt.theme.client.accent);
    int32_t w = l->w, h = l->h;

    gfx_clear(s, bg);
    gfx_draw_rect(s, 0, 0, w, h, border);

    // Heading: what this is, and how the scan goes
    char head[48], status[32];
    heading(head, sizeof(head), status, sizeof(status));

    int32_t ty = gfx_text_y(s, 0, l->head_h);
    gfx_draw_text(s, PAD, ty, head, text);
    gfx_draw_text(s, w - PAD - gfx_text_width(s, status), ty, status, nt.scanning ? accent : muted);
    gfx_draw_hline(s, 1, l->head_h, w - 2, border);

    int32_t y = l->list_y;
    int32_t name_x  = PAD + BARS_W + GAP + LOCK_W + GAP;
    int32_t level_r = l->sb_x - PAD / 2;
    char buf[64];
    const char* msg = message(buf, sizeof(buf));
    if (msg != NULL) {
        draw_cut(s, PAD, gfx_text_y(s, y, l->row_h), msg, (l->sb_x - 2 * PAD) / l->cw, muted);
        y += l->row_h;
    }

    clamp_top();
    int32_t vis = visible_rows(l);
    for (int32_t i = 0; i < vis && nt.top + i < (int32_t)nt.list.count; i++, y += l->row_h) {
        const netlist_entry_t* e = &nt.list.entry[nt.top + i];
        int32_t mid = y + l->row_h / 2;
        draw_bars(s, PAD, mid + 5, e->bars, text, dim);
        if (e->security != 0)
            draw_lock(s, PAD + BARS_W + GAP, mid - 4, muted);
        int32_t tyr = gfx_text_y(s, y, l->row_h);
        draw_cut(s, name_x, tyr, e->name, NAME_COLS, e->hidden ? muted : text);

        char level[16];
        level_text(e, level, sizeof(level));
        gfx_draw_text(s, level_r - gfx_text_width(s, level), tyr, level, muted);
    }

    // The scroll bar: always there (the window never changes size), its
    // thumb filling the track while everything fits
    scrollbar_t b = scrollbar(l);
    bool can = max_top(l) > 0;
    gfx_draw_vline(s, l->sb_x - 1, l->list_y, l->rows * l->row_h, border);
    gfx_fill_rect(s, b.track.x, b.track.y, b.track.w, b.track.h, mix(text, bg, 25));
    gfx_fill_rect(s, b.thumb.x, b.thumb.y, b.thumb.w, b.thumb.h, can ? (nt.drag ? text : muted) : dim);
    draw_arrow(s, b.up,   true,  can && nt.top > 0 ? text : dim);
    draw_arrow(s, b.down, false, can && nt.top < max_top(l) ? text : dim);
}

//
// The desktop's tray panel
//

// Send what the list shows now, if it differs from what was sent last. The
// desktop draws it and scrolls it; a message line (when there is one) goes
// first.
static void panel_send(void) {
    static wm_tray_panel_row_t rows[WM_TRAY_PANEL_ROWS_MAX];
    char head[WM_TRAY_PANEL_HEAD_MAX], status[WM_TRAY_PANEL_HEAD_MAX], buf[64];
    heading(head, sizeof(head), status, sizeof(status));
    bool busy = nt.scanning || (nt.link_known && nt.link.state != WLAN_LINK_IDLE &&
                                nt.link.state != WLAN_LINK_CONNECTED);

    memset(rows, 0, sizeof(rows));
    size_t n = 0;
    const char* notes[2] = { nt.note[0] != '\0' ? nt.note : NULL, message(buf, sizeof(buf)) };
    for (int k = 0; k < 2; k++) {
        if (notes[k] == NULL)
            continue;
        if (strlcpy(rows[n].text, notes[k], sizeof(rows[n].text)) >= sizeof(rows[n].text))
            memcpy(rows[n].text + sizeof(rows[n].text) - 3, "..", 3);
        rows[n].level = WM_PANEL_NO_LEVEL;
        rows[n].flags = WM_PANEL_ROW_NOTE;
        nt.row_entry[n] = -1;
        n++;
    }
    for (uint32_t i = 0; i < nt.list.count && n < WM_TRAY_PANEL_ROWS_MAX; i++, n++) {
        nt.row_entry[n] = (int16_t)i;
        const netlist_entry_t* e = &nt.list.entry[i];
        wm_tray_panel_row_t* r = &rows[n];
        size_t len = strlen(e->name);
        if (len < sizeof(r->text)) {
            memcpy(r->text, e->name, len + 1);
        } else {
            // Escaped names run long: cut, ".." at the end
            memcpy(r->text, e->name, sizeof(r->text) - 3);
            memcpy(r->text + sizeof(r->text) - 3, "..", 3);
        }
        level_text(e, r->side, sizeof(r->side));
        r->level = e->bars > 4 ? 4 : e->bars;
        r->flags = (e->security != 0 ? WM_PANEL_ROW_LOCK : 0) | (e->hidden ? WM_PANEL_ROW_MUTED : 0) |
                   WM_PANEL_ROW_CLICK;
        if (nt.link_known && nt.link.state != WLAN_LINK_IDLE && memcmp(nt.link.bssid, e->bssid, 6) == 0) {
            r->flags |= WM_PANEL_ROW_ACTIVE;
            strlcpy(r->side, nt.link.state == WLAN_LINK_CONNECTED ? "connected" : "joining", sizeof(r->side));
        }
    }

    // The buttons: refresh while the Wi-Fi is on, Disconnect while connected
    uint32_t flags = busy ? WM_PANEL_BUSY : 0;
    if (!nt.panel_old) {
        if (!nt.radio_off)
            flags |= WM_PANEL_HAS_REFRESH;
        if (connected())
            flags |= WM_PANEL_HAS_DISCONNECT;
    }

    if (nt.panel_sent && n == nt.sent.count && flags == nt.sent.flags && strcmp(head, nt.sent.title) == 0 &&
        strcmp(status, nt.sent.status) == 0 && memcmp(rows, nt.sent.rows, n * sizeof(rows[0])) == 0)
        return;     // The same: every scan poll would send it again
    int r = wm_tray_set_panel_ex(NETTRAY_ICON_ID, head, status, flags, rows, n);
    if (r < 0 && errno == EINVAL && (flags & ~(uint32_t)WM_PANEL_BUSY)) {
        // An older desktop: no buttons, the menu still has it all
        nt.panel_old = true;
        flags &= WM_PANEL_BUSY;
        r = wm_tray_set_panel_ex(NETTRAY_ICON_ID, head, status, flags, rows, n);
    }
    if (r < 0) {
        nt.panel_sent = false;
        return;
    }
    nt.panel_sent = true;
    strlcpy(nt.sent.title, head, sizeof(nt.sent.title));
    strlcpy(nt.sent.status, status, sizeof(nt.sent.status));
    nt.sent.flags = flags;
    nt.sent.count = n;
    memcpy(nt.sent.rows, rows, n * sizeof(rows[0]));
}

// Where the list goes: the start menu's place mirrored, against the
// screen's right edge and on top of the taskbar
static void place(const layout_t* l, int32_t* x, int32_t* y) {
    int32_t sw, sh;
    wm_screen_size(&sw, &sh);
    *x = sw - l->w;
    if (*x < 0) *x = 0;
    *y = taskbar_top(sh) - l->h;
    if (*y < 0) *y = 0;
}

static void popup_failed(const char* what) {
    char text[96];
    snprintf(text, sizeof(text), "%s: %s", what, strerror(errno));
    wm_notify_ex(WM_NOTIFY_ERROR, "Wi-Fi list", text, WM_NOTIFY_DEFAULT_MS);
}

// Draw and commit what the list shows now
static void popup_draw(void) {
    if (!nt.open || nt.popup == 0)
        return;
    gfx_surface_t* s = wm_window_surface(nt.popup);
    if (s == NULL)
        return;
    draw_popup(s, &nt.lay);
    if (wm_window_commit(nt.popup, NULL) < 0 && !nt.commit_failed) {
        nt.commit_failed = true;            // Said once: it would repeat every poll
        popup_failed("drawing failed");
    }
}

// Make the window (once, at its one size), place it and show it
static void popup_show(void) {
    if (!nt.have_theme)
        theme_load();
    int32_t sw, sh;
    wm_screen_size(&sw, &sh);

    if (nt.popup == 0) {
        if (!measure(&nt.lay, sh)) {
            popup_failed("no surface to measure the font on");
            nt.open = false;
            return;
        }
        int32_t x, y;
        place(&nt.lay, &x, &y);
        wm_window_desc_t d = { .title = "Wi-Fi networks", .x = x, .y = y,
                               .width = nt.lay.w, .height = nt.lay.h, .style = STYLE_POPUP };
        nt.popup = wm_window_create(&d);
        if (nt.popup == 0) {
            popup_failed("no window");
            nt.open = false;
            return;
        }
        nt.x = x;
        nt.y = y;
    } else {
        // The icon may have moved along the tray since
        int32_t x, y;
        place(&nt.lay, &x, &y);
        if (x != nt.x || y != nt.y)
            wm_window_move(nt.popup, x, y);
        nt.x = x;
        nt.y = y;
    }

    popup_draw();
    if (wm_window_show(nt.popup, true) < 0)      // Raised and focused: clicking elsewhere closes it
        popup_failed("can't show the window");
    nt.shown_at = now();
}

static void popup_close(void) {
    if (!nt.open)
        return;
    nt.open = false;
    nt.drag = false;
    if (nt.popup != 0)
        wm_window_show(nt.popup, false);
}

static void popup_open(const wm_event_t* ev) {
    nt.anchor_x = ev->rect_x;
    nt.anchor_y = ev->rect_y;
    nt.anchor_w = ev->rect_w;
    nt.anchor_h = ev->rect_h;
    nt.open     = true;
    nt.top      = 0;
    popup_show();
}

static void scroll_to(int32_t top) {
    int32_t old = nt.top;
    nt.top = top;
    clamp_top();
    if (nt.top != old)
        popup_draw();
}

// A press in the list: the scroll bar's arrows step a line, the track a
// page, and the thumb drags
static void popup_press(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    scrollbar_t b = scrollbar(&nt.lay);
    int32_t page = visible_rows(&nt.lay) - 1;
    if (page < 1) page = 1;
    if (inside(b.up, ev->x, ev->y)) {
        scroll_to(nt.top - 1);
    } else if (inside(b.down, ev->x, ev->y)) {
        scroll_to(nt.top + 1);
    } else if (inside(b.thumb, ev->x, ev->y)) {
        nt.drag     = true;
        nt.drag_y   = ev->y;
        nt.drag_top = nt.top;
        popup_draw();
    } else if (inside(b.track, ev->x, ev->y)) {
        scroll_to(ev->y < b.thumb.y ? nt.top - page : nt.top + page);
    }
}

static void popup_drag(const wm_event_t* ev) {
    if (!nt.drag)
        return;
    scrollbar_t b = scrollbar(&nt.lay);
    int32_t span = b.track.h - b.thumb.h, m = max_top(&nt.lay);
    if (span <= 0 || m == 0)
        return;
    int32_t dy = ev->y - nt.drag_y;
    scroll_to(nt.drag_top + (dy * m + (dy >= 0 ? span / 2 : -span / 2)) / span);
}

// The wheel: WHEEL_LINES a notch, Ctrl a page a notch
static void popup_wheel(const wm_event_t* ev) {
    if (ev->wheel == 0 || nt.drag)
        return;
    int32_t step = WHEEL_LINES;
    if (ev->mods & INPUT_MOD_CTRL) {
        step = visible_rows(&nt.lay) - 1;
        if (step < 1) step = 1;
    }
    scroll_to(nt.top + ev->wheel * step);
}

static void popup_key(uint16_t key) {
    int32_t page = visible_rows(&nt.lay) - 1;
    if (page < 1) page = 1;
    switch (key) {
        case KEY_ESCAPE:    popup_close();                   break;
        case KEY_UP:        scroll_to(nt.top - 1);           break;
        case KEY_DOWN:      scroll_to(nt.top + 1);           break;
        case KEY_PAGE_UP:   scroll_to(nt.top - page);        break;
        case KEY_PAGE_DOWN: scroll_to(nt.top + page);        break;
        case KEY_HOME:      scroll_to(0);                    break;
        case KEY_END:       scroll_to(max_top(&nt.lay));     break;
        default: break;
    }
}

//
// Connecting
//

static bool link_busy(void) {
    return nt.link_known && nt.link.state != WLAN_LINK_IDLE;
}

// A network's name for a note: escaped, short, or "the hidden network"
static void short_name(const uint8_t* ssid, uint32_t length, char* out, size_t size) {
    char name[NETLIST_NAME_MAX];
    if (length == 0) {
        strlcpy(out, "the hidden network", size);
        return;
    }
    netlist_escape(ssid, length > WLAN_SSID_MAX ? WLAN_SSID_MAX : length, name);
    if (strlen(name) >= 20)
        strcpy(name + 17, "..");
    strlcpy(out, name, size);
}

static void set_note(const char* text) {
    strlcpy(nt.note, text != NULL ? text : "", sizeof(nt.note));
}

// Which part of a network's security we can't do (WlanLink.detail), short
// enough for the panel's line
static void unsupported_text(uint8_t detail, const char* name, char* text, size_t size) {
    switch (detail) {
        case WLAN_DETAIL_NO_RSN:   snprintf(text, size, "%s is WPA1 only", name);         break;
        case WLAN_DETAIL_GROUP:    snprintf(text, size, "%s uses WEP", name);             break;
        case WLAN_DETAIL_PAIRWISE: snprintf(text, size, "%s is TKIP only: use AES", name); break;
        case WLAN_DETAIL_AKM:      snprintf(text, size, "%s needs WPA3", name);           break;
        case WLAN_DETAIL_PMF:      snprintf(text, size, "%s requires PMF", name);         break;
        case WLAN_DETAIL_CHANGED:  snprintf(text, size, "%s: keys didn't match", name);   break;
        case WLAN_DETAIL_GTK:      snprintf(text, size, "%s: bad group key", name);       break;
        case WLAN_DETAIL_OPEN:     snprintf(text, size, "%s wants a password", name);     break;
        default:                   snprintf(text, size, "%s: security not supported", name); break;
    }
}

// Why the last connection or attempt ended, as a note
static void note_reason(void) {
    char name[24];
    short_name(nt.link.ssid, nt.link.ssid_length, name, sizeof(name));
    char text[NOTE_MAX];
    switch (nt.link.reason) {
        case WLAN_REASON_NOT_FOUND:   snprintf(text, sizeof(text), "%s didn't answer", name);        break;
        case WLAN_REASON_AUTH:
        case WLAN_REASON_ASSOC:       snprintf(text, sizeof(text), "%s turned us away", name);       break;
        case WLAN_REASON_PASSWORD:    snprintf(text, sizeof(text), "Wrong password for %s", name);   break;
        case WLAN_REASON_UNSUPPORTED: unsupported_text(nt.link.detail, name, text, sizeof(text));    break;
        case WLAN_REASON_LEFT:        snprintf(text, sizeof(text), "%s disconnected us", name);      break;
        case WLAN_REASON_LOST:        snprintf(text, sizeof(text), "Lost %s", name);                 break;
        case WLAN_REASON_RADIO:       snprintf(text, sizeof(text), "The adapter failed to send");   break;
        default:                      text[0] = '\0';                                               break;
    }
    set_note(text);
}

// Ask the kernel how the connection is. Changes show.
static void poll_link(void) {
    nt.link_poll_at = 0;
    if (nt.fd < 0)
        return;
    struct WlanLink l;
    memset(&l, 0, sizeof(l));
    if (wlan_dev_link(nt.fd, &l) < 0) {
        nt.link_known = false;          // ENOTTY: this adapter can't connect yet
        return;
    }
    uint8_t was = nt.link_known ? nt.link.state : WLAN_LINK_IDLE;
    bool same = nt.link_known && memcmp(&l, &nt.link, sizeof(l)) == 0;
    nt.link = l;
    nt.link_known = true;
    if (l.state != WLAN_LINK_IDLE)
        nt.link_poll_at = now() + LINK_POLL_MS;     // Until it settles (and while connected: for the address, a drop)
    if (same)
        return;
    if (l.state == WLAN_LINK_IDLE && was != WLAN_LINK_IDLE)
        note_reason();
    else if (l.state == WLAN_LINK_CONNECTED && was != WLAN_LINK_CONNECTED)
        set_note(NULL);
    changed();
}

// Join `e` with `password` ("" for an open network). The password is
// wiped here, whatever happens.
static void connect_to(const netlist_entry_t* e, char* password) {
    struct WlanConnect c;
    memset(&c, 0, sizeof(c));
    c.version = WLAN_CONNECT_VERSION;
    memcpy(c.bssid, e->bssid, 6);
    c.ssid_length = e->ssid_length;
    memcpy(c.ssid, e->ssid, e->ssid_length);
    size_t len = strlen(password);
    c.passphrase_length = (uint8_t)len;
    memcpy(c.passphrase, password, len);
    volatile char* v = password;
    for (size_t i = 0; i < len; i++)
        v[i] = 0;

    char name[24], text[NOTE_MAX];
    short_name(e->ssid, e->ssid_length, name, sizeof(name));
    int r = (nt.fd >= 0) ? wlan_dev_connect(nt.fd, &c) : (errno = ENODEV, -1);
    int e_no = errno;
    volatile uint8_t* w = (volatile uint8_t*)&c;
    for (size_t i = 0; i < sizeof(c); i++)
        w[i] = 0;
    if (r < 0) {
        // EINVAL: security it can't do (the link says which part), else a
        // name that no longer matches the scan
        struct WlanLink l;
        memset(&l, 0, sizeof(l));
        bool refused = e_no == EINVAL && nt.fd >= 0 && wlan_dev_link(nt.fd, &l) == 0 &&
                       l.reason == WLAN_REASON_UNSUPPORTED;
        switch (e_no) {
            case ENOTTY: snprintf(text, sizeof(text), "%s can't connect yet", nt.info.chip); break;
            case ENOENT: snprintf(text, sizeof(text), "%s is gone: scan again", name);       break;
            case EINVAL:
                if (refused)
                    unsupported_text(l.detail, name, text, sizeof(text));
                else
                    snprintf(text, sizeof(text), "%s changed: scan again", name);
                break;
            case EBUSY:  snprintf(text, sizeof(text), "Still scanning: try again");         break;
            default:     snprintf(text, sizeof(text), "Can't connect: %s", strerror(e_no)); break;
        }
        set_note(text);
        changed();
        return;
    }
    snprintf(text, sizeof(text), "Connecting to %s...", name);
    set_note(text);
    poll_link();
    changed();
}

// The question at the bottom of the panel, with a Connect button under its
// box when the answer is the last thing asked (an older desktop: Enter only)
static void prompt(const char* question, bool secret, bool button) {
    uint32_t flags = secret ? WM_TRAY_SECRET : 0;
    if (button && !nt.prompt_old) {
        if (wm_tray_prompt_ex(NETTRAY_ICON_ID, question, flags | WM_TRAY_CONNECT) == 0 || errno != EINVAL)
            return;
        nt.prompt_old = true;
    }
    wm_tray_prompt_ex(NETTRAY_ICON_ID, question, flags);
}

static void ask(ask_t what) {
    char question[WM_TRAY_TIP_MAX], name[24];
    nt.asking = what;
    if (what == ASK_NAME) {
        prompt("Name of the hidden network:", false, nt.chosen.security == 0);
        return;
    }
    short_name(nt.chosen.ssid, nt.chosen.ssid_length, name, sizeof(name));
    snprintf(question, sizeof(question), "Password for %s:", name);
    prompt(question, true, true);
}

static bool supported(uint8_t security) {
    if (security & (WLAN_SEC_WEP | WLAN_SEC_ENTERPRISE))
        return false;
    return security == 0 || (security & WLAN_SEC_WPA2);    // WPA2, or WPA2 / WPA3 mixed
}

// A network's row was clicked
static void choose(uint32_t row) {
    if (row >= WM_TRAY_PANEL_ROWS_MAX || nt.row_entry[row] < 0 || (uint32_t)nt.row_entry[row] >= nt.list.count)
        return;
    if (nt.radio_off)
        return;
    const netlist_entry_t* e = &nt.list.entry[nt.row_entry[row]];
    char name[24], text[NOTE_MAX];
    short_name(e->ssid, e->ssid_length, name, sizeof(name));
    if (link_busy() && memcmp(nt.link.bssid, e->bssid, 6) == 0)
        return;                     // That one already
    if (!supported(e->security)) {
        const char* kind = (e->security & WLAN_SEC_ENTERPRISE) ? "enterprise" :
                           (e->security & WLAN_SEC_WEP) ? "WEP" :
                           (e->security & WLAN_SEC_WPA3) ? "WPA3 only" : "WPA1 only";
        snprintf(text, sizeof(text), "%s: %s isn't supported", name, kind);
        set_note(text);
        changed();
        return;
    }
    nt.chosen = *e;
    set_note(NULL);
    if (e->hidden) {
        ask(ASK_NAME);
    } else if (e->security != 0) {
        ask(ASK_PASSWORD);
    } else {
        char none[1] = "";
        connect_to(e, none);
    }
    changed();
}

// The prompt was answered
static void answered(bool accepted) {
    ask_t what = nt.asking;
    nt.asking = ASK_NONE;
    if (!accepted || what == ASK_NONE || nt.radio_off)
        return;
    char text[WM_TRAY_TEXT_MAX];
    if (wm_tray_prompt_text(NETTRAY_ICON_ID, text, sizeof(text)) < 0)
        return;
    size_t len = strlen(text);
    if (what == ASK_NAME) {
        if (len == 0 || len > WLAN_SSID_MAX) {
            set_note("Names are 1 to 32 characters");
            changed();
            ask(ASK_NAME);
            return;
        }
        nt.chosen.ssid_length = (uint8_t)len;
        memcpy(nt.chosen.ssid, text, len);
        if (nt.chosen.security != 0) {
            ask(ASK_PASSWORD);
        } else {
            char none[1] = "";
            connect_to(&nt.chosen, none);
        }
        return;
    }
    bool hex = len == 64;
    for (size_t i = 0; hex && i < len; i++)
        hex = (text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
              (text[i] >= 'A' && text[i] <= 'F');
    // WPA2 passwords are plain printable ASCII (the kernel refuses others)
    bool ascii = true;
    for (size_t i = 0; ascii && i < len; i++)
        ascii = (unsigned char)text[i] >= 0x20 && (unsigned char)text[i] <= 0x7E;
    if ((!hex && (len < 8 || len > 63)) || !ascii) {
        volatile char* v = text;
        for (size_t i = 0; i < sizeof(text); i++)
            v[i] = 0;
        set_note(ascii ? "Passwords are 8 to 63 characters" : "Passwords use plain ASCII only");
        changed();
        ask(ASK_PASSWORD);
        return;
    }
    connect_to(&nt.chosen, text);       // Wipes it
}

static void disconnect(void) {
    if (nt.fd >= 0)
        wlan_dev_disconnect(nt.fd);
    poll_link();
    set_note(NULL);
    changed();
}

// The panel's refresh button (or F5): scan again. Not while connected (the
// radio stays on the network's channel): the connection is asked about
// again instead.
static void refresh(void) {
    if (nt.radio_off)
        return;
    if (!sec_has(SEC_NET) && !get_permission()) {
        changed();
        return;
    }
    if (link_busy()) {
        poll_link();
        changed();
        return;
    }
    set_note(NULL);
    start_scan();
}

// Disable: off the network, no scans, the adapter let go (the driver puts
// the chip to sleep when nothing is using the radio)
static void radio_disable(void) {
    if (nt.radio_off)
        return;
    if (nt.asking != ASK_NONE) {
        nt.asking = ASK_NONE;
        wm_tray_prompt(NETTRAY_ICON_ID, NULL, false);   // The question goes
    }
    if (nt.fd >= 0 && link_busy())
        wlan_dev_disconnect(nt.fd);
    close_adapter();
    nt.radio_off    = true;
    nt.scanning     = false;
    nt.scanned      = false;
    nt.retry_at     = 0;
    nt.retries      = 0;
    nt.link_poll_at = 0;
    nt.link_known   = false;
    nt.list.count   = 0;
    nt.top          = 0;
    if (nt.problem != PROBLEM_NO_PERMISSION && nt.problem != PROBLEM_ASK && nt.problem != PROBLEM_REFUSED)
        nt.problem = PROBLEM_NONE;
    memset(&nt.link, 0, sizeof(nt.link));
    set_note(NULL);
    changed();
}

// Enable: back on, and a fresh scan (when we may: the first click asks
// for the permission as before)
static void radio_enable(void) {
    if (!nt.radio_off)
        return;
    nt.radio_off = false;
    set_note(NULL);
    if (sec_has(SEC_NET)) {
        start_scan();
    } else {
        nt.problem = sec_has(SEC_ELEVATE) ? PROBLEM_ASK : PROBLEM_NO_PERMISSION;
        changed();
    }
}

//
// Events
//

static void left_click(const wm_event_t* ev) {
    if (nt.panel) {
        // The desktop opened its panel on the press, showing the list sent
        // last: ask for permission if need be, then scan again (not while
        // connected: the radio stays on the network's channel)
        if (nt.radio_off) {
            changed();              // It says the Wi-Fi is off
            return;
        }
        if (!sec_has(SEC_NET))
            get_permission();
        if (nt.link_known && nt.link.state == WLAN_LINK_IDLE && nt.link.reason != WLAN_REASON_NONE)
            set_note(NULL);         // Said already
        if (sec_has(SEC_NET) && !link_busy())
            start_scan();
        else
            changed();
        return;
    }
    if (nt.open) {
        popup_close();
        return;
    }
    if (!sec_has(SEC_NET) && !nt.radio_off)
        get_permission();
    popup_open(ev);
    if (sec_has(SEC_NET) && !nt.radio_off)
        start_scan();
    else
        changed();
}

// The desktop opened the menu it keeps already; an older one waits for
// this answer
static void right_click(const wm_event_t* ev) {
    if (nt.menu_kept)
        return;
    wm_menu_item_t items[MENU_ITEMS];
    size_t n = menu_items(items);
    wm_tray_menu(ev->tray, items, n);
}

bool nettray_handle(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_TRAY:
            if (ev->tray != NETTRAY_ICON_ID)
                break;
            if (ev->button == INPUT_BTN_LEFT)
                left_click(ev);
            else if (ev->button == INPUT_BTN_RIGHT)
                right_click(ev);
            break;
        case WM_EVENT_TRAY_COMMAND:
            if (ev->tray != NETTRAY_ICON_ID)
                break;
            if (ev->param == CMD_SCAN && !link_busy())
                start_scan();
            if (ev->param == CMD_DISCONNECT)
                disconnect();
            if (ev->param == CMD_ENABLE)
                radio_enable();
            if (ev->param == CMD_DISABLE)
                radio_disable();
            break;
        case WM_EVENT_TRAY_PANEL:
            if (ev->tray != NETTRAY_ICON_ID)
                break;
            if (ev->param == WM_PANEL_ACCEPT || ev->param == WM_PANEL_CANCEL)
                answered(ev->param == WM_PANEL_ACCEPT);
            else if (ev->param == WM_PANEL_REFRESH)
                refresh();
            else if (ev->param == WM_PANEL_DISCONNECT)
                disconnect();
            else
                choose(ev->param);
            break;
        case WM_EVENT_THEME_CHANGED:
            // The font may have changed too: the window is made again, at
            // its new size, the next time it opens
            nt.have_theme = false;
            popup_close();
            if (nt.popup != 0)
                wm_window_destroy(nt.popup);
            nt.popup = 0;
            break;
        default:
            if (ev->window == 0 || ev->window != nt.popup)
                break;
            switch (ev->type) {
                case WM_EVENT_FOCUS_LOST:
                    // Something taking the focus straight after the click
                    // that opened the list would close it unseen
                    if (now() - nt.shown_at >= FOCUS_GRACE_MS)
                        popup_close();
                    break;
                case WM_EVENT_CLOSE:      popup_close();          break;
                case WM_EVENT_KEY:        popup_key(ev->key);     break;
                case WM_EVENT_MOUSE_DOWN: popup_press(ev);        break;
                case WM_EVENT_MOUSE_MOVE: popup_drag(ev);         break;
                case WM_EVENT_MOUSE_WHEEL: popup_wheel(ev);       break;
                case WM_EVENT_MOUSE_UP:
                    if (nt.drag) {
                        nt.drag = false;
                        popup_draw();
                    }
                    break;
                case WM_EVENT_DESTROYED:
                    nt.popup = 0;
                    nt.open  = false;
                    break;
                default: break;
            }
            break;
    }
    return true;
}

void nettray_tick(void) {
    uint64_t t = now();
    if (nt.retry_at != 0 && t >= nt.retry_at) {
        nt.retry_at = 0;
        start_scan();
    }
    if (nt.scanning && t >= nt.poll_at)
        poll_scan();
    if (nt.link_poll_at != 0 && t >= nt.link_poll_at)
        poll_link();
}

int32_t nettray_timeout(void) {
    uint64_t due = 0;
    if (nt.scanning)
        due = nt.poll_at;
    if (nt.retry_at != 0 && (due == 0 || nt.retry_at < due))
        due = nt.retry_at;
    if (nt.link_poll_at != 0 && (due == 0 || nt.link_poll_at < due))
        due = nt.link_poll_at;
    if (due == 0)
        return -1;
    uint64_t t = now();
    return due <= t ? 0 : (int32_t)(due - t);
}

int nettray_start(void) {
    trim_permissions();

    // Already running: the port is taken (EEXIST)
    int port = ipc_port_create(NETTRAY_INSTANCE_PORT);
    if (port < 0)
        return -1;
    nt.instance = port;

    static uint32_t pic[ICON_PX * ICON_PX];
    draw_fan(pic, -1, false);
    if (wm_tray_add(NETTRAY_ICON_ID, pic, ICON_PX, ICON_PX, "Wi-Fi") < 0) {
        int err = errno;
        ipc_close(nt.instance);
        nt.instance = 0;
        errno = err;
        return -1;
    }
    strcpy(nt.tip, "Wi-Fi");

    // A left click opens the desktop's tray panel (the start menu's,
    // mirrored), which shows the list this sends. Older desktops refuse the
    // flag (EINVAL): this program's own list window opens as before.
    nt.panel = wm_tray_set_panel(NETTRAY_ICON_ID) == 0;

    // A first scan now, so the icon says something before anyone clicks.
    // Without the network bit nothing is asked until the first click.
    if (sec_has(SEC_NET)) {
        start_scan();
    } else {
        nt.problem = sec_has(SEC_ELEVATE) ? PROBLEM_ASK : PROBLEM_NO_PERMISSION;
        changed();
    }
    return 0;
}

void nettray_scroll_state(int32_t* top, int32_t* max, gfx_rect_t* thumb) {
    *top   = nt.top;
    *max   = max_top(&nt.lay);
    *thumb = scrollbar(&nt.lay).thumb;
}

void nettray_stop(void) {
    close_adapter();
    if (nt.popup != 0)
        wm_window_destroy(nt.popup);
    nt.popup = 0;
    wm_tray_remove(NETTRAY_ICON_ID);
    if (nt.instance > 0)
        ipc_close(nt.instance);     // Another copy may start now
    memset(&nt, 0, sizeof(nt));
    nt.fd        = -1;
    nt.icon_bars = -1;
}
