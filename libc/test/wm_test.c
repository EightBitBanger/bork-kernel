/*
 * Window server test: a program (os/wm.h) asking the DWM (src/dwm, with its
 * window server) for windows, over the kernel's own ipc/ipc.c (ipc_shim.c).
 *
 * Two emulated processes in one: the DWM runs as this process, the client
 * as PID_CLIENT. Whenever the client would sleep waiting for the DWM, the
 * shim's sleep hook runs one round of the DWM's main loop instead.
 *
 * Graphics are a small stand-in (below): surfaces are plain pixel arrays
 * and only the commands this test needs are carried out. PIXELS commands
 * (how the DWM paints a client window) are recorded, so the test can see
 * the DWM paint from the shared buffer.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/gfx.h>
#include <os/ipc.h>
#include <os/wm.h>
#include <os/wm_proto.h>

#include "dwm/dwm.h"
#include "dwm/compositor.h"
#include "dwm/internal.h"
#include "dwm/server.h"

int      malloc_check(void);
void     __test_ipc_set_pid(uint32_t pid);
void     __test_ipc_set_sleep_hook(void (*fn)(void));
void     __test_ipc_exit(uint32_t pid);
uint32_t __test_ipc_pages_live(void);
uint32_t __test_ipc_maps_live(void);

#define PID_CLIENT  201
#define PID_OTHER   202

static int failures = 0, checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); } \
} while (0)

#define RED   0xFFFF0000u
#define BLUE  0xFF0000FFu

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
// The DWM's main loop, one round at a time
//

static void as(uint32_t pid) { __test_ipc_set_pid(pid); }

static void dwm_round(void) {
    as(0);
    server_poll();
    event_dispatch();
    server_flush();
    compositor_render();
}

// Runs whenever the client would sleep; one round per sleep
static void hook(void) {
    dwm_round();
    __test_ipc_set_sleep_hook(hook);
}

// Next event of `type` for the client (others skipped), within a few rounds
static bool client_wait_for(uint32_t type, wm_event_t* out, uint32_t* seen_mask) {
    for (int tries = 0; tries < 20; tries++) {
        as(PID_CLIENT);
        wm_event_t ev;
        int r = wm_wait_event(&ev, 0);
        if (r < 0) return false;
        if (r == 0) { dwm_round(); continue; }
        if (seen_mask) *seen_mask |= 1u << ev.type;
        if (ev.type == type) {
            if (out) *out = ev;
            return true;
        }
    }
    return false;
}

static void test_basics(void) {
    as(PID_CLIENT);
    CHECK(!wm_connected());
    CHECK(wm_connect(1000) == 0 && wm_connected());
    int32_t sw, sh;
    wm_screen_size(&sw, &sh);
    CHECK(sw == 640 && sh == 480);

    wm_window_desc_t d = { .title = "Client", .x = 50, .y = 60, .width = 100, .height = 60,
                           .min_width = 40, .min_height = 30, .style = WM_STYLE_RESIZABLE };
    wm_window_t win = wm_window_create(&d);
    CHECK(win != 0);
    gfx_surface_t* s = wm_window_surface(win);
    CHECK(s != NULL && gfx_surface_width(s) == 100 && gfx_surface_height(s) == 60);

    as(0);
    CHECK(dwm_window_exists(win) && !dwm_window_visible(win));     // Hidden until the first commit
    CHECK(strcmp(dwm_window_get_title(win), "Client") == 0);
    CHECK(server_client_count() == 1);

    // Draw, commit: the DWM's copy of the buffer matches, it shows, it paints
    as(PID_CLIENT);
    gfx_clear(s, RED);
    gfx_fill_rect(s, 0, 0, 1, 1, BLUE);
    CHECK(wm_window_commit(win, NULL) == 0);
    uint32_t painted = pixels_cmds;
    dwm_round();
    int32_t bw, bh;
    const uint32_t* px = server_window_pixels(win, &bw, &bh);
    CHECK(px != NULL && bw == 100 && bh == 60 && px[0] == BLUE && px[1] == RED && px[59 * 100 + 99] == RED);
    CHECK(dwm_window_visible(win) && dwm_window_get_focus() == win);
    CHECK(pixels_cmds > painted && pixels_first == BLUE);

    // A partial commit: only that part is copied
    as(PID_CLIENT);
    gfx_clear(s, BLUE);
    gfx_rect_t part = gfx_rect(10, 10, 5, 5);
    CHECK(wm_window_commit(win, &part) == 0);
    dwm_round();
    px = server_window_pixels(win, NULL, NULL);
    CHECK(px[10 * 100 + 10] == BLUE && px[14 * 100 + 14] == BLUE && px[15 * 100 + 15] == RED);

    // Events: SHOW and FOCUS came first, then a close request
    as(0);
    dwm_window_close(win);
    dwm_round();
    uint32_t seen = 0;
    wm_event_t ev;
    CHECK(client_wait_for(WM_EVENT_CLOSE, &ev, &seen) && ev.window == win);
    CHECK((seen & (1u << WM_EVENT_SHOW)) && (seen & (1u << WM_EVENT_FOCUS_GAINED)));
    as(0);
    CHECK(dwm_window_exists(win));      // CLOSE alone destroys nothing

    // Keys reach the focused client window
    dwm_event_t key;
    memset(&key, 0, sizeof(key));
    key.type = DWM_EVENT_KEY;
    key.key = 'q';
    dwm_post(dwm_window_get_focus(), &key);
    dwm_round();
    CHECK(client_wait_for(WM_EVENT_KEY, &ev, NULL) && ev.key == 'q');

    // Resizes in a row arrive as one, with a new buffer of that size
    as(0);
    dwm_window_resize(win, 150, 90);
    dwm_window_resize(win, 180, 100);
    dwm_round();
    seen = 0;
    CHECK(client_wait_for(WM_EVENT_RESIZED, &ev, NULL) && ev.rect_w == 180 && ev.rect_h == 100);
    CHECK(!client_wait_for(WM_EVENT_RESIZED, NULL, NULL));
    as(PID_CLIENT);
    s = wm_window_surface(win);
    int32_t w, h;
    wm_window_get_size(win, &w, &h);
    CHECK(gfx_surface_width(s) == 180 && gfx_surface_height(s) == 100 && w == 180 && h == 100);
    CHECK(gfx_get_pixel(s, 10, 10) == BLUE && gfx_get_pixel(s, 0, 0) == BLUE &&
          gfx_get_pixel(s, 170, 90) == GFX_COLOR_BLACK);           // Old picture copied, rest new
    dwm_round();
    px = server_window_pixels(win, &bw, &bh);
    CHECK(px != NULL && bw == 180 && bh == 100);
    CHECK(__test_ipc_pages_live() == (180 * 100 * 4 + 4095) / 4096);    // The old buffer is gone

    // Requests the client makes itself
    as(PID_CLIENT);
    CHECK(wm_window_set_title(win, "Renamed") == 0);
    CHECK(wm_window_move(win, 200, 100) == 0);
    CHECK(wm_window_resize(win, 0, 5) == -1 && errno == EINVAL);
    CHECK(wm_window_show(win, false) == 0);
    dwm_round();
    gfx_rect_t frame;
    CHECK(strcmp(dwm_window_get_title(win), "Renamed") == 0);
    CHECK(dwm_window_get_rect(win, &frame, NULL) && frame.x == 200 && frame.y == 100);
    CHECK(!dwm_window_visible(win));
    as(PID_CLIENT);
    CHECK(wm_window_show(win, true) == 0 && wm_window_focus(win) == 0);
    dwm_round();
    CHECK(dwm_window_visible(win) && dwm_window_get_focus() == win);

    // Bad window ids are refused locally
    as(PID_CLIENT);
    CHECK(wm_window_commit(12345, NULL) == -1 && errno == EBADF);
    CHECK(wm_window_surface(12345) == NULL);

    // Destroy: the DWM's window and its buffer go
    CHECK(wm_window_destroy(win) == 0);
    CHECK(wm_window_surface(win) == NULL);
    dwm_round();
    CHECK(!dwm_window_exists(win) && __test_ipc_pages_live() == 0 && __test_ipc_maps_live() == 0);
}

// A second program, speaking the protocol by hand: it must not reach the
// first program's windows, and bad requests must do no harm
static int raw_pipe;
static uint32_t raw_seq = 100;

static int raw_call(wm_request_t* q, int attach, wm_message_t* out) {
    q->seq = ++raw_seq;
    as(PID_OTHER);
    if (ipc_send(raw_pipe, q, sizeof(*q), attach) < 0) return -1;
    for (int i = 0; i < 10; i++) {
        dwm_round();
        as(PID_OTHER);
        wm_message_t m;
        while (ipc_recv(raw_pipe, &m, sizeof(m), NULL) == sizeof(m)) {
            if (m.kind == WM_MSG_REPLY && m.seq == q->seq) {
                if (out) *out = m;
                return m.status;
            }
        }
    }
    return -1;
}

static void test_isolation(void) {
    as(PID_CLIENT);
    wm_window_desc_t d = { .title = "Mine", .x = WM_POS_DEFAULT, .y = WM_POS_DEFAULT, .width = 64, .height = 64 };
    wm_window_t mine = wm_window_create(&d);
    CHECK(mine != 0);

    as(PID_OTHER);
    raw_pipe = ipc_connect(WM_PORT_NAME);
    CHECK(raw_pipe > 0);
    dwm_round();
    as(PID_OTHER);
    wm_message_t m;
    CHECK(ipc_recv(raw_pipe, &m, sizeof(m), NULL) == sizeof(m) && m.kind == WM_MSG_WELCOME &&
          m.version == WM_PROTOCOL_VERSION);

    wm_request_t q;
    memset(&q, 0, sizeof(q));
    q.type = WM_REQ_DESTROY;
    q.window = mine;
    CHECK(raw_call(&q, 0, NULL) == EBADF);
    q.type = WM_REQ_MOVE;
    CHECK(raw_call(&q, 0, NULL) == EBADF);
    as(0);
    CHECK(dwm_window_exists(mine));

    // Its own window, then a buffer smaller than it claims, and no buffer
    memset(&q, 0, sizeof(q));
    q.type = WM_REQ_CREATE;
    q.w = 32; q.h = 32;
    q.x = q.y = WM_POS_DEFAULT;
    CHECK(raw_call(&q, 0, &m) == 0 && m.window != 0);
    wm_window_t theirs = m.window;
    as(PID_OTHER);
    int tiny = shm_create(16);
    memset(&q, 0, sizeof(q));
    q.type = WM_REQ_ATTACH;
    q.window = theirs;
    q.w = 64; q.h = 64;     // 16 KB claimed, one 4 KB page given
    CHECK(raw_call(&q, tiny, NULL) == EINVAL);
    CHECK(raw_call(&q, 0, NULL) == EINVAL);
    as(PID_OTHER);
    int pipe2[2];
    CHECK(ipc_pipe(pipe2) == 0);
    CHECK(raw_call(&q, pipe2[0], NULL) == EINVAL);              // Not shared memory
    CHECK(server_window_pixels(theirs, NULL, NULL) == NULL);

    // Nonsense: too short, unknown type, bad sizes, bad styles
    as(PID_OTHER);
    CHECK(ipc_send(raw_pipe, "junk", 4, 0) == 0);
    memset(&q, 0, sizeof(q));
    q.type = 999;
    q.window = theirs;
    CHECK(raw_call(&q, 0, NULL) == EINVAL);
    q.type = WM_REQ_CREATE;
    q.w = -5; q.h = 10;
    CHECK(raw_call(&q, 0, NULL) == EINVAL);
    q.w = 10; q.style = 0x8000;
    CHECK(raw_call(&q, 0, NULL) == EINVAL);

    // Ending the program closes its windows (only its own)
    as(PID_OTHER);
    __test_ipc_exit(PID_OTHER);
    dwm_round();
    as(0);
    CHECK(!dwm_window_exists(theirs) && dwm_window_exists(mine) && server_client_count() == 1);

    as(PID_CLIENT);
    CHECK(wm_window_destroy(mine) == 0);
    dwm_round();
}

static void test_disconnect_and_shutdown(void) {
    as(PID_CLIENT);
    wm_window_desc_t d = { .title = "A", .x = 10, .y = 10, .width = 50, .height = 40 };
    wm_window_t a = wm_window_create(&d), b = wm_window_create(&d);
    CHECK(a != 0 && b != 0 && a != b);
    wm_disconnect();
    CHECK(!wm_connected());
    dwm_round();
    as(0);
    CHECK(!dwm_window_exists(a) && !dwm_window_exists(b) && server_client_count() == 0);
    CHECK(__test_ipc_pages_live() == 0);

    // The DWM shutting down: the program hears its window is gone, then EPIPE
    as(PID_CLIENT);
    CHECK(wm_connect(1000) == 0);
    a = wm_window_create(&d);
    CHECK(a != 0);
    as(0);
    server_shutdown();
    wm_event_t ev;
    CHECK(client_wait_for(WM_EVENT_DESTROYED, &ev, NULL) && ev.window == a);
    as(PID_CLIENT);
    CHECK(wm_window_surface(a) == NULL);
    CHECK(wm_wait_event(&ev, 0) == -1 && errno == EPIPE);
    wm_disconnect();

    // And with no DWM at all
    CHECK(wm_connect(0) == -1 && errno == ENOENT);
    as(0);
}

int main(void) {
    if (!compositor_init(640, 480)) {
        printf("compositor_init failed\n");
        return 1;
    }
    CHECK(server_init());
    CHECK(server_init());                           // Once is enough
    as(PID_OTHER);
    CHECK(ipc_port_create(WM_PORT_NAME) == -1 && errno == EEXIST);  // Nobody else can take it
    as(0);
    __test_ipc_set_sleep_hook(hook);

    test_basics();
    test_isolation();
    test_disconnect_and_shutdown();

    __test_ipc_set_sleep_hook(NULL);
    window_destroy_all();
    compositor_shutdown();
    __test_ipc_exit(PID_CLIENT);
    __test_ipc_exit(PID_OTHER);
    CHECK(__test_ipc_pages_live() == 0 && __test_ipc_maps_live() == 0);
    CHECK(malloc_check() == 0);

    printf("wm: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
