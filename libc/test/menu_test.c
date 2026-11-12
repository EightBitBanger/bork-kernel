/*
 * Context menus with submenus: the DWM's menu.c (levels opening on hover
 * beside their item, keys, dimmed empty submenus), and wm_menu_show_tree
 * through server.c, including requests that break the submenu rules. Runs
 * the real DWM, the kernel's ipc/ipc.c and gfx code in one process, as
 * tray_test.c does.
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
#include "dwm/menu.h"
#include "dwm/text.h"

int      malloc_check(void);
void     __test_ipc_set_pid(uint32_t pid);
void     __test_ipc_set_sleep_hook(void (*fn)(void));
void     __test_ipc_exit(uint32_t pid);
uint32_t __test_ipc_pages_live(void);
uint32_t __test_ipc_maps_live(void);
uint32_t __test_gfx_screen_pixel(int32_t x, int32_t y);

#define PID_CLIENT  201
#define PID_RAW     202

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

//
// Menu geometry, as menu.c lays it out (BORDER 1, PAD_TOP 2, ITEM_PAD_Y 3,
// CHECK_COLUMN 16, PAD_RIGHT 12, ARROW_GAP 12, ARROW_PAD 5, MIN_WIDTH 96)
//

static int32_t item_h(void) { return dwm_text_height() + 6; }

static int32_t menu_w(const char* const* labels, size_t n, bool arrows) {
    int32_t widest = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t w = dwm_text_width(labels[i], strlen(labels[i]));
        if (w > widest) widest = w;
    }
    int32_t w = 2 + 16 + widest + (arrows ? 12 + dwm_text_width(">", 1) + 5 : 12);
    return w < 96 ? 96 : w;
}

// The middle of item i (no separators above it) of a level at (x, y)
static int32_t row_y(int32_t top, int i) { return top + 3 + i * item_h() + item_h() / 2; }

static void move(int32_t x, int32_t y)  { menu_mouse(x, y, true, 0, 0, 0); }
static void click_at(int32_t x, int32_t y) {
    menu_mouse(x, y, true, 0, 0, 0);
    menu_mouse(x, y, false, 1, 0, 1);
    menu_mouse(x, y, false, 0, 1, 0);
}

//
// The DWM's side: dwm_menu_show on the desktop
//

static uint32_t chosen;         // The last action's id, 0: none
static int      chosen_count;
static void chose(const dwm_menu_context_t* ctx) { chosen = ctx->id; chosen_count++; }

static const dwm_menu_item_t deep_items[] = {
    { .label = "Deep", .id = 21, .action = chose },
};
static const dwm_menu_t deep_menu = DWM_MENU_INIT(deep_items, NULL);

static const dwm_menu_item_t sort_items[] = {
    { .label = "Name", .id = 11, .action = chose },
    { .label = "Type", .id = 12, .action = chose },
    { .label = "More", .submenu = &deep_menu },
};
static const dwm_menu_t sort_menu = DWM_MENU_INIT(sort_items, NULL);

static const dwm_menu_item_t no_items[] = {
    { .label = "Gone", .id = 31, .flags = DWM_MENU_HIDDEN },
};
static const dwm_menu_t empty_menu = DWM_MENU_INIT(no_items, NULL);

static const dwm_menu_item_t top_items[] = {
    { .label = "Refresh", .id = 1, .action = chose },
    { .label = "Sort By", .submenu = &sort_menu },
    { .label = "Nothing", .submenu = &empty_menu },
    { .label = "Close",   .id = 3, .action = chose },
};
static const dwm_menu_t top_menu = DWM_MENU_INIT(top_items, NULL);

#define MX  50
#define MY  50

static void test_levels(void) {
    static const char* const top_labels[] = { "Refresh", "Sort By", "Nothing", "Close" };
    static const char* const sort_labels[] = { "Name", "Type", "More" };
    int32_t w0 = menu_w(top_labels, 4, true);
    int32_t w1 = menu_w(sort_labels, 3, true);
    int32_t sub_x = MX + w0 - 1;                // Where Sort By's submenu starts
    int32_t sub_top = MY + item_h();             // Level with Sort By (its border above it)
    int32_t deep_x = sub_x + w1 - 1;
    int32_t deep_top = sub_top + 2 * item_h();

    // An ordinary item, chosen from the menu
    chosen = 0; chosen_count = 0;
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    click_at(MX + 20, row_y(MY, 0));
    CHECK(chosen == 1 && chosen_count == 1 && !dwm_menu_is_open());

    // Resting on Sort By opens its submenu beside it: Type is chosen there
    chosen = 0;
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    move(MX + 20, row_y(MY, 1));
    move(sub_x + 20, row_y(sub_top, 1));
    click_at(sub_x + 20, row_y(sub_top, 1));
    CHECK(chosen == 12 && !dwm_menu_is_open());

    // Two levels down
    chosen = 0;
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    move(MX + 20, row_y(MY, 1));
    move(sub_x + 20, row_y(sub_top, 2));         // More
    click_at(deep_x + 20, row_y(deep_top, 0));    // Deep
    CHECK(chosen == 21 && !dwm_menu_is_open());

    // Going back to Refresh shuts the submenu: a click where it was is
    // outside every level and closes the menu, nothing chosen
    chosen = 0;
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    move(MX + 20, row_y(MY, 1));
    move(MX + 20, row_y(MY, 0));
    click_at(sub_x + 20, row_y(sub_top, 1));
    CHECK(chosen == 0 && !dwm_menu_is_open());

    // Clicking Sort By itself chooses nothing and leaves the menu open;
    // the submenu is there to choose from
    chosen = 0;
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    click_at(MX + 20, row_y(MY, 1));
    CHECK(chosen == 0 && dwm_menu_is_open());
    click_at(sub_x + 20, row_y(sub_top, 0));
    CHECK(chosen == 11 && !dwm_menu_is_open());

    // A submenu with nothing to show: its item is dimmed, nothing opens
    chosen = 0;
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    move(MX + 20, row_y(MY, 2));
    click_at(MX + 20, row_y(MY, 2));
    CHECK(chosen == 0 && dwm_menu_is_open());
    click_at(MX + w0 + 20, row_y(MY + 2 * item_h(), 0));      // Where it would be
    CHECK(chosen == 0 && !dwm_menu_is_open());

    // Keys: Down Down Right opens Sort By at Name, Down Enter is Type;
    // Esc in a submenu goes back to the menu, again closes it
    chosen = 0;
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    menu_key(KEY_DOWN); menu_key(KEY_DOWN); menu_key(KEY_RIGHT);
    menu_key(KEY_DOWN); menu_key(KEY_ENTER);
    CHECK(chosen == 12 && !dwm_menu_is_open());
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    menu_key(KEY_DOWN); menu_key(KEY_DOWN); menu_key(KEY_RIGHT);
    menu_key(KEY_ESCAPE);
    CHECK(dwm_menu_is_open());
    menu_key(KEY_LEFT);                          // At the top already: nothing
    CHECK(dwm_menu_is_open());
    menu_key(KEY_ENTER);                         // Sort By again: opens, chooses nothing
    CHECK(dwm_menu_is_open() && chosen == 12);
    menu_key(KEY_ESCAPE); menu_key(KEY_ESCAPE);
    CHECK(!dwm_menu_is_open());

    // At the right edge of the screen the submenu opens on the left
    int32_t sw = compositor_width();
    int32_t ex = sw - w0 - 2;
    chosen = 0;
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, ex, MY));
    move(ex + 20, row_y(MY, 1));
    click_at(ex - w1 + 20, row_y(sub_top, 0));
    CHECK(chosen == 11 && !dwm_menu_is_open());

    compositor_render();         // Paints every level without trouble
}

// `menu_test shot.ppm`: the screen with Sort By's submenu open and More's
// below it, for a look
static void write_shot(const char* path) {
    CHECK(dwm_menu_show(DWM_DESKTOP, &top_menu, MX, MY));
    static const char* const top_labels[] = { "Refresh", "Sort By", "Nothing", "Close" };
    static const char* const sort_labels[] = { "Name", "Type", "More" };
    int32_t sub_x = MX + menu_w(top_labels, 4, true) - 1, sub_top = MY + item_h();
    move(MX + 20, row_y(MY, 1));
    move(sub_x + 20, row_y(sub_top, 2));
    move(sub_x + menu_w(sort_labels, 3, true) + 10, row_y(sub_top + 2 * item_h(), 0));
    compositor_invalidate(gfx_rect(0, 0, compositor_width(), compositor_height()));
    compositor_render();
    FILE* f = fopen(path, "wb");
    if (f != NULL) {
        int32_t w = 360, h = 180;
        fprintf(f, "P6\n%d %d\n255\n", (int)w, (int)h);
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++) {
                uint32_t p = __test_gfx_screen_pixel(x, y);
                unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
    }
    dwm_menu_close();
}

//
// A program's side: wm_menu_show_tree through the server
//

static void test_client(void) {
    as(PID_CLIENT);
    wm_window_desc_t d = { .title = "menus", .x = 10, .y = 10, .width = 300, .height = 200 };
    wm_window_t win = wm_window_create(&d);
    CHECK(win != 0);
    gfx_surface_t* g = wm_window_surface(win);
    CHECK(g != NULL);
    if (g) gfx_clear(g, GFX_COLOR_BLACK);
    CHECK(wm_window_commit(win, NULL) == 0);
    dwm_round();

    static const wm_menu_node_t deep[] = { { "Deep", 21, 0, NULL, 0 } };
    static const wm_menu_node_t sort[] = {
        { "Name", 11, WM_MENU_CHECKED, NULL, 0 },
        { "Type", 12, 0, NULL, 0 },
        { NULL, 0, WM_MENU_SEPARATOR, NULL, 0 },
        { "More", 0, 0, deep, 1 },
    };
    static const wm_menu_node_t top[] = {
        { "Refresh", 1, 0, NULL, 0 },
        { "Sort By", 0, 0, sort, 4 },
        { "Close", 3, 0, NULL, 0 },
    };

    // Down Down Right Down Enter: Type, back to the program
    as(PID_CLIENT);
    CHECK(wm_menu_show_tree(win, 20, 20, top, 3) == 0);
    as(0);
    CHECK(dwm_menu_is_open());
    menu_key(KEY_DOWN); menu_key(KEY_DOWN); menu_key(KEY_RIGHT);
    menu_key(KEY_DOWN); menu_key(KEY_ENTER);
    wm_event_t ev;
    CHECK(client_wait_for(WM_EVENT_MENU_COMMAND, &ev) && ev.param == 12);

    // Three levels down: Sort By, More, Deep (the separator is skipped)
    as(PID_CLIENT);
    CHECK(wm_menu_show_tree(win, 20, 20, top, 3) == 0);
    as(0);
    menu_key(KEY_DOWN); menu_key(KEY_DOWN); menu_key(KEY_RIGHT);
    menu_key(KEY_UP); menu_key(KEY_RIGHT); menu_key(KEY_ENTER);
    CHECK(client_wait_for(WM_EVENT_MENU_COMMAND, &ev) && ev.param == 21);

    // Plain wm_menu_show still works
    static const wm_menu_item_t plain[] = { { "Copy", 7, 0 } };
    as(PID_CLIENT);
    CHECK(wm_menu_show(win, 5, 5, plain, 1) == 0);
    as(0);
    menu_key(KEY_DOWN); menu_key(KEY_ENTER);
    CHECK(client_wait_for(WM_EVENT_MENU_COMMAND, &ev) && ev.param == 7);

    // Too deep (a fifth level) or too many in one submenu: refused here
    static const wm_menu_node_t l4[] = { { "4", 40, 0, NULL, 0 } };
    static const wm_menu_node_t l3[] = { { "3", 0, 0, l4, 1 } };
    static const wm_menu_node_t l2[] = { { "2", 0, 0, l3, 1 } };
    static const wm_menu_node_t l1[] = { { "1", 0, 0, l2, 1 } };
    static const wm_menu_node_t l0[] = { { "0", 0, 0, l1, 1 } };
    as(PID_CLIENT);
    errno = 0;
    CHECK(wm_menu_show_tree(win, 5, 5, l0, 1) < 0 && errno == EINVAL);
    CHECK(wm_menu_show_tree(win, 5, 5, l1, 1) == 0);        // Four levels is fine
    as(0);
    dwm_menu_close();
    wm_menu_node_t many[WM_MENU_MAX_ITEMS + 1];
    for (size_t i = 0; i < WM_MENU_MAX_ITEMS + 1; i++)
        many[i] = (wm_menu_node_t){ "x", (uint32_t)i + 100, 0, NULL, 0 };
    wm_menu_node_t holder[] = { { "Many", 0, 0, many, WM_MENU_MAX_ITEMS + 1 } };
    as(PID_CLIENT);
    CHECK(wm_menu_show_tree(win, 5, 5, holder, 1) < 0 && errno == EINVAL);

    as(PID_CLIENT);
    wm_window_destroy(win);
    dwm_round();
}

//
// Requests that break the rules, sent by hand
//

static int raw_window(int pipe) {
    wm_request_t q;
    memset(&q, 0, sizeof(q));
    q.type = WM_REQ_CREATE;
    q.seq = ++raw_seq;
    q.w = 100; q.h = 100;
    strlcpy(q.title, "raw", sizeof(q.title));
    as(PID_RAW);
    if (ipc_send(pipe, &q, sizeof(q), 0) < 0)
        return 0;
    for (int i = 0; i < 10; i++) {
        dwm_round();
        as(PID_RAW);
        wm_message_t m;
        while (ipc_recv(pipe, &m, sizeof(m), NULL) == sizeof(m))
            if (m.kind == WM_MSG_REPLY && m.seq == q.seq)
                return m.status == 0 ? (int)m.window : 0;
    }
    return 0;
}

static int raw_tree(uint32_t count, const wm_menu_tree_entry_t* e, size_t bytes) {
    as(PID_RAW);
    int shm = shm_create(bytes);
    wm_menu_tree_t* t = (shm > 0) ? (wm_menu_tree_t*)shm_map(shm) : NULL;
    if (t == NULL)
        return -1;
    memset(t, 0, bytes);
    t->count = count;
    size_t room = (bytes - sizeof(*t)) / sizeof(t->items[0]);
    for (uint32_t i = 0; i < count && i < room; i++)
        t->items[i] = e[i];
    return shm;
}

static void test_hostile(void) {
    int pipe = raw_connect(PID_RAW);
    CHECK(pipe > 0);
    int win = raw_window(pipe);
    CHECK(win != 0);
    // A window must have been drawn once to show: attach and commit are
    // more than this needs, so the menus below check the rules only (each
    // fails before or at showing with EINVAL)

    wm_menu_request_t q;
    memset(&q, 0, sizeof(q));
    q.type = WM_REQ_MENU;
    q.window = (uint32_t)win;
    q.count = 2;
    strlcpy(q.items[0].label, "A", sizeof(q.items[0].label));
    q.items[0].id = 1;
    strlcpy(q.items[1].label, "Sub", sizeof(q.items[1].label));
    q.items[1].flags = WM_MENU_SUBMENU;

    // SUBMENU with no block
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), 0) == EINVAL);

    wm_menu_tree_entry_t e[WM_MENU_TREE_MAX + 2];
    memset(e, 0, sizeof(e));
    for (int i = 0; i < WM_MENU_TREE_MAX + 2; i++) {
        e[i].parent = 1;
        e[i].id = 50 + (uint32_t)i;
        strlcpy(e[i].label, "c", sizeof(e[i].label));
    }
    size_t big = sizeof(wm_menu_tree_t) + (WM_MENU_TREE_MAX + 2) * sizeof(e[0]);

    // A parent that is not a SUBMENU item, one after itself, one out of range
    e[0].parent = 0;
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), raw_tree(1, e, big)) == EINVAL);
    e[0].parent = 2;
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), raw_tree(1, e, big)) == EINVAL);
    e[0].parent = 0xFFFFFFFFu;
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), raw_tree(1, e, big)) == EINVAL);
    e[0].parent = 1;

    // An item that is its own parent's parent can't happen: parents come first
    e[0].flags = WM_MENU_SUBMENU;
    e[1].parent = 3;                 // Itself (2 + 1)
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), raw_tree(2, e, big)) == EINVAL);
    e[0].flags = 0;
    e[1].parent = 1;

    // More than WM_MENU_MAX_ITEMS in one submenu, more than TREE_MAX in all,
    // a count bigger than the block, no entries
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), raw_tree(WM_MENU_MAX_ITEMS + 1, e, big)) == EINVAL);
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), raw_tree(WM_MENU_TREE_MAX + 1, e, big)) == EINVAL);
    as(PID_RAW);
    int small = shm_create(64);
    wm_menu_tree_t* t = (wm_menu_tree_t*)shm_map(small);
    t->count = 30;
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), small) == EINVAL);
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), raw_tree(0, e, big)) == EINVAL);

    // Not a shared memory block
    as(PID_RAW);
    int fds[2];
    CHECK(ipc_pipe(fds) == 0);
    CHECK(raw_send(PID_RAW, pipe, &q, sizeof(q), fds[0]) == EINVAL);
    CHECK(!dwm_menu_is_open());

    as(PID_RAW);
    ipc_close(pipe);
    dwm_round();
}

int main(int argc, char** argv) {
    if (!compositor_init(640, 480)) {
        printf("compositor_init failed\n");
        return 1;
    }
    CHECK(server_init());
    as(0);
    test_levels();
    if (argc > 1)
        write_shot(argv[1]);

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
    __test_ipc_exit(PID_RAW);
    CHECK(__test_ipc_pages_live() == 0 && __test_ipc_maps_live() == 0);
    CHECK(malloc_check() == 0);

    printf("menu: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
