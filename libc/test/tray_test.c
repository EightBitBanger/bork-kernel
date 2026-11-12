/*
 * System tray test: programs putting icons on the DWM's taskbar (tray.c,
 * server.c) over the kernel's own ipc/ipc.c (ipc_shim.c), the way wm_test.c
 * runs the window server.
 *
 * PID_CLIENT uses os/wm.h (wm_tray_*); the other "processes" speak the
 * protocol by hand, so they can send what os/wm.h never would: bad sizes,
 * pictures smaller than they claim, handles that aren't shared memory,
 * requests for other programs' icons. Clicks are made on the taskbar the
 * way input.c makes them (taskbar_press / taskbar_release).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/ipc.h>
#include <os/wm.h>
#include <os/wm_proto.h>

#include "dwm/dwm.h"
#include "dwm/compositor.h"
#include "dwm/internal.h"
#include "dwm/server.h"
#include "dwm/timer.h"

int      malloc_check(void);
void     __test_ipc_set_pid(uint32_t pid);
void     __test_ipc_set_sleep_hook(void (*fn)(void));
void     __test_ipc_exit(uint32_t pid);
uint32_t __test_ipc_pages_live(void);
uint32_t __test_ipc_maps_live(void);

#define PID_CLIENT  201
#define PID_OTHER   202
#define PID_CRASH   203
#define PID_FILL    300         // And up: processes filling the tray

static int failures = 0, checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); } \
} while (0)

//
// Graphics stand-in
//

typedef struct { int32_t w, h; uint32_t* px; } fsurf_t;
static fsurf_t fs[SYS_GFX_SURFACE_MAX + 1];
static uint32_t pixels_cmds;            // PIXELS commands run
static uint32_t pixels_first;           // First pixel of the last one

static fsurf_t* fget(uint32_t h) { return (h >= 1 && h <= SYS_GFX_SURFACE_MAX && fs[h].px) ? &fs[h] : NULL; }

static void fill(fsurf_t* s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
    for (int32_t j = y; j < y + h; j++)
        for (int32_t i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < s->w && j < s->h)
                s->px[j * s->w + i] = c;
}

int32_t __test_gfx_syscall(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    switch (n) {
        case SYS_GFX_INFO: {
            SysGfxInfo info = { 640, 480, 32, 6, 8, 4096, 16, 32u << 20 };
            memcpy((void*)(uintptr_t)a1, &info, sizeof(info));
            return 0;
        }
        case SYS_GFX_CREATE:
            for (uint32_t h = 1; h <= SYS_GFX_SURFACE_MAX; h++) {
                if (fs[h].px) continue;
                fs[h].w = (int32_t)a1;
                fs[h].h = (int32_t)a2;
                fs[h].px = (uint32_t*)malloc(a1 * a2 * 4);
                fill(&fs[h], 0, 0, fs[h].w, fs[h].h, GFX_COLOR_BLACK);
                return (int32_t)h;
            }
            return SYS_EMFILE;
        case SYS_GFX_DESTROY: {
            fsurf_t* s = fget(a1);
            if (!s) return SYS_EBADF;
            free(s->px);
            memset(s, 0, sizeof(*s));
            return 0;
        }
        case SYS_GFX_SUBMIT: {
            fsurf_t* s = fget(a1);
            if (!s) return SYS_EBADF;
            const uint32_t* c = (const uint32_t*)(uintptr_t)a2;
            uint32_t words = a3 / 4, pos = 0, ran = 0;
            while (pos < words) {
                uint32_t op = c[pos] & 0xFFFF, len = c[pos] >> 16;
                const int32_t* a = (const int32_t*)&c[pos + 1];
                if (len == 0) return SYS_EINVAL;
                if (op == SYS_GFX_CMD_CLEAR) fill(s, 0, 0, s->w, s->h, (uint32_t)a[0]);
                if (op == SYS_GFX_CMD_FILL_RECT) fill(s, a[0], a[1], a[2], a[3], (uint32_t)a[4]);
                if (op == SYS_GFX_CMD_BLIT && (a[7] & SYS_GFX_F_WHOLE)) {
                    fsurf_t* src = fget((uint32_t)a[0]);
                    for (int32_t j = 0; src && j < src->h; j++)
                        for (int32_t i = 0; i < src->w; i++)
                            fill(s, a[5] + i, a[6] + j, 1, 1, src->px[j * src->w + i]);
                }
                if (op == SYS_GFX_CMD_PIXELS) {
                    pixels_cmds++;
                    pixels_first = *(const uint32_t*)(uintptr_t)(uint32_t)a[0];
                }
                pos += len;
                ran++;
            }
            return (int32_t)ran;
        }
        case SYS_GFX_READ:
        case SYS_GFX_WRITE: {
            fsurf_t* s = fget(a1);
            if (!s) return SYS_EBADF;
            const SysGfxRect* r = (const SysGfxRect*)(uintptr_t)a2;
            uint32_t* user = (uint32_t*)(uintptr_t)a3;
            int32_t stride = a4 ? (int32_t)a4 : r->w;
            if (r->x < 0 || r->y < 0 || r->x + r->w > s->w || r->y + r->h > s->h) return SYS_EINVAL;
            for (int32_t j = 0; j < r->h; j++)
                for (int32_t i = 0; i < r->w; i++) {
                    uint32_t* sp = &s->px[(r->y + j) * s->w + r->x + i];
                    uint32_t* up = &user[j * stride + i];
                    if (n == SYS_GFX_READ) *up = *sp; else *sp = *up;
                }
            return 0;
        }
        case SYS_GFX_PRESENT:
            return fget(a1) ? 0 : SYS_EBADF;
    }
    return SYS_ENOSYS;
}

//
// Pieces of dwm.c this test stands in for
//

int32_t dwm_screen_width(void)  { return compositor_width(); }
int32_t dwm_screen_height(void) { return compositor_height(); }
bool    dwm_warp_pointer(int32_t x, int32_t y) { (void)x; (void)y; return false; }

//
// The DWM's main loop, one round at a time
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

// Next event of `type` for the client (others skipped), within a few rounds
static bool client_wait_for(uint32_t type, wm_event_t* out) {
    for (int tries = 0; tries < 20; tries++) {
        as(PID_CLIENT);
        wm_event_t ev;
        int r = wm_wait_event(&ev, 0);
        if (r < 0) return false;
        if (r == 0) { dwm_round(); continue; }
        if (ev.type == type) {
            if (out) *out = ev;
            return true;
        }
    }
    return false;
}

// A click on tray icon `index` (press and release in the middle of it)
static void click(int index, uint8_t button) {
    dwm_round();                        // Laid out
    as(0);
    gfx_rect_t r = tray_icon_rect(index);
    int32_t x = r.x + r.w / 2, y = r.y + r.h / 2;
    taskbar_press(x, y, button);
    taskbar_release(x, y, button);
}

// A 16 x 16 picture of one color
static uint32_t pic[WM_TRAY_ICON_MAX * WM_TRAY_ICON_MAX];
static const uint32_t* solid(uint32_t c) {
    for (size_t i = 0; i < sizeof(pic) / sizeof(pic[0]); i++)
        pic[i] = c;
    return pic;
}

//
// Programs speaking the protocol by hand
//

static uint32_t raw_seq = 100;

static int raw_connect(uint32_t pid) {
    as(pid);
    int pipe = ipc_connect(WM_PORT_NAME);
    dwm_round();
    as(pid);
    wm_message_t m;
    if (pipe <= 0 || ipc_recv(pipe, &m, sizeof(m), NULL) != sizeof(m) || m.kind != WM_MSG_WELCOME)
        return -1;
    return pipe;
}

// Send `size` bytes (and a handle) as `pid`, return the reply's status
static int raw_send(uint32_t pid, int pipe, void* q, size_t size, int attach) {
    uint32_t seq = ++raw_seq;
    ((uint32_t*)q)[1] = seq;            // Every request has seq second
    as(pid);
    if (ipc_send(pipe, q, size, attach) < 0)
        return -1000;
    for (int i = 0; i < 10; i++) {
        dwm_round();
        as(pid);
        wm_message_t m;
        while (ipc_recv(pipe, &m, sizeof(m), NULL) == sizeof(m)) {
            if (m.kind == WM_MSG_REPLY && m.seq == seq)
                return m.status;
        }
    }
    return -1001;
}

// A shm block of `bytes` holding a solid picture, as `pid`
static int raw_picture(uint32_t pid, size_t bytes, uint32_t color) {
    as(pid);
    int shm = shm_create(bytes);
    uint32_t* p = (shm > 0) ? (uint32_t*)shm_map(shm) : NULL;
    if (p == NULL)
        return -1;
    for (size_t i = 0; i < bytes / 4; i++)
        p[i] = color;
    return shm;
}

static int raw_tray(uint32_t pid, int pipe, uint32_t type, uint32_t id, uint32_t flags,
                    int32_t w, int32_t h, int attach, const char* tip) {
    wm_tray_request_t q;
    memset(&q, 0, sizeof(q));
    q.type   = type;
    q.id     = id;
    q.flags  = flags;
    q.icon_w = w;
    q.icon_h = h;
    if (tip != NULL)
        memcpy(q.tip, tip, strlen(tip) < sizeof(q.tip) ? strlen(tip) : sizeof(q.tip));    // Maybe unterminated
    int r = raw_send(pid, pipe, &q, sizeof(q), attach);
    as(pid);
    if (attach > 0)
        ipc_close(attach);
    return r;
}

static int raw_add(uint32_t pid, int pipe, uint32_t id) {
    int shm = raw_picture(pid, 16 * 16 * 4, 0xFF00FF00u);
    return raw_tray(pid, pipe, WM_REQ_TRAY_ADD, id, WM_TRAY_ICON, 16, 16, shm, "raw");
}

//
// Tests
//

static void test_client(void) {
    as(PID_CLIENT);
    uint32_t serial = tray_serial();
    CHECK(wm_tray_add(1, solid(0xFFFF0000u), 16, 16, "Red") == 0);
    as(0);
    CHECK(tray_count() == 1 && tray_serial() != serial);
    wm_menu_item_t early = { "Early", 1, 0 };
    as(PID_CLIENT);
    CHECK(wm_tray_menu(1, &early, 1) == -1 && errno == EPERM);    // No click yet

    // Refused by the library before anything is sent
    as(PID_CLIENT);
    CHECK(wm_tray_add(2, NULL, 16, 16, "x") == -1 && errno == EINVAL);
    CHECK(wm_tray_add(2, pic, 0, 16, "x") == -1 && errno == EINVAL);
    CHECK(wm_tray_add(2, pic, WM_TRAY_ICON_MAX + 1, 16, "x") == -1 && errno == EINVAL);

    // Refused by the DWM
    CHECK(wm_tray_add(1, pic, 16, 16, "again") == -1 && errno == EEXIST);
    CHECK(wm_tray_set_tip(9, "nope") == -1 && errno == ENOENT);
    CHECK(wm_tray_remove(9) == -1 && errno == ENOENT);

    // Changes, and pictures of other sizes (fit into 16 x 16)
    CHECK(wm_tray_set_icon(1, solid(0xFF0000FFu), 64, 64) == 0);
    CHECK(wm_tray_set_icon(1, solid(0xFF0000FFu), 3, 40) == 0);
    CHECK(wm_tray_set_tip(1, "Blue\nwith a newline") == 0);
    CHECK(wm_tray_set_tip(1, NULL) == 0);

    // Per process: WM_TRAY_PER_PROCESS
    for (uint32_t id = 2; id <= WM_TRAY_PER_PROCESS; id++)
        CHECK(wm_tray_add(id, solid(0xFF808080u), 16, 16, NULL) == 0);
    CHECK(wm_tray_add(99, pic, 16, 16, NULL) == -1 && errno == EMFILE);
    as(0);
    CHECK(tray_count() == WM_TRAY_PER_PROCESS);
    as(PID_CLIENT);
    for (uint32_t id = 2; id <= WM_TRAY_PER_PROCESS; id++)
        CHECK(wm_tray_remove(id) == 0);
    as(0);
    CHECK(tray_count() == 1);

    // Clicks: left, a quick second (a double click), right, middle
    click(0, INPUT_BTN_LEFT);
    wm_event_t ev;
    CHECK(client_wait_for(WM_EVENT_TRAY, &ev) && ev.window == 0 && ev.tray == 1 &&
          ev.button == INPUT_BTN_LEFT && ev.clicks == 1 && ev.rect_w > 0);
    click(0, INPUT_BTN_LEFT);
    CHECK(client_wait_for(WM_EVENT_TRAY, &ev) && ev.clicks == 2);
    click(0, INPUT_BTN_MIDDLE);
    CHECK(client_wait_for(WM_EVENT_TRAY, &ev) && ev.button == INPUT_BTN_MIDDLE);

    // A press on the icon let go elsewhere is no click
    dwm_round();
    as(0);
    gfx_rect_t r = tray_icon_rect(0);
    taskbar_press(r.x + 2, r.y + 2, INPUT_BTN_LEFT);
    taskbar_release(5, compositor_height() - 3, INPUT_BTN_LEFT);
    CHECK(!client_wait_for(WM_EVENT_TRAY, NULL));

    // Menus: only right after a click, once per click
    wm_menu_item_t items[] = { { "First", 41, 0 }, { NULL, 0, WM_MENU_SEPARATOR }, { "Second", 42, 0 } };
    click(0, INPUT_BTN_RIGHT);
    CHECK(client_wait_for(WM_EVENT_TRAY, &ev) && ev.button == INPUT_BTN_RIGHT);
    as(PID_CLIENT);
    CHECK(wm_tray_menu(9, items, 3) == -1 && errno == ENOENT);
    CHECK(wm_tray_menu(1, items, 0) == -1 && errno == EINVAL);
    CHECK(wm_tray_menu(1, items, 3) == 0);
    as(0);
    CHECK(dwm_menu_is_open());
    as(PID_CLIENT);
    CHECK(wm_tray_menu(1, items, 3) == -1 && errno == EPERM);     // One per click
    as(0);
    menu_key(KEY_DOWN);
    menu_key(KEY_DOWN);                 // Over the separator to "Second"
    menu_key(KEY_ENTER);
    CHECK(!dwm_menu_is_open());
    CHECK(client_wait_for(WM_EVENT_TRAY_COMMAND, &ev) && ev.window == 0 && ev.tray == 1 && ev.param == 42);

    // ... and only for a while after it
    click(0, INPUT_BTN_RIGHT);
    CHECK(client_wait_for(WM_EVENT_TRAY, &ev));
    uint64_t until = dwm_time_ms() + WM_TRAY_MENU_MS + 100;
    while (dwm_time_ms() < until)
        ;                               // The DWM's clock (sleep() runs the fake one)
    as(PID_CLIENT);
    CHECK(wm_tray_menu(1, items, 3) == -1 && errno == EPERM);

    CHECK(wm_tray_remove(1) == 0);
    as(0);
    CHECK(tray_count() == 0);
}

// Bad requests from a program speaking the protocol by hand do no harm,
// and it can't touch another program's icons
static void test_hostile(void) {
    as(PID_CLIENT);
    CHECK(wm_tray_add(7, solid(0xFFFF0000u), 16, 16, "Mine") == 0);

    int pipe = raw_connect(PID_OTHER);
    CHECK(pipe > 0);

    // No picture, a picture smaller than it claims, sizes out of range,
    // a handle that isn't shared memory, unknown flags
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, WM_TRAY_ICON, 16, 16, 0, NULL) == EINVAL);
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, 0, 16, 16, raw_picture(PID_OTHER, 1024, 1), NULL) == EINVAL);
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, WM_TRAY_ICON, 64, 64,
                   raw_picture(PID_OTHER, 4096, 1), NULL) == EINVAL);           // Needs 16 KB
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, WM_TRAY_ICON, 65, 1,
                   raw_picture(PID_OTHER, 8192, 1), NULL) == EINVAL);
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, WM_TRAY_ICON, -4, -4,
                   raw_picture(PID_OTHER, 4096, 1), NULL) == EINVAL);
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, WM_TRAY_ICON, 0x40000000, 0x40000000,
                   raw_picture(PID_OTHER, 4096, 1), NULL) == EINVAL);
    as(PID_OTHER);
    int fds[2];
    CHECK(ipc_pipe(fds) == 0);
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, WM_TRAY_ICON, 16, 16, fds[0], NULL) == EINVAL);
    as(PID_OTHER);
    ipc_close(fds[1]);
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, WM_TRAY_ICON | 0x80, 16, 16,
                   raw_picture(PID_OTHER, 1024, 1), NULL) == EINVAL);
    as(0);
    CHECK(tray_count() == 1);

    // An unterminated tooltip is cut, not read past
    char long_tip[WM_TRAY_TIP_MAX + 8];
    memset(long_tip, 'A', sizeof(long_tip) - 1);
    long_tip[sizeof(long_tip) - 1] = '\0';
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_ADD, 1, WM_TRAY_ICON | WM_TRAY_TIP, 16, 16,
                   raw_picture(PID_OTHER, 1024, 1), long_tip) == 0);

    // Not its icon: the client's id 7 is unknown on this connection
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_REMOVE, 7, 0, 0, 0, 0, NULL) == ENOENT);
    CHECK(raw_tray(PID_OTHER, pipe, WM_REQ_TRAY_MODIFY, 7, WM_TRAY_TIP, 0, 0, 0, "mine now") == ENOENT);
    wm_menu_request_t mq;
    memset(&mq, 0, sizeof(mq));
    mq.type = WM_REQ_TRAY_MENU;
    mq.window = 7;
    mq.count = 1;
    strcpy(mq.items[0].label, "Steal");
    CHECK(raw_send(PID_OTHER, pipe, &mq, sizeof(mq), 0) == ENOENT);
    mq.window = 1;
    CHECK(raw_send(PID_OTHER, pipe, &mq, sizeof(mq), 0) == EPERM);     // Its own icon, but no click
    mq.count = WM_MENU_MAX_ITEMS + 1;
    CHECK(raw_send(PID_OTHER, pipe, &mq, sizeof(mq), 0) == EINVAL);

    // A second connection from the same process shares its quota
    CHECK(raw_add(PID_OTHER, pipe, 2) == 0 && raw_add(PID_OTHER, pipe, 3) == 0 && raw_add(PID_OTHER, pipe, 4) == 0);
    int pipe2 = raw_connect(PID_OTHER);
    CHECK(pipe2 > 0 && raw_add(PID_OTHER, pipe2, 1) == EMFILE);
    as(0);
    CHECK(tray_count() == 1 + WM_TRAY_PER_PROCESS);

    // The whole tray: WM_TRAY_MAX
    int fill[WM_TRAY_MAX];
    size_t pids = 0;
    for (; tray_count() < WM_TRAY_MAX; pids++) {
        fill[pids] = raw_connect(PID_FILL + pids);
        for (uint32_t id = 1; id <= WM_TRAY_PER_PROCESS && tray_count() < WM_TRAY_MAX; id++)
            CHECK(raw_add(PID_FILL + pids, fill[pids], id) == 0);
        as(0);
    }
    fill[pids] = raw_connect(PID_FILL + pids);
    CHECK(raw_add(PID_FILL + pids, fill[pids], 1) == ENOSPC);
    pids++;
    as(0);
    taskbar_tick();
    for (size_t i = 0; i < pids; i++) {
        as(PID_FILL + i);
        ipc_close(fill[i]);             // Hanging up takes its icons
    }
    dwm_round();
    as(0);
    CHECK(tray_count() == 1 + WM_TRAY_PER_PROCESS);

    // The other program crashes: the kernel closes its handles, its icons go
    __test_ipc_exit(PID_OTHER);
    dwm_round();
    as(0);
    CHECK(tray_count() == 1);

    // A program whose menu is open goes away: the menu goes too
    int crash = raw_connect(PID_CRASH);
    CHECK(crash > 0 && raw_add(PID_CRASH, crash, 5) == 0);
    click(1, INPUT_BTN_RIGHT);
    mq.window = 5;
    mq.count = 1;
    CHECK(raw_send(PID_CRASH, crash, &mq, sizeof(mq), 0) == 0);
    as(0);
    CHECK(dwm_menu_is_open() && tray_count() == 2);
    __test_ipc_exit(PID_CRASH);
    dwm_round();
    as(0);
    CHECK(!dwm_menu_is_open() && tray_count() == 1);

    // The client's icon is untouched, and goes when it disconnects
    as(PID_CLIENT);
    wm_disconnect();
    dwm_round();
    as(0);
    CHECK(tray_count() == 0);
}

int main(void) {
    if (!compositor_init(640, 480)) {
        printf("compositor_init failed\n");
        return 1;
    }
    CHECK(server_init());
    as(0);
    __test_ipc_set_sleep_hook(hook);
    as(PID_CLIENT);
    CHECK(wm_connect(1000) == 0);

    test_client();
    test_hostile();

    __test_ipc_set_sleep_hook(NULL);
    as(0);
    server_shutdown();
    window_destroy_all();
    compositor_shutdown();
    taskbar_shutdown();
    __test_ipc_exit(PID_CLIENT);
    CHECK(__test_ipc_pages_live() == 0 && __test_ipc_maps_live() == 0);
    CHECK(malloc_check() == 0);

    printf("tray: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
