/*
 * Network tray test: programs/nettray against the real DWM (server.c,
 * tray.c, window.c, input.c ...), the kernel's ipc/ipc.c and the kernel's
 * gfx code, all in this one process the way tray_test.c runs them. The
 * adapter is a pretend one (wlan_dev_* below, in place of wlan_dev.c),
 * which plays back scans and can be unplugged, refuse, or not be there.
 *
 * Clicks go through the DWM's real input path (input_route), so a click
 * on the icon, on the desktop and the Esc key do what they would on the
 * machine. `nettray_test preview.ppm` also writes the screen with the list
 * open, for the export's preview.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <os/gfx.h>
#include <os/font.h>
#include <os/input.h>
#include <os/ipc.h>
#include <os/os.h>
#include <os/security.h>
#include <os/wm.h>
#include <os/wm_proto.h>

#include "dwm/dwm.h"
#include "dwm/compositor.h"
#include "dwm/internal.h"
#include "dwm/server.h"
#include "dwm/tray.h"

#include "nettray.h"
#include "netlist.h"
#include "wlan_dev.h"

void     __test_ipc_set_pid(uint32_t pid);
void     __test_ipc_set_sleep_hook(void (*fn)(void));
uint32_t __test_gfx_screen_pixel(int32_t x, int32_t y);

#define PID_TRAY    201

static int failures = 0, checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); } \
} while (0)

//
// Pieces of dwm.c this test stands in for
//

int32_t dwm_screen_width(void)  { return compositor_width(); }
int32_t dwm_screen_height(void) { return compositor_height(); }
bool    dwm_warp_pointer(int32_t x, int32_t y) { (void)x; (void)y; return false; }

//
// The pretend adapter
//

typedef struct {
    const char* ssid;           // NULL: hidden (ssid_length 0)
    uint8_t     zeros;          // Hidden as that many zero bytes instead
    int8_t      rssi;
    uint8_t     channel;
    uint8_t     security;
    uint8_t     last;           // The byte that ends the BSSID
} air_t;

// What the air holds: names, levels, and two hostile names
static const air_t air[] = {
    { "Cafe\x1b[2J",        0, -77, 11, 0,                  1 },
    { "HomeNetwork",        0, -71,  6, WLAN_SEC_WPA2,      2 },    // A second, weaker access point
    { NULL,                 0, -51,  6, WLAN_SEC_WPA2,      3 },
    { "PrinterSetup-4F2A",  0, -83,  1, 0,                  4 },
    { "HomeNetwork",        0, -48,  6, WLAN_SEC_WPA2,      5 },
    { "Neighbor 5G",        0, -62,  1, WLAN_SEC_WPA2 | WLAN_SEC_WPA3, 6 },
    { NULL,                 6, -88, 13, WLAN_SEC_WPA2,      7 },    // All-zero name: hidden too
    { "xfinitywifi",        0, -70, 11, 0,                  8 },
    { "back\\slash",        0, -66,  3, WLAN_SEC_WPA2,      9 },
    { "Ghost",              0, -128, 9, 0,                 10 },    // Heard, level not known
};
#define AIR_COUNT   (sizeof(air) / sizeof(air[0]))

static struct {
    bool     present;           // wlan0 is there
    bool     unplug_next;       // Gone at the next results poll
    int      scan_error;        // SCAN fails with this (0: works)
    uint32_t again;             // SCAN says EAGAIN this many more times
    uint32_t opens, scans, polls;
    bool     running;
    uint8_t  channel;           // 1 .. 13 while running
    bool     fd_open;
} dev;

static uint32_t extra;

static void fill_bss(struct WlanBss* b, const air_t* a) {
    memset(b, 0, sizeof(*b));
    uint8_t bssid[6] = { 0x9c, 0x53, 0x22, 0x01, 0xaa, a->last };
    memcpy(b->bssid, bssid, 6);
    b->channel  = a->channel;
    b->rssi     = a->rssi;
    b->security = a->security;
    if (a->ssid != NULL) {
        b->ssid_length = (uint8_t)strlen(a->ssid);
        memcpy(b->ssid, a->ssid, b->ssid_length);
    } else {
        b->ssid_length = a->zeros;      // Bytes all zero
        b->flags = WLAN_BSS_HIDDEN;
    }
}

int wlan_dev_open(uint32_t index) {
    dev.opens++;
    if (!sec_has(SEC_NET)) { errno = EPERM; return -1; }      // As the kernel checks it
    if (index != 0 || !dev.present) { errno = ENOENT; return -1; }
    dev.fd_open = true;
    return 77;
}

void wlan_dev_close(int fd) {
    if (fd == 77)
        dev.fd_open = false;
}

int wlan_dev_info(int fd, struct WlanInfo* out) {
    if (fd != 77 || !dev.present) { errno = ENOENT; return -1; }
    memset(out, 0, sizeof(*out));
    out->version = 1;
    out->state   = WLAN_STATE_RADIO_ON;
    strcpy(out->chip, "RTL8192EU");
    return 0;
}

int wlan_dev_scan(int fd) {
    if (fd != 77 || !dev.present) { errno = ENOENT; return -1; }
    if (dev.again > 0) { dev.again--; errno = EAGAIN; return -1; }
    if (dev.scan_error) { errno = dev.scan_error; return -1; }
    if (dev.running) { errno = EBUSY; return -1; }
    dev.scans++;
    dev.running = true;
    dev.channel = 1;
    return 0;
}

// One channel further each poll; what was heard on the channels so far
int wlan_dev_results(int fd, struct WlanScanResults* out) {
    if (dev.unplug_next) {
        dev.unplug_next = false;
        dev.present = false;
        dev.running = false;
    }
    if (fd != 77 || !dev.present) { errno = ENOENT; return -1; }
    dev.polls++;
    memset(out, 0, sizeof(*out));
    out->version = WLAN_SCAN_VERSION;
    out->state = dev.running ? WLAN_SCAN_RUNNING : (dev.scans ? WLAN_SCAN_DONE : WLAN_SCAN_IDLE);
    out->channel = dev.channel;
    for (uint32_t i = 0; i < AIR_COUNT; i++)
        if (!dev.running || air[i].channel <= dev.channel)
            fill_bss(&out->bss[out->count++], &air[i]);
    static char names[32][16];
    for (uint32_t i = 0; i < extra && out->count < WLAN_SCAN_MAX; i++) {
        air_t a = { names[i], 0, (int8_t)(-50 - (int)i), (uint8_t)(1 + i % 13), WLAN_SEC_WPA2, (uint8_t)(100 + i) };
        snprintf(names[i], sizeof(names[i]), "Extra%02u", (unsigned)i);
        if (!dev.running || a.channel <= dev.channel)
            fill_bss(&out->bss[out->count++], &a);
    }
    if (dev.running && (dev.channel += 4) > 13) {
        dev.running = false;
        dev.channel = 0;
    }
    return 0;
}

// Connecting: off (ENOTTY, as a driver that can't) until a test turns it on
static struct {
    bool     can;
    struct WlanLink link;
    struct WlanConnect last;        // What the service asked for
    int      connects, disconnects;
    int      error;                 // CONNECT fails with this (0: works)
} net;

int wlan_dev_connect(int fd, const struct WlanConnect* c) {
    if (fd != 77 || !dev.present) { errno = ENOENT; return -1; }
    if (!net.can) { errno = ENOTTY; return -1; }
    net.connects++;
    net.last = *c;
    if (net.error) { errno = net.error; return -1; }
    memset(&net.link, 0, sizeof(net.link));
    net.link.version = WLAN_LINK_VERSION;
    net.link.state = WLAN_LINK_JOINING;
    memcpy(net.link.bssid, c->bssid, 6);
    net.link.ssid_length = c->ssid_length;
    memcpy(net.link.ssid, c->ssid, c->ssid_length);
    net.link.rssi = -48;
    return 0;
}

int wlan_dev_disconnect(int fd) {
    if (fd != 77 || !net.can) { errno = ENOTTY; return -1; }
    net.disconnects++;
    net.link.state  = WLAN_LINK_IDLE;
    net.link.reason = WLAN_REASON_USER;
    net.link.ip     = 0;
    return 0;
}

int wlan_dev_link(int fd, struct WlanLink* out) {
    if (fd != 77 || !dev.present) { errno = ENOENT; return -1; }
    if (!net.can) { errno = ENOTTY; return -1; }
    *out = net.link;
    out->version = WLAN_LINK_VERSION;
    return 0;
}

//
// The DWM's main loop and the service's, a round at a time
//

static void as(uint32_t pid) { __test_ipc_set_pid(pid); }

static void dwm_round(void) {
    as(0);
    server_poll();
    event_dispatch();
    server_flush();
    taskbar_tick();
    compositor_render();
}

static void hook(void) {
    dwm_round();
    __test_ipc_set_sleep_hook(hook);
}

static bool running = true;

// Both sides for `ms` (at least one round)
static void run_for(uint32_t ms) {
    uint64_t until = os_time_ms() + ms;
    do {
        dwm_round();
        as(PID_TRAY);
        wm_event_t ev;
        while (running && wm_wait_event(&ev, 0) > 0)
            running = nettray_handle(&ev);
        nettray_tick();
        dwm_round();
        if (ms > 0)
            os_sleep_ms(10);
    } while (os_time_ms() < until);
}

// Until the pretend scan is over (and a little more)
static void run_scan(void) {
    for (int i = 0; i < 100 && dev.running; i++)
        run_for(50);
    run_for(300);
}

static void mouse(int32_t x, int32_t y, uint8_t buttons) {
    as(0);
    input_event_t e;
    memset(&e, 0, sizeof(e));
    e.type    = INPUT_MOUSE;
    e.x       = (int16_t)x;
    e.y       = (int16_t)y;
    e.buttons = buttons;
    e.time_ms = (uint32_t)os_time_ms();
    input_route(&e);
}

static void key(uint16_t k) {
    as(0);
    input_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = INPUT_KEY;
    e.key  = k;
    input_route(&e);
}

// A click at (x, y), the way the mouse driver reports one
static void click_at(int32_t x, int32_t y, uint8_t button) {
    mouse(x, y, 0);
    mouse(x, y, button);
    mouse(x, y, 0);
    run_for(0);
}

static void click_icon(uint8_t button) {
    dwm_round();
    as(0);
    gfx_rect_t r = tray_icon_rect(0);
    click_at(r.x + r.w / 2, r.y + r.h / 2, button);
    os_sleep_ms(500);       // No double clicks between tests
    run_for(0);
}

// The popup: the one window the service has
static dwm_handle_t popup(void) {
    as(0);
    for (int32_t y = 0; y < compositor_height(); y += 4)
        for (int32_t x = 0; x < compositor_width(); x += 4) {
            dwm_handle_t w = dwm_window_at(x, y);
            if (w != 0 && w != DWM_DESKTOP && strcmp(dwm_window_get_title(w), "Wi-Fi networks") == 0)
                return w;
        }
    return 0;
}

static bool popup_open(void) {
    return popup() != 0;
}

//
// Tests
//

static void test_netlist(void) {
    struct WlanScanResults r;
    memset(&r, 0, sizeof(r));
    r.version = WLAN_SCAN_VERSION;
    r.state = WLAN_SCAN_DONE;
    for (uint32_t i = 0; i < AIR_COUNT; i++)
        fill_bss(&r.bss[r.count++], &air[i]);

    netlist_t l;
    netlist_build(&r, &l);

    // HomeNetwork once (two access points, the stronger kept), both hidden ones
    CHECK(l.count == AIR_COUNT - 1);
    static const char* order[] = {
        "HomeNetwork", NETLIST_HIDDEN, "Neighbor 5G", "back\\x5cslash", "xfinitywifi",
        "Cafe\\x1b[2J", "PrinterSetup-4F2A", NETLIST_HIDDEN, "Ghost",
    };
    for (uint32_t i = 0; i < l.count && i < sizeof(order) / sizeof(order[0]); i++)
        CHECK(strcmp(l.entry[i].name, order[i]) == 0);
    CHECK(l.entry[0].rssi == -48 && l.entry[0].aps == 2 && l.entry[0].bssid[5] == 5 && l.entry[0].bars == 4);
    CHECK(l.entry[1].hidden && l.entry[7].hidden && !l.entry[0].hidden);
    for (uint32_t i = 1; i + 1 < l.count; i++)
        CHECK(l.entry[i - 1].rssi >= l.entry[i].rssi);      // Strongest first, unknown level last
    CHECK(l.entry[l.count - 1].bars == 0);

    // Nothing in a name can reach the screen raw
    for (uint32_t i = 0; i < l.count; i++)
        for (const char* p = l.entry[i].name; *p; p++)
            CHECK(*p >= 0x20 && *p < 0x7F);

    // A name of only "(hidden)" is not a hidden network; nor are 32 bytes of 0xFF
    memset(&r, 0, sizeof(r));
    r.version = WLAN_SCAN_VERSION;
    r.count = 3;
    r.bss[0].ssid_length = 8;
    memcpy(r.bss[0].ssid, "(hidden)", 8);
    r.bss[0].rssi = -40;
    r.bss[1].ssid_length = 32;
    memset(r.bss[1].ssid, 0xFF, 32);
    r.bss[1].rssi = -41;
    r.bss[2].ssid_length = 200;             // Lying: more than WLAN_SSID_MAX
    memset(r.bss[2].ssid, 'A', 32);
    r.bss[2].rssi = -42;
    netlist_build(&r, &l);
    CHECK(l.count == 3 && !l.entry[0].hidden && !l.entry[1].hidden);
    CHECK(strlen(l.entry[1].name) == 4 * 32);
    CHECK(strlen(l.entry[2].name) == 32);

    // A count past the table is cut to it
    r.count = 250;
    netlist_build(&r, &l);
    CHECK(l.count <= WLAN_SCAN_MAX);

    CHECK(netlist_bars(-30) == 4 && netlist_bars(-60) == 3 && netlist_bars(-75) == 2 &&
          netlist_bars(-85) == 1 && netlist_bars(-95) == 0 && netlist_bars(-128) == 0);
}

// The icon's tooltip as the service last set it: wm_tray_add and
// wm_tray_set_tip are wrapped at link time (-Wl,--wrap) to see it
static char last_tip[64];
int __real_wm_tray_add(uint32_t id, const uint32_t* pixels, int32_t width, int32_t height, const char* tip);
int __real_wm_tray_set_tip(uint32_t id, const char* tip);

int __wrap_wm_tray_add(uint32_t id, const uint32_t* pixels, int32_t width, int32_t height, const char* tip) {
    int r = __real_wm_tray_add(id, pixels, width, height, tip);
    if (r == 0)
        snprintf(last_tip, sizeof(last_tip), "%s", tip ? tip : "");
    return r;
}

int __wrap_wm_tray_set_tip(uint32_t id, const char* tip) {
    int r = __real_wm_tray_set_tip(id, tip);
    if (r == 0)
        snprintf(last_tip, sizeof(last_tip), "%s", tip ? tip : "");
    return r;
}

// sec_drop is wrapped too: the test process is the DWM as well, so what the
// service gives up is recorded, not done
static sec_mask_t dropped;
int __wrap_sec_drop(sec_mask_t bits) {
    dropped |= bits;
    return 0;
}

// wm_tray_set_panel too: off (refused, as by an older desktop) for the
// tests of the service's own list window, on for test_panel
static bool panel_on;
int __real_wm_tray_set_panel(uint32_t id);
int __wrap_wm_tray_set_panel(uint32_t id) {
    if (panel_on)
        return __real_wm_tray_set_panel(id);
    errno = EINVAL;
    return -1;
}

static bool tip_is(const char* want) {
    if (strcmp(last_tip, want) != 0) {
        printf("  tip: \"%s\", wanted \"%s\"\n", last_tip, want);
        return false;
    }
    return true;
}

static bool placed_right(void);
static const char* preview_path;
static void write_preview(const char* path);

static void test_start_and_open(void) {
    dev.present = true;
    as(PID_TRAY);
    CHECK(nettray_start() == 0);
    run_for(0);
    CHECK(tray_count() == 1);
    CHECK(dropped == (SEC_FILE_WRITE | SEC_CONSOLE | SEC_EXEC | SEC_ELEVATE | SEC_INPUT | SEC_GFX_SCREEN | SEC_ADMIN));
    dropped = 0;
    CHECK(dev.opens == 1 && dev.scans == 1);        // A first scan right away
    run_scan();
    CHECK(tip_is("Wi-Fi: 9 networks nearby"));
    CHECK(!popup_open());

    // Left click: the list, above the icon, and a new scan
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    dwm_handle_t w = popup();
    CHECK(w != 0);
    CHECK(dev.scans == 2);
    run_scan();
    as(0);
    gfx_rect_t frame, client, icon = tray_icon_rect(0), bar = taskbar_rect();
    CHECK(dwm_window_get_rect(w, &frame, &client));
    CHECK(frame.y + frame.h <= bar.y && placed_right());             // On the taskbar, at the right edge
    CHECK(frame.x <= icon.x + icon.w && frame.x + frame.w >= icon.x);     // Over the icon
    CHECK(frame.x >= 0 && frame.x + frame.w <= compositor_width() && frame.y >= 0);
    CHECK(dwm_window_get_focus() == w);
    CHECK(dwm_window_visible(w) && (dwm_window_style(w) & DWM_STYLE_NOBORDER));

    // Click the icon again: closed
    click_icon(INPUT_BTN_LEFT);
    run_for(50);
    CHECK(!popup_open());

    // Open, then a click on the desktop closes it
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    CHECK(popup_open());
    click_at(40, 40, INPUT_BTN_LEFT);
    run_for(50);
    CHECK(!popup_open());

    // ... and so does Esc
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    CHECK(popup_open());
    key(KEY_ESCAPE);
    run_for(50);
    CHECK(!popup_open());
    run_scan();
}

static void test_menu(void) {
    uint32_t scans = dev.scans;
    click_icon(INPUT_BTN_RIGHT);
    run_for(50);
    as(0);
    CHECK(dwm_menu_is_open());
    menu_key(KEY_DOWN);                 // "Scan now"
    menu_key(KEY_ENTER);
    run_for(100);
    CHECK(dev.scans == scans + 1);
    run_scan();
}

static void test_problems(void) {
    // Still identifying the adapter: tried again a moment later
    dev.again = 2;
    uint32_t scans = dev.scans;
    click_icon(INPUT_BTN_LEFT);
    run_for(2500);
    CHECK(dev.scans == scans + 1 && dev.again == 0);
    run_scan();
    key(KEY_ESCAPE);
    run_for(50);

    // A chip that can't scan
    dev.scan_error = ENOTTY;
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    CHECK(popup_open() && tip_is("Wi-Fi: RTL8192EU can't scan yet"));
    key(KEY_ESCAPE);
    dev.scan_error = 0;
    run_for(50);

    // Unplugged in the middle of a scan: no adapter, grey icon, no crash
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    CHECK(dev.running);
    dev.unplug_next = true;
    run_for(400);
    CHECK(tip_is("Wi-Fi: no adapter") && !dev.fd_open && popup_open());
    key(KEY_ESCAPE);
    run_for(50);

    // Plugged back in: found again on the next click
    dev.present = true;
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    CHECK(dev.fd_open && dev.running);
    run_scan();
    CHECK(tip_is("Wi-Fi: 9 networks nearby"));
    key(KEY_ESCAPE);
    run_for(50);
}

// The list sits like the start menu, mirrored: against the screen's right
// edge, its bottom on the taskbar's top, and always the same size
static int32_t popup_w, popup_h;

static bool placed_right(void) {
    dwm_handle_t w = popup();
    as(0);
    gfx_rect_t frame, bar = taskbar_rect();
    if (w == 0 || !dwm_window_get_rect(w, &frame, NULL))
        return false;
    if (popup_w == 0) {
        popup_w = frame.w;
        popup_h = frame.h;
    }
    bool ok = frame.y + frame.h <= bar.y && frame.y + frame.h >= bar.y - 2 && frame.y >= 0 &&
              frame.x + frame.w == compositor_width() && frame.w == popup_w && frame.h == popup_h;
    if (!ok)
        printf("  popup at %d,%d %dx%d, taskbar top %d\n", (int)frame.x, (int)frame.y, (int)frame.w, (int)frame.h,
               (int)bar.y);
    return ok;
}

static int32_t scroll_top(void) {
    int32_t top, max;
    gfx_rect_t thumb;
    as(PID_TRAY);
    nettray_scroll_state(&top, &max, &thumb);
    return top;
}

// More networks than the list has lines (27, as on the machine it was
// reported on): one size and place throughout, a scroll bar and keys to
// see the rest, and a rescan doesn't empty it
static void test_many(void) {
    extra = 18;
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    CHECK(popup_open() && placed_right());
    run_scan();
    CHECK(tip_is("Wi-Fi: 27 networks nearby"));
    CHECK(placed_right());

    int32_t top, max;
    gfx_rect_t thumb, frame;
    as(PID_TRAY);
    nettray_scroll_state(&top, &max, &thumb);
    CHECK(top == 0 && max > 0 && max < 27);

    // Keys
    key(KEY_DOWN);  run_for(20); CHECK(scroll_top() == 1);
    key(KEY_END);   run_for(20); CHECK(scroll_top() == max);
    key(KEY_DOWN);  run_for(20); CHECK(scroll_top() == max);
    key(KEY_HOME);  run_for(20); CHECK(scroll_top() == 0);
    key(KEY_UP);    run_for(20); CHECK(scroll_top() == 0);
    key(KEY_PAGE_DOWN); run_for(20); CHECK(scroll_top() > 1);
    key(KEY_HOME);  run_for(20);

    // The mouse: the down arrow (bottom right), the track below the thumb,
    // then dragging the thumb back to the top
    as(0);
    CHECK(dwm_window_get_rect(popup(), &frame, NULL));
    click_at(frame.x + frame.w - 8, frame.y + frame.h - 6, INPUT_BTN_LEFT);
    run_for(20);
    CHECK(scroll_top() == 1);
    as(PID_TRAY);
    nettray_scroll_state(&top, &max, &thumb);
    click_at(frame.x + thumb.x + 2, frame.y + thumb.y + thumb.h + 3, INPUT_BTN_LEFT);
    run_for(20);
    CHECK(scroll_top() > 1);
    as(PID_TRAY);
    nettray_scroll_state(&top, &max, &thumb);
    int32_t tx = frame.x + thumb.x + 2, ty = frame.y + thumb.y + thumb.h / 2;
    mouse(tx, ty, 0);
    mouse(tx, ty, INPUT_BTN_LEFT);
    run_for(20);
    mouse(tx, ty + 400, INPUT_BTN_LEFT);    // Past the bottom: the end
    run_for(20);
    CHECK(scroll_top() == max);
    mouse(tx, ty - 400, INPUT_BTN_LEFT);    // Past the top: the start
    run_for(20);
    CHECK(scroll_top() == 0);
    mouse(tx, ty - 400, 0);
    run_for(20);
    CHECK(popup_open() && placed_right());   // Still open: the presses were on it

    if (false) {
        key(KEY_DOWN);
        key(KEY_DOWN);
        key(KEY_DOWN);
        mouse(20, 20, 0);       // The pointer out of the picture
        run_for(50);
        write_preview(preview_path);
    }
    key(KEY_ESCAPE);
    run_for(50);
    CHECK(!popup_open());

    // Open again: the last full list at once, kept while the new scan runs,
    // scrolled back to the top
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    CHECK(placed_right() && scroll_top() == 0);
    CHECK(dev.running);
    run_for(250);
    as(PID_TRAY);
    nettray_scroll_state(&top, &max, &thumb);
    CHECK(dev.running && max > 0);
    run_scan();
    CHECK(placed_right());
    key(KEY_ESCAPE);
    run_for(50);

    // Down to a few: the same window, nothing to scroll
    extra = 0;
    click_icon(INPUT_BTN_LEFT);
    run_scan();
    CHECK(placed_right());
    as(PID_TRAY);
    nettray_scroll_state(&top, &max, &thumb);
    CHECK(top == 0 && max == 0);
    key(KEY_ESCAPE);
    run_for(50);
}

static size_t panel_top(void) {
    size_t top, max;
    as(0);
    dwm_start_tray_scroll(&top, &max);
    return top;
}

// What the desktop's panel lists now (NULL: nothing sent)
static const dwm_tray_panel_t* panel_list(void) {
    void* owner;
    uint32_t id;
    as(0);
    if (!tray_icon_key(0, &owner, &id))
        return NULL;
    return tray_panel_get(owner, id);
}

// With a desktop that has the tray panel: a left press opens it at once,
// the start menu's place mirrored and a little smaller, listing what the
// service sent, strongest first; the service scans again as it opens, and
// the panel scrolls
static void test_panel(void) {
    as(PID_TRAY);
    nettray_stop();
    run_for(50);
    extra = 18;
    panel_on = true;
    as(PID_TRAY);
    CHECK(nettray_start() == 0);
    run_for(50);
    run_scan();
    panel_on = false;

    // Sent before any click, so it opens full
    const dwm_tray_panel_t* l = panel_list();
    CHECK(l != NULL && l->count == 27);
    if (l == NULL || l->count != 27)
        return;
    CHECK(strcmp(l->title, "Wi-Fi  wlan0") == 0 && strcmp(l->status, "27 found") == 0 && !l->busy);
    CHECK(l->rows[0].level == 4 && strcmp(l->rows[0].text, l->rows[0].text) == 0 && strstr(l->rows[0].side, "dBm") != NULL);
    bool sorted = true, hidden = false;
    for (size_t i = 1; i < l->count; i++) {
        if (l->rows[i].level > l->rows[i - 1].level)
            sorted = false;
        if (strcmp(l->rows[i].text, "(hidden)") == 0 && (l->rows[i].flags & DWM_PANEL_ROW_MUTED))
            hidden = true;
    }
    CHECK(sorted && hidden);

    uint32_t scans = dev.scans;
    click_icon(INPUT_BTN_LEFT);
    as(0);
    CHECK(dwm_start_is_tray() && !popup_open());
    run_for(100);
    CHECK(dev.scans == scans + 1);                  // The service heard of it: a new scan
    l = panel_list();
    CHECK(l != NULL && l->busy && l->count == 27);  // The last list stays while it runs
    run_scan();
    l = panel_list();
    CHECK(l != NULL && !l->busy && l->count == 27);

    // Where: against the right edge, on the taskbar, narrower and lower
    // than the start menu
    as(0);
    gfx_rect_t p = dwm_start_rect(), bar = taskbar_rect();
    CHECK(p.x + p.w == compositor_width() && p.y + p.h == bar.y);
    dwm_start_close();
    dwm_start_open();
    gfx_rect_t start = dwm_start_rect();
    dwm_start_close();
    CHECK(start.x == 0 && p.w < start.w && p.h < start.h && p.h > start.h / 2);

    // Scrolling: keys ...
    click_icon(INPUT_BTN_LEFT);
    run_scan();
    as(0);
    size_t top, max;
    dwm_start_tray_scroll(&top, &max);
    CHECK(dwm_start_is_tray() && top == 0 && max > 0 && max < 27);
    key(KEY_DOWN);      CHECK(panel_top() == 1);
    key(KEY_END);       CHECK(panel_top() == max);
    key(KEY_DOWN);      CHECK(panel_top() == max);
    key(KEY_HOME);      CHECK(panel_top() == 0);
    key(KEY_UP);        CHECK(panel_top() == 0);
    key(KEY_PAGE_DOWN); CHECK(panel_top() > 1);
    key(KEY_HOME);

    // ... the down arrow (bottom right, left of the strip), the track, and
    // dragging the thumb; none of them close it
    as(0);
    p = dwm_start_rect();
    int32_t sb_x = p.x + p.w - 1 - 6 - 7;
    click_at(sb_x, p.y + p.h - 1 - 6, INPUT_BTN_LEFT);
    as(0);
    CHECK(dwm_start_is_tray() && panel_top() == 1);
    click_at(sb_x, p.y + p.h - 30, INPUT_BTN_LEFT);
    CHECK(dwm_start_is_tray() && panel_top() > 1);
    key(KEY_HOME);
    int32_t thumb_y = p.y + 1 + dwm_text_height() + 10 + 1 + 13 + 4;     // Just inside the thumb, at the top
    mouse(sb_x, thumb_y, INPUT_BTN_LEFT);
    mouse(sb_x, thumb_y + 400, INPUT_BTN_LEFT);
    mouse(sb_x, thumb_y + 400, 0);
    CHECK(dwm_start_is_tray() && panel_top() == max);      // Let go below the panel: still open
    key(KEY_HOME);

    // A shorter list: the scrolling follows
    key(KEY_END);
    extra = 0;
    as(PID_TRAY);
    nettray_handle(&(wm_event_t){ .type = WM_EVENT_TRAY_COMMAND, .tray = NETTRAY_ICON_ID, .param = 1 });
    run_scan();
    as(0);
    dwm_start_tray_scroll(&top, &max);
    l = panel_list();
    CHECK(l != NULL && l->count == 9 && max == 0 && top == 0);
    extra = 18;
    as(PID_TRAY);
    nettray_handle(&(wm_event_t){ .type = WM_EVENT_TRAY_COMMAND, .tray = NETTRAY_ICON_ID, .param = 1 });
    run_scan();

    // Clicking the icon again closes it (and doesn't open it again)
    click_icon(INPUT_BTN_LEFT);
    run_for(50);
    as(0);
    CHECK(!dwm_start_is_open() && !popup_open());

    // Esc, and a click on the desktop, close it
    click_icon(INPUT_BTN_LEFT);
    as(0);
    CHECK(dwm_start_is_tray());
    key(KEY_ESCAPE);
    as(0);
    CHECK(!dwm_start_is_open());
    click_icon(INPUT_BTN_LEFT);
    as(0);
    CHECK(dwm_start_is_tray());
    click_at(40, 40, INPUT_BTN_LEFT);
    as(0);
    CHECK(!dwm_start_is_open());

    // The right button still gets the service's menu
    click_icon(INPUT_BTN_RIGHT);
    run_for(50);
    as(0);
    CHECK(dwm_menu_is_open() && !dwm_start_is_open());
    dwm_menu_close();

    if (preview_path != NULL) {
        click_icon(INPUT_BTN_LEFT);
        run_scan();
        mouse(20, 20, 0);
        key(KEY_DOWN);
        key(KEY_DOWN);
        run_for(50);
        write_preview(preview_path);
        key(KEY_ESCAPE);
        run_for(50);
    }

    // The service going away closes the panel and drops its list
    click_icon(INPUT_BTN_LEFT);
    as(0);
    CHECK(dwm_start_is_tray());
    as(PID_TRAY);
    nettray_stop();
    run_for(50);
    as(0);
    CHECK(!dwm_start_is_open() && tray_count() == 0);
}

// The panel row (counted from the top of the list) that shows `text`, -1: none
static int row_with(const char* text) {
    const dwm_tray_panel_t* l = panel_list();
    for (size_t i = 0; l != NULL && i < l->count; i++)
        if (strcmp(l->rows[i].text, text) == 0)
            return (int)i;
    return -1;
}

static bool first_row_is(const char* text) {
    const dwm_tray_panel_t* l = panel_list();
    if (l != NULL && l->count > 0 && strcmp(l->rows[0].text, text) == 0)
        return true;
    printf("  first row: \"%s\", wanted \"%s\"\n", l != NULL && l->count > 0 ? l->rows[0].text : "(none)", text);
    return false;
}

// Click panel row `i` (scrolled to the top first)
static void click_row(int i) {
    key(KEY_HOME);
    as(0);
    gfx_rect_t p = dwm_start_rect();
    int32_t rh = dwm_text_height() + 6 < 16 ? 16 : dwm_text_height() + 6;
    int32_t y = p.y + 1 + dwm_text_height() + 10 + 1 + i * rh + rh / 2;
    click_at(p.x + 60, y, INPUT_BTN_LEFT);
    run_for(50);
}

static void type(const char* text) {
    for (const char* c = text; *c; c++)
        key((uint16_t)*c);
}

static const char* prompt_now(void) {
    const dwm_tray_panel_t* l = panel_list();
    return l != NULL ? l->prompt : "";
}

// Connecting from the panel: a password asked for in its box and handed to
// the kernel, the connection followed, a wrong password said, a hidden
// network's name asked for, an open one joined at once
static void test_connect(void) {
    as(PID_TRAY);
    nettray_stop();
    run_for(50);
    extra = 0;
    net.can = true;
    memset(&net.link, 0, sizeof(net.link));
    panel_on = true;
    as(PID_TRAY);
    CHECK(nettray_start() == 0);
    run_scan();
    panel_on = false;
    click_icon(INPUT_BTN_LEFT);
    run_scan();
    as(0);
    CHECK(dwm_start_is_tray());

    // Rows are clickable; HomeNetwork (WPA2) asks for its password, secret
    int home = row_with("HomeNetwork");
    CHECK(home >= 0 && (panel_list()->rows[home].flags & DWM_PANEL_ROW_CLICK));
    click_row(home);
    as(0);
    CHECK(strcmp(prompt_now(), "Password for HomeNetwork:") == 0 && panel_list()->secret);
    CHECK(net.connects == 0);

    // Too short: said, and asked again
    type("short");
    key(KEY_ENTER);
    run_for(50);
    CHECK(first_row_is("Passwords are 8 to 63 characters"));
    CHECK(strcmp(prompt_now(), "Password for HomeNetwork:") == 0 && net.connects == 0);

    // The right length: to the kernel, with the network's BSSID and name
    type("correct horse");
    if (preview_path != NULL) {
        // A second picture: the password being typed (preview.ppm -> preview.ppm.prompt.ppm)
        char path[512];
        snprintf(path, sizeof(path), "%s.prompt.ppm", preview_path);
        mouse(20, 20, 0);
        run_for(50);
        write_preview(path);
    }
    key(KEY_ENTER);
    run_for(50);
    CHECK(net.connects == 1 && net.last.passphrase_length == 13 &&
          memcmp(net.last.passphrase, "correct horse", 13) == 0);
    CHECK(net.last.ssid_length == 11 && memcmp(net.last.ssid, "HomeNetwork", 11) == 0 && net.last.bssid[5] == 5);
    CHECK(prompt_now()[0] == '\0');
    as(0);
    CHECK(first_row_is("Connecting to HomeNetwork..."));
    CHECK(panel_list()->busy && strcmp(panel_list()->status, "connecting") == 0);
    // Nothing typed stays in the desktop
    char left[WM_TRAY_TEXT_MAX];
    as(PID_TRAY);
    CHECK(wm_tray_prompt_text(NETTRAY_ICON_ID, left, sizeof(left)) < 0 && errno == ENOENT);

    // Connected, then an address: the row says so, the heading has the IP
    net.link.state = WLAN_LINK_CONNECTED;
    run_for(600);
    as(0);
    home = row_with("HomeNetwork");
    CHECK(home >= 0 && (panel_list()->rows[home].flags & DWM_PANEL_ROW_ACTIVE) &&
          strcmp(panel_list()->rows[home].side, "connected") == 0);
    CHECK(strcmp(panel_list()->status, "connected") == 0 && !panel_list()->busy);
    CHECK(tip_is("Wi-Fi: connected to HomeNetwork"));
    net.link.ip = 0xC0A80117;
    run_for(600);
    as(0);
    CHECK(strcmp(panel_list()->status, "192.168.1.23") == 0);

    // No scan while connected; a click on it does nothing
    uint32_t scans = dev.scans;
    click_icon(INPUT_BTN_LEFT);         // Closes
    click_icon(INPUT_BTN_LEFT);         // Opens: no scan
    run_for(100);
    CHECK(dev.scans == scans && dwm_start_is_tray());
    click_row(row_with("HomeNetwork"));
    CHECK(net.connects == 1 && prompt_now()[0] == '\0');

    // Disconnect from the menu
    key(KEY_ESCAPE);
    click_icon(INPUT_BTN_RIGHT);
    run_for(50);
    as(0);
    CHECK(dwm_menu_is_open());
    dwm_menu_close();
    as(PID_TRAY);
    nettray_handle(&(wm_event_t){ .type = WM_EVENT_TRAY_COMMAND, .tray = NETTRAY_ICON_ID, .param = 3 });
    run_for(50);
    CHECK(net.disconnects == 1);
    as(0);
    CHECK(row_with("HomeNetwork") >= 0 && !(panel_list()->rows[row_with("HomeNetwork")].flags & DWM_PANEL_ROW_ACTIVE));

    // A wrong password: the kernel's reason, said
    click_icon(INPUT_BTN_LEFT);
    run_scan();
    click_row(row_with("HomeNetwork"));
    type("wrong password");
    key(KEY_ENTER);
    run_for(50);
    CHECK(net.connects == 2);
    net.link.state  = WLAN_LINK_IDLE;
    net.link.reason = WLAN_REASON_PASSWORD;
    run_for(600);
    CHECK(first_row_is("Wrong password for HomeNetwork"));

    // Esc in the box: nothing sent, the panel stays
    click_row(row_with("HomeNetwork"));
    as(0);
    CHECK(prompt_now()[0] != '\0');
    type("secret12");
    key(KEY_ESCAPE);
    run_for(50);
    as(0);
    CHECK(prompt_now()[0] == '\0' && dwm_start_is_tray() && net.connects == 2);
    // ... and closing the panel with the box open cancels it too
    click_row(row_with("HomeNetwork"));
    key(KEY_ESCAPE);                    // Cancels the box
    click_row(row_with("HomeNetwork"));
    click_at(40, 40, INPUT_BTN_LEFT);   // Closes the panel
    run_for(50);
    as(0);
    CHECK(!dwm_start_is_open() && prompt_now()[0] == '\0');

    // A hidden network: its name, then its password
    click_icon(INPUT_BTN_LEFT);
    run_scan();
    click_row(row_with("(hidden)"));
    as(0);
    CHECK(strcmp(prompt_now(), "Name of the hidden network:") == 0 && !panel_list()->secret);
    type("Attic");
    key(KEY_ENTER);
    run_for(50);
    as(0);
    CHECK(strcmp(prompt_now(), "Password for Attic:") == 0 && panel_list()->secret);
    type("up the ladder");
    key(KEY_ENTER);
    run_for(50);
    CHECK(net.connects == 3 && net.last.ssid_length == 5 && memcmp(net.last.ssid, "Attic", 5) == 0 &&
          net.last.bssid[5] == 3);

    // An open network: joined straight away, no password
    net.link.state = WLAN_LINK_IDLE;
    net.link.reason = WLAN_REASON_NOT_FOUND;
    run_for(600);
    CHECK(first_row_is("Attic didn't answer"));
    int cafe = -1;
    const dwm_tray_panel_t* l = panel_list();
    for (size_t i = 0; l != NULL && i < l->count; i++)
        if (strncmp(l->rows[i].text, "Cafe", 4) == 0) cafe = (int)i;
    CHECK(cafe >= 0);
    click_row(cafe);
    CHECK(net.connects == 4 && net.last.passphrase_length == 0 && prompt_now()[0] == '\0');

    // WPA3 only: refused before asking
    net.link.state = WLAN_LINK_IDLE;
    run_for(600);
    int n5 = row_with("Neighbor 5G");
    CHECK(n5 >= 0);
    click_row(n5);
    CHECK(net.connects == 4);       // Mixed WPA2 / WPA3: asked for a password
    CHECK(strcmp(prompt_now(), "Password for Neighbor 5G:") == 0);
    key(KEY_ESCAPE);

    // A driver that can't connect: said so
    net.can = false;
    click_row(row_with("HomeNetwork"));
    type("correct horse");
    key(KEY_ENTER);
    run_for(50);
    CHECK(first_row_is("RTL8192EU can't connect yet"));
    key(KEY_ESCAPE);
    run_for(50);
}

// A press and release in the middle of a panel button
static void click_button(uint32_t which) {
    as(0);
    gfx_rect_t r = dwm_start_tray_button(which);
    click_at(r.x + r.w / 2, r.y + r.h / 2, INPUT_BTN_LEFT);
    run_for(50);
}

// The panel's buttons: refresh in the heading, Connect under the password
// box, Disconnect along the bottom while connected
static void test_buttons(void) {
    as(PID_TRAY);
    nettray_stop();
    run_for(50);
    extra = 0;
    net.can = true;
    net.error = 0;
    memset(&net.link, 0, sizeof(net.link));
    panel_on = true;
    as(PID_TRAY);
    CHECK(nettray_start() == 0);
    run_scan();
    panel_on = false;

    // The right-click menu is the desktop's own, as for a taskbar button:
    // it is open as soon as the button comes up, before the service has
    // even heard of the click
    as(0);
    gfx_rect_t ic = tray_icon_rect(0);
    mouse(ic.x + ic.w / 2, ic.y + ic.h / 2, 0);
    mouse(ic.x + ic.w / 2, ic.y + ic.h / 2, INPUT_BTN_RIGHT);
    // The icon's picture and tooltip change while the button is down (as
    // they do every scan poll on real hardware): still one click
    as(PID_TRAY);
    CHECK(wm_tray_set_tip(NETTRAY_ICON_ID, "Wi-Fi: scanning") == 0);
    dwm_round();
    mouse(ic.x + ic.w / 2, ic.y + ic.h / 2, 0);
    as(0);
    CHECK(dwm_menu_is_open());
    // Its first item is still Scan now, and choosing it scans
    uint32_t before = dev.scans;
    as(0);
    menu_key(KEY_DOWN);
    menu_key(KEY_ENTER);
    run_for(100);
    CHECK(dev.scans == before + 1 && !dwm_menu_is_open());
    run_scan();
    os_sleep_ms(500);

    // A right click held down a while (the desktop runs its rounds, and
    // the taskbar its tick, before the button comes up): one click still
    as(0);
    mouse(ic.x + ic.w / 2, ic.y + ic.h / 2, INPUT_BTN_RIGHT);
    for (int i = 0; i < 5; i++)
        dwm_round();
    run_for(150);
    mouse(ic.x + ic.w / 2, ic.y + ic.h / 2, 0);
    as(0);
    CHECK(dwm_menu_is_open());
    dwm_menu_close();
    run_for(50);
    os_sleep_ms(500);

    // One right click with the panel open: the panel closes and the menu
    // opens, in the same click
    click_icon(INPUT_BTN_LEFT);
    run_scan();
    as(0);
    CHECK(dwm_start_is_tray());
    click_icon(INPUT_BTN_RIGHT);
    as(0);
    CHECK(!dwm_start_is_open() && dwm_menu_is_open());
    // ... and one more with the menu open: it opens again, not just closes
    click_icon(INPUT_BTN_RIGHT);
    as(0);
    CHECK(dwm_menu_is_open());
    dwm_menu_close();
    run_for(50);

    click_icon(INPUT_BTN_LEFT);
    run_scan();
    as(0);
    CHECK(dwm_start_is_tray());

    // Refresh: there, left of the title; no Disconnect while not connected
    CHECK(panel_list()->refresh && !panel_list()->disconnect);
    gfx_rect_t p = dwm_start_rect(), r = dwm_start_tray_button(DWM_PANEL_REFRESH);
    CHECK(r.w > 0 && r.x > p.x && r.x < p.x + 20 && r.y > p.y && r.y + r.h < p.y + dwm_text_height() + 12);
    CHECK(dwm_start_tray_button(DWM_PANEL_DISCONNECT).w == 0 && dwm_start_tray_button(DWM_PANEL_ACCEPT).w == 0);
    uint32_t scans = dev.scans;
    click_button(DWM_PANEL_REFRESH);
    CHECK(dev.scans == scans + 1 && dwm_start_is_tray());
    run_scan();
    // F5 does the same
    key(KEY_F5);
    run_for(50);
    CHECK(dev.scans == scans + 2);
    run_scan();
    // Down on it, up somewhere else: nothing
    as(0);
    r = dwm_start_tray_button(DWM_PANEL_REFRESH);
    mouse(r.x + 2, r.y + 2, 0);
    mouse(r.x + 2, r.y + 2, INPUT_BTN_LEFT);
    mouse(r.x + 60, r.y + 60, INPUT_BTN_LEFT);
    mouse(r.x + 60, r.y + 60, 0);
    run_for(100);
    CHECK(dev.scans == scans + 2 && dwm_start_is_tray());

    // The password box has a Connect button under it, which is Enter
    int connects = net.connects;
    click_row(row_with("HomeNetwork"));
    as(0);
    CHECK(strcmp(prompt_now(), "Password for HomeNetwork:") == 0 && panel_list()->connect);
    gfx_rect_t c = dwm_start_tray_button(DWM_PANEL_ACCEPT);
    p = dwm_start_rect();
    CHECK(c.w > 0 && c.x + c.w < p.x + p.w && c.y + c.h < p.y + p.h && c.y > p.y + p.h / 2);
    type("correct horse");
    click_button(DWM_PANEL_ACCEPT);
    CHECK(net.connects == connects + 1 && net.last.passphrase_length == 13 &&
          memcmp(net.last.passphrase, "correct horse", 13) == 0);
    as(0);
    CHECK(prompt_now()[0] == '\0' && dwm_start_tray_button(DWM_PANEL_ACCEPT).w == 0);
    // Joining: not yet a Disconnect button
    CHECK(!panel_list()->disconnect);

    // Connected: Disconnect shows, and takes us off
    net.link.state = WLAN_LINK_CONNECTED;
    run_for(600);
    as(0);
    CHECK(panel_list()->disconnect && dwm_start_tray_button(DWM_PANEL_DISCONNECT).w > 0);
    int disconnects = net.disconnects;
    scans = dev.scans;
    click_button(DWM_PANEL_REFRESH);        // Connected: no scan, the radio stays on the channel
    CHECK(dev.scans == scans);
    click_button(DWM_PANEL_DISCONNECT);
    CHECK(net.disconnects == disconnects + 1);
    run_for(600);
    as(0);
    CHECK(!panel_list()->disconnect && dwm_start_tray_button(DWM_PANEL_DISCONNECT).w == 0);
    CHECK(dwm_start_is_tray());
    key(KEY_ESCAPE);
    run_for(50);
}

// Disable / Enable from the icon's menu: off, grey, nothing scanned; on
// again with a fresh scan
static void test_radio(void) {
    as(PID_TRAY);
    // Connected first, so turning it off disconnects
    net.can = true;
    click_icon(INPUT_BTN_LEFT);
    run_scan();
    click_row(row_with("HomeNetwork"));
    type("correct horse");
    key(KEY_ENTER);
    run_for(50);
    net.link.state = WLAN_LINK_CONNECTED;
    run_for(600);
    CHECK(tip_is("Wi-Fi: connected to HomeNetwork"));
    key(KEY_ESCAPE);
    run_for(50);

    // The menu has Enable (greyed, ticked: on now) and Disable
    click_icon(INPUT_BTN_RIGHT);
    run_for(50);
    as(0);
    CHECK(dwm_menu_is_open());
    dwm_menu_close();
    int disconnects = net.disconnects;
    as(PID_TRAY);
    nettray_handle(&(wm_event_t){ .type = WM_EVENT_TRAY_COMMAND, .tray = NETTRAY_ICON_ID, .param = 5 });   // Disable
    run_for(50);
    CHECK(net.disconnects == disconnects + 1 && !dev.fd_open);
    CHECK(tip_is("Wi-Fi: off (right-click to enable)"));

    // Off: a click opens the panel saying so, scans nothing, no buttons
    uint32_t scans = dev.scans, opens = dev.opens;
    click_icon(INPUT_BTN_LEFT);
    run_for(300);
    as(0);
    CHECK(dwm_start_is_tray() && dev.scans == scans && dev.opens == opens);
    CHECK(first_row_is("Wi-Fi is off. Right-click the icon to enable") && panel_list()->count == 1);
    CHECK(strcmp(panel_list()->status, "off") == 0 && !panel_list()->refresh && !panel_list()->disconnect);
    key(KEY_F5);
    run_for(100);
    CHECK(dev.scans == scans);
    key(KEY_ESCAPE);
    run_for(50);
    // Scan now does nothing either
    as(PID_TRAY);
    nettray_handle(&(wm_event_t){ .type = WM_EVENT_TRAY_COMMAND, .tray = NETTRAY_ICON_ID, .param = 1 });
    run_for(100);
    CHECK(dev.scans == scans && !dev.fd_open);

    // Enable: the adapter again, and a scan
    nettray_handle(&(wm_event_t){ .type = WM_EVENT_TRAY_COMMAND, .tray = NETTRAY_ICON_ID, .param = 4 });   // Enable
    run_for(100);
    CHECK(dev.fd_open && dev.scans == scans + 1);
    net.link.state = WLAN_LINK_IDLE;
    run_scan();
    CHECK(tip_is("Wi-Fi: 9 networks nearby"));
}

// No network bit and no way to ask: nothing is opened, the list says why
static void test_no_permission(void) {
    as(PID_TRAY);
    nettray_stop();
    run_for(50);
    CHECK(tray_count() == 0 && !dev.fd_open);

    as(PID_TRAY);
    dropped = 0;
    CHECK(sec_set(sec_get() & ~(SEC_NET | SEC_ELEVATE | SEC_ADMIN)) == 0);     // The last change: no admin after
    CHECK(dropped == 0);
    uint32_t opens = dev.opens;
    CHECK(nettray_start() == 0);
    run_for(50);
    CHECK(tray_count() == 1 && tip_is("Wi-Fi: no network permission"));
    click_icon(INPUT_BTN_LEFT);
    run_for(100);
    CHECK(popup_open() && dev.opens == opens);
}

//
// The preview: the bottom-right of the screen, twice the size
//

static void write_preview(const char* path) {
    int32_t sw = compositor_width(), sh = compositor_height();
    int32_t x0 = 0, y0 = 0, w = sw, h = sh;
    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return;
    fprintf(f, "P6\n%d %d\n255\n", (int)w, (int)h);
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            uint32_t p = __test_gfx_screen_pixel(x0 + x, y0 + y);
            unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

int main(int argc, char** argv) {
    // Another screen size: nettray_test - - 1920x1080 (the preview needs 640 x 480)
    int32_t sw = 640, sh = 480;
    if (argc > 3) {
        sw = (int32_t)strtol(argv[3], NULL, 10);
        const char* x = strchr(argv[3], 'x');
        sh = x ? (int32_t)strtol(x + 1, NULL, 10) : 0;
    }
    if (!compositor_init(sw, sh)) {
        printf("compositor_init failed\n");
        return 1;
    }
    CHECK(server_init());
    as(0);
    __test_ipc_set_sleep_hook(hook);
    as(PID_TRAY);
    CHECK(wm_connect(1000) == 0);

    // A TrueType font for the service, as the desktop hands it one
    if (argc > 2 && strcmp(argv[2], "-") != 0) {
        as(PID_TRAY);
        font_t* font = font_open_file(argv[2]);
        CHECK(font != NULL && gfx_set_font(font, 16) == 0);
    }
    test_netlist();
    test_start_and_open();
    test_menu();
    test_problems();

    preview_path = (argc > 1 && strcmp(argv[1], "-") != 0) ? argv[1] : NULL;
    test_many();
    test_panel();
    test_connect();
    test_buttons();
    test_radio();
    test_no_permission();

    printf("nettray: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
