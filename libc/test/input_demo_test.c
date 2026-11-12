/*
 * End-to-end: examples/input_demo.c, unchanged, against the kernel's own
 * IPC and gfx code (ipc_shim.c, gfx_shim.c). Scripted input arrives while
 * it waits: drag the box, nudge it with a key, quit with Esc. Then the
 * emulated screen is checked.
 */

#define main input_demo_main
#include "../examples/input_demo.c"
#undef main

#include <errno.h>
#include <string.h>

void     __test_ipc_set_sleep_hook(void (*fn)(void));
bool     __test_ipc_grabbed(void);
bool     __test_input_push(const input_event_t* ev);
uint32_t __test_gfx_screen_pixel(int32_t x, int32_t y);

static int failures = 0, checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// 640 x 480 screen: the box starts at (260, 200), 120 x 80
static const input_event_t script[] = {
    { INPUT_MOUSE, 0, 300, 240, 0,              0, 0, 1 },  // Hover
    { INPUT_MOUSE, 0, 300, 240, INPUT_BTN_LEFT, 0, 0, 2 },  // Press inside
    { INPUT_MOUSE, 0, 350, 270, INPUT_BTN_LEFT, 0, 0, 3 },
    { INPUT_MOUSE, 0, 400, 300, INPUT_BTN_LEFT, 0, 0, 4 },  // Box now at (360, 260)
    { INPUT_MOUSE, 0, 400, 300, 0,              0, 0, 5 },  // Release
    { INPUT_MOUSE, 0, 600, 100, 0,              0, 0, 6 },  // Away; a plain move doesn't drag
    { INPUT_KEY,   KEY_DOWN, 600, 100, 0,       0, 0, 7 },  // Box now at (360, 270)
    { INPUT_KEY,   KEY_ESCAPE, 600, 100, 0,     0, 0, 8 },
};
static unsigned step = 0;
static bool grabbed_while_running = false;

// One event per wait, so every step is drawn
static void feed(void) {
    grabbed_while_running = grabbed_while_running || __test_ipc_grabbed();
    if (step < sizeof(script) / sizeof(script[0]))
        __test_input_push(&script[step++]);
    __test_ipc_set_sleep_hook(feed);
}

int main(void) {
    __test_ipc_set_sleep_hook(feed);
    int rc = input_demo_main();
    __test_ipc_set_sleep_hook(NULL);

    CHECK(rc == 0);
    CHECK(step == sizeof(script) / sizeof(script[0]));
    CHECK(grabbed_while_running && !__test_ipc_grabbed());   // Let go on exit

    // Where the box ended up: (360, 270) .. (480, 350), hover colour off (mouse is away)
    CHECK(__test_gfx_screen_pixel(370, 280) == BOX_FILL);
    CHECK(__test_gfx_screen_pixel(470, 340) == BOX_FILL);
    CHECK(__test_gfx_screen_pixel(360, 270) == BOX_EDGE);
    // Where it was, and where it passed through: background again
    CHECK(__test_gfx_screen_pixel(270, 210) == BG);
    CHECK(__test_gfx_screen_pixel(365, 262) == BG);
    // No cursor trail at the drag positions
    CHECK(__test_gfx_screen_pixel(301, 245) == BG);
    // The cursor where the mouse is now, the status bar on top
    CHECK(__test_gfx_screen_pixel(600, 100) == GFX_COLOR_BLACK);
    CHECK(__test_gfx_screen_pixel(601, 105) == GFX_COLOR_WHITE);
    CHECK(__test_gfx_screen_pixel(5, 5) == GFX_RGB(20, 60, 90));

    // A picture of the screen, for a look by eye
    FILE* f = fopen("input_demo.ppm", "w");
    if (f != NULL) {
        fprintf(f, "P6\n640 480\n255\n");
        for (int y = 0; y < 480; y++)
            for (int x = 0; x < 640; x++) {
                uint32_t c = __test_gfx_screen_pixel(x, y);
                unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
    }

    printf("input_demo: %u scripted events, %d checks, %d failures\n", step, checks, failures);
    return failures != 0;
}

