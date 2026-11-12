/*
 * Graphics tests: os/gfx.h through the kernel's own gfx code and command
 * interpreter (gfx_shim.c). Linked like a real user program.
 *
 * Writes gfx_test.ppm, a picture of the demo surface, for a look by eye.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/gfx.h>
#include <os/syscall.h>

int malloc_check(void);
uint32_t __test_gfx_screen_pixel(int32_t x, int32_t y);

static int failures = 0, checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); } \
} while (0)

#define BLACK GFX_COLOR_BLACK
#define RED   GFX_RGB(255, 0, 0)
#define GREEN GFX_RGB(0, 255, 0)
#define BLUE  GFX_RGB(0, 0, 255)

static int count_color(gfx_surface_t* s, int x, int y, int w, int h, gfx_color_t c) {
    static uint32_t buf[400 * 300];
    if (gfx_read_pixels(s, x, y, w, h, buf, 0) < 0) return -1;
    int n = 0;
    for (int i = 0; i < w * h; i++) n += (buf[i] == c);
    return n;
}

static void test_basics(void) {
    gfx_info_t info;
    CHECK(gfx_get_info(&info) == 0);
    CHECK(info.width == 640 && info.height == 480 && info.bpp == 32);
    CHECK(info.char_width == 6 && info.line_height == 8 && info.max_surfaces == 16);

    errno = 0;
    CHECK(gfx_surface_create(0, 10) == NULL && errno == EINVAL);
    CHECK(gfx_surface_create(5000, 10) == NULL && errno == EINVAL);

    gfx_surface_t* s = gfx_surface_create(100, 80);
    CHECK(s != NULL && gfx_surface_width(s) == 100 && gfx_surface_height(s) == 80);
    CHECK(gfx_get_pixel(s, 5, 5) == BLACK);              // New surfaces are opaque black
    CHECK(count_color(s, 0, 0, 100, 80, BLACK) == 8000);

    gfx_fill_rect(s, 10, 10, 20, 5, RED);
    CHECK(gfx_get_pixel(s, 10, 10) == RED);               // get_pixel flushes
    CHECK(gfx_get_pixel(s, 29, 14) == RED && gfx_get_pixel(s, 30, 14) == BLACK && gfx_get_pixel(s, 29, 15) == BLACK);
    CHECK(count_color(s, 0, 0, 100, 80, RED) == 100);

    // Outline, lines
    gfx_clear(s, BLACK);
    gfx_draw_rect(s, 0, 0, 10, 10, GREEN);
    gfx_draw_line(s, 20, 20, 29, 20, BLUE);
    gfx_draw_hline(s, 0, 50, 100, RED);
    gfx_draw_vline(s, 99, 0, 80, RED);
    CHECK(gfx_flush(s) == 0);
    CHECK(count_color(s, 0, 0, 10, 10, GREEN) == 36);
    CHECK(count_color(s, 20, 20, 10, 1, BLUE) == 10);
    CHECK(count_color(s, 0, 50, 100, 1, RED) == 100);
    CHECK(count_color(s, 99, 0, 1, 80, RED) == 80);

    // Origin and clip
    gfx_clear(s, BLACK);
    gfx_set_origin(s, 40, 30);
    gfx_set_pixel(s, 0, 0, GREEN);
    int32_t ox, oy;
    gfx_get_origin(s, &ox, &oy);
    CHECK(ox == 40 && oy == 30);
    gfx_set_origin(s, 0, 0);
    CHECK(gfx_get_pixel(s, 40, 30) == GREEN);

    gfx_rect_t clip = gfx_rect(10, 10, 5, 5);
    gfx_set_clip(s, &clip);
    gfx_fill_rect(s, 0, 0, 100, 80, RED);
    gfx_set_clip(s, NULL);
    CHECK(gfx_flush(s) == 0);
    CHECK(count_color(s, 0, 0, 100, 80, RED) == 25);

    // Clip survives an origin change; save/restore bring both back
    CHECK(gfx_save(s) == 0);
    gfx_clear(s, BLACK);
    gfx_set_clip(s, &clip);
    gfx_translate(s, 50, 0);
    gfx_fill_rect(s, -50, 0, 100, 80, BLUE);
    CHECK(gfx_restore(s) == 0);
    gfx_get_origin(s, &ox, &oy);
    CHECK(ox == 0 && oy == 0);
    gfx_fill_rect(s, 0, 0, 1, 1, GREEN);                  // Clip is the whole surface again
    CHECK(gfx_flush(s) == 0);
    CHECK(count_color(s, 0, 0, 100, 80, BLUE) == 25 && gfx_get_pixel(s, 0, 0) == GREEN);
    CHECK(gfx_restore(s) == -1);
    for (int i = 0; i < 8; i++) CHECK(gfx_save(s) == 0);
    CHECK(gfx_save(s) == -1);
    for (int i = 0; i < 8; i++) gfx_restore(s);
    CHECK(gfx_flush(s) == 0);

    // Alpha blending
    gfx_clear(s, BLACK);
    CHECK(gfx_set_blend(s, GFX_BLEND_ALPHA) == GFX_BLEND_COPY);
    gfx_fill_rect(s, 0, 0, 4, 4, GFX_ARGB(128, 255, 255, 255));
    CHECK(gfx_set_blend(s, GFX_BLEND_COPY) == GFX_BLEND_ALPHA);
    gfx_color_t c = gfx_get_pixel(s, 1, 1);
    CHECK(GFX_ALPHA(c) == 0xFF && GFX_RED(c) == 128 && GFX_GREEN(c) == 128);

    // Gradient end points
    gfx_fill_gradient(s, 0, 0, 100, 1, GFX_RGB(0, 0, 0), GFX_RGB(0, 0, 99), GFX_GRADIENT_HORIZONTAL);
    CHECK(GFX_BLUE(gfx_get_pixel(s, 0, 0)) == 0 && GFX_BLUE(gfx_get_pixel(s, 99, 0)) == 99);

    // Circles
    gfx_clear(s, BLACK);
    gfx_fill_circle(s, 50, 40, 10, RED);
    CHECK(gfx_flush(s) == 0);
    int disc = count_color(s, 0, 0, 100, 80, RED);
    CHECK(disc > 290 && disc < 340);                      // ~ pi * 10^2
    CHECK(gfx_get_pixel(s, 50, 40) == RED && gfx_get_pixel(s, 61, 40) == BLACK);

    CHECK(gfx_surface_destroy(s) == 0);
}

static void test_text(void) {
    gfx_surface_t* s = gfx_surface_create(400, 40);
    CHECK(gfx_char_width(s) == 6 && gfx_line_height(s) == 8);
    CHECK(gfx_text_width(s, "abcd") == 24);

    gfx_draw_text(s, 0, 0, "H", GREEN);
    CHECK(gfx_flush(s) == 0);
    int on = count_color(s, 0, 0, 6, 8, GREEN);
    CHECK(on > 5 && on < 48);

    // Opaque cells fill the background
    gfx_draw_text_opaque(s, 0, 10, "  ", GREEN, BLUE);
    CHECK(gfx_flush(s) == 0);
    CHECK(count_color(s, 0, 10, 12, 8, BLUE) == 96);

    // Scale 2: cells are 12 x 16, and long strings split across commands
    // land exactly where one call would put them
    CHECK(gfx_set_text_scale(s, 2) == 1);
    CHECK(gfx_char_width(s) == 12 && gfx_text_width(s, "ab") == 24);
    CHECK(gfx_set_text_scale(s, 20) == 2 && gfx_char_width(s) == 48);   // Clamped to 8
    gfx_set_text_scale(s, 1);

    char long_text[401];
    memset(long_text, ' ', 400);
    long_text[399] = 'X';
    long_text[400] = '\0';
    gfx_clear(s, BLACK);
    gfx_draw_text_opaque(s, -1000 + 6 * 60, 20, long_text, RED, BLACK);   // Mostly off the left edge
    CHECK(gfx_flush(s) == 0);
    // 'X' is character 399 -> x = -640 + 6 * 399 = 1754: off the right edge,
    // so try a version that ends on screen
    gfx_clear(s, BLACK);
    gfx_draw_text(s, 6 * 66 - 6 * 399, 20, long_text, RED);   // 'X' lands at x = 396
    CHECK(gfx_flush(s) == 0);
    CHECK(count_color(s, 390, 20, 6, 8, RED) == 0 && count_color(s, 396, 20, 4, 8, RED) > 3);

    gfx_draw_char(s, 0, 30, 'A', GREEN);
    gfx_draw_char_opaque(s, 6, 30, 0, GREEN, BLUE);       // '\0' is a blank cell
    CHECK(gfx_flush(s) == 0);
    CHECK(count_color(s, 0, 30, 6, 8, GREEN) > 5 && count_color(s, 6, 30, 6, 8, BLUE) == 48);

    gfx_surface_destroy(s);
}

static void test_pixels_and_blit(void) {
    gfx_surface_t* a = gfx_surface_create(64, 64);
    gfx_surface_t* b = gfx_surface_create(32, 32);

    // Raw write/read with a stride
    static uint32_t img[16 * 20];
    for (int i = 0; i < 16 * 20; i++) img[i] = 0xFF000000u | (uint32_t)i * 977u;
    CHECK(gfx_write_pixels(a, 5, 6, 10, 16, img, 20) == 0);
    static uint32_t back[10 * 16];
    CHECK(gfx_read_pixels(a, 5, 6, 10, 16, back, 0) == 0);
    int same = 1;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 10; x++) same &= back[y * 10 + x] == img[y * 20 + x];
    CHECK(same);
    CHECK(gfx_write_pixels(a, 60, 0, 10, 1, img, 0) == -1 && errno == EINVAL);   // Not inside
    CHECK(gfx_write_pixels(a, 0, 0, 4, 2, img, 3) == -1 && errno == EINVAL);     // Stride < w
    CHECK(gfx_read_pixels(a, 0, 0, 4, 4, (uint32_t*)16, 0) == -1 && errno == EFAULT);

    // Keyed sprite through the context (origin applies, key skipped)
    uint32_t sprite[4] = { RED, 0xFF00FF00u, 0xFF00FF00u, RED };
    gfx_clear(a, BLACK);
    gfx_set_origin(a, 10, 10);
    gfx_draw_pixels_keyed(a, sprite, 2, 2, 0, 0, 0, 0xFF00FF00u);
    gfx_set_origin(a, 0, 0);
    CHECK(gfx_flush(a) == 0);
    CHECK(gfx_get_pixel(a, 10, 10) == RED && gfx_get_pixel(a, 11, 10) == BLACK && gfx_get_pixel(a, 11, 11) == RED);

    // A big image goes through the kernel in several strips
    static uint32_t big[1000 * 30];
    for (int i = 0; i < 1000 * 30; i++) big[i] = 0xFF000000u | (uint32_t)i;
    gfx_surface_t* wide = gfx_surface_create(1000, 30);
    gfx_draw_pixels(wide, big, 1000, 30, 0, 0, 0);
    CHECK(gfx_flush(wide) == 0);
    CHECK(gfx_get_pixel(wide, 999, 29) == big[29 * 1000 + 999] && gfx_get_pixel(wide, 0, 17) == big[17000]);
    gfx_surface_destroy(wide);

    // Blit: b's pending drawing is flushed first, later drawing is not seen
    gfx_clear(b, BLUE);
    gfx_fill_rect(b, 0, 0, 4, 4, RED);
    gfx_clear(a, BLACK);
    gfx_rect_t part = gfx_rect(-2, -2, 6, 6);             // Cut to b: (0,0,4,4) lands at (22,22)
    gfx_blit(a, b, &part, 20, 20);
    gfx_clear(b, GREEN);
    CHECK(gfx_flush(a) == 0 && gfx_flush(b) == 0);
    CHECK(count_color(a, 0, 0, 64, 64, RED) == 16 && gfx_get_pixel(a, 22, 22) == RED && gfx_get_pixel(a, 21, 21) == BLACK);

    gfx_blit_keyed(a, b, NULL, 0, 0, GREEN);              // All key: nothing copied
    CHECK(gfx_flush(a) == 0 && gfx_get_pixel(a, 0, 0) == BLACK);
    gfx_blit(a, b, NULL, 32, 32);
    CHECK(gfx_flush(a) == 0 && count_color(a, 32, 32, 32, 32, GREEN) == 1024);

    gfx_blit(a, a, NULL, 0, 0);                           // Self blit is refused
    CHECK(gfx_flush(a) == -1 && errno == EINVAL);
    CHECK(gfx_flush(a) == 0);                             // The error was reported once

    // Present to the screen, whole and partial
    gfx_clear(a, RED);
    CHECK(gfx_present(a, 600, 450) == 0);                 // Partly off screen: clipped
    CHECK(__test_gfx_screen_pixel(600, 450) == RED && __test_gfx_screen_pixel(639, 479) == RED);
    CHECK(__test_gfx_screen_pixel(599, 450) != RED);
    gfx_fill_rect(a, 0, 0, 8, 8, BLUE);
    gfx_rect_t src = gfx_rect(-4, -4, 8, 8);              // Only (0,0,4,4) exists, shown at (104,104)
    CHECK(gfx_present_rect(a, &src, 100, 100) == 0);
    CHECK(__test_gfx_screen_pixel(104, 104) == BLUE && __test_gfx_screen_pixel(107, 107) == BLUE &&
          __test_gfx_screen_pixel(108, 108) != BLUE && __test_gfx_screen_pixel(103, 103) != BLUE);

    gfx_surface_destroy(a);
    gfx_surface_destroy(b);
}

static void test_errors_and_limits(void) {
    gfx_surface_t* s = gfx_surface_create(50, 50);

    // A bad command is skipped, the rest of the batch still runs
    gfx_fill_rect(s, 0, 0, 5, 5, RED);
    gfx_fill_circle(s, 10, 10, 100000, RED);              // Radius out of range
    gfx_fill_rect(s, 40, 40, 5, 5, GREEN);
    CHECK(gfx_flush(s) == -1 && errno == EINVAL);
    CHECK(gfx_get_pixel(s, 0, 0) == RED && gfx_get_pixel(s, 44, 44) == GREEN);

    gfx_draw_line(s, 0, 0, 2000000000, 5, RED);           // Coordinates far past the limits
    CHECK(gfx_flush(s) == -1 && errno == EINVAL);
    gfx_translate(s, 9000, 0);                            // Origin would leave the range: refused here
    int32_t ox;
    gfx_get_origin(s, &ox, NULL);
    CHECK(ox == 0 && gfx_flush(s) == -1 && errno == EINVAL);

    // Thousands of commands: the buffer flushes itself along the way
    gfx_clear(s, BLACK);
    for (int i = 0; i < 2500; i++) gfx_set_pixel(s, i % 50, i / 50, GREEN);
    CHECK(gfx_flush(s) == 0 && count_color(s, 0, 0, 50, 50, GREEN) == 2500);

    // Raw submits: a bad header ends the batch, unknown ops are skipped
    uint32_t bad[] = { SYS_GFX_CMD_HEADER(SYS_GFX_CMD_CLEAR, 2), RED, SYS_GFX_CMD_HEADER(99, 1),
                       SYS_GFX_CMD_HEADER(SYS_GFX_CMD_CLEAR, 9), BLUE };
    CHECK(syscall3(SYS_GFX_SUBMIT, 1, (uint32_t)bad, 8) == 1);
    CHECK(syscall3(SYS_GFX_SUBMIT, 1, (uint32_t)bad, 12) == SYS_EINVAL);
    CHECK(syscall3(SYS_GFX_SUBMIT, 1, (uint32_t)bad, 20) == SYS_EINVAL);
    CHECK(syscall3(SYS_GFX_SUBMIT, 1, (uint32_t)bad, 6) == SYS_EINVAL);
    CHECK(syscall3(SYS_GFX_SUBMIT, 77, (uint32_t)bad, 8) == SYS_EBADF);
    uint32_t pix[] = { SYS_GFX_CMD_HEADER(SYS_GFX_CMD_PIXELS, 9), 16, 4, 4, 0, 0, 0, 0, 0 };
    CHECK(syscall3(SYS_GFX_SUBMIT, 1, (uint32_t)pix, sizeof(pix)) == SYS_EFAULT);

    // Handles: limit and reuse
    gfx_surface_t* many[16];
    int made = 0;
    while (made < 16 && (many[made] = gfx_surface_create(8, 8)) != NULL) made++;
    CHECK(made == 15 && errno == EMFILE);                 // s holds the 16th
    for (int i = 0; i < made; i++) gfx_surface_destroy(many[i]);
    gfx_surface_t* again = gfx_surface_create(8, 8);
    CHECK(again != NULL);
    gfx_surface_destroy(again);

    gfx_surface_destroy(s);
    CHECK(syscall1(SYS_GFX_DESTROY, 1) == SYS_EBADF);
}

// Random commands with random arguments straight at the interpreter. Every
// write it makes must stay inside a surface: any stray one lands in this
// process's heap, which malloc_check() then finds corrupt.
static int fuzz_ran, fuzz_rejected;
static void test_fuzz(void) {
    gfx_surface_t* s = gfx_surface_create(64, 48);
    gfx_surface_t* t = gfx_surface_create(16, 16);
    static uint32_t buf[1024];
    uint32_t rng = 7;
    #define RND() (rng = rng * 1103515245u + 12345u, rng >> 3)

    static const int32_t edges[] = { 0, 1, -1, 63, 64, 47, 48, 8191, -8191, 8192, -8192, 16384,
                                     -16384, 2147483647, (int32_t)0x80000000, 4096, 100000 };
    for (int round = 0; round < 20000; round++) {
        uint32_t words = 0;
        int ncmd = 1 + (int)(RND() % 6);
        for (int c = 0; c < ncmd && words < 900; c++) {
            // Mostly the right length, so the value checks get the hits
            static const uint8_t argn[] = { 0, 1, 3, 5, 5, 5, 7, 4, 4, 6, 6, 6, 9, 8, 5, 4, 2, 2, 1, 1, 0, 0 };
            uint32_t op = 1 + RND() % 21;
            uint32_t argc = (RND() % 8) ? argn[op] : RND() % 12;
            if (op == SYS_GFX_CMD_TEXT) argc = 6 + RND() % 10;
            if (op == SYS_GFX_CMD_PIE) argc = 6 + RND() % 70;
            buf[words] = SYS_GFX_CMD_HEADER(op, argc + 1);
            for (uint32_t i = 1; i <= argc; i++) {
                uint32_t k = RND() % 4;
                buf[words + i] = (k == 0) ? (uint32_t)edges[RND() % (sizeof(edges) / 4)]
                               : (k == 1) ? RND() % 200 - 100 : (k == 2) ? RND() % 70 : RND() * 2654435761u;
            }
            if (op == SYS_GFX_CMD_TEXT && argc >= 6)
                buf[words + 6] = (RND() % 4) ? (argc - 6) * 4 - RND() % 4 : RND() % 300;
            if (op == SYS_GFX_CMD_PIE && argc >= 6)
                buf[words + 6] = (RND() % 4 && argc % 2 == 0) ? (argc - 6) / 2 : RND() % 40;
            if (op == SYS_GFX_CMD_BLIT) buf[words + 1] = RND() % 4;
            if (op == SYS_GFX_CMD_PIXELS && argc >= 4) {
                // Real memory, small enough to stay inside buf (the shim reads it directly)
                buf[words + 1] = (uint32_t)buf;
                buf[words + 2] = RND() % 17;
                buf[words + 3] = RND() % 17;
                buf[words + 4] = RND() % 33;
            }
            words += argc + 1;
        }
        int32_t r = syscall3(SYS_GFX_SUBMIT, RND() % 2 ? 1 : 2, (uint32_t)buf, words * 4);
        if (r > 0) fuzz_ran += r; else fuzz_rejected++;
        if (round % 2000 == 0) CHECK(malloc_check() == 0);
    }
    CHECK(malloc_check() == 0);
    gfx_surface_destroy(s);
    gfx_surface_destroy(t);
    CHECK(fuzz_ran > 1000);     // The drawing code really ran, not only the checks
    printf("gfx fuzz: 20000 random batches (%d commands in fully valid batches, %d batches with a rejected command), heap intact\n",
           fuzz_ran, fuzz_rejected);
}

// A picture to look at
static void demo_picture(void) {
    gfx_surface_t* s = gfx_surface_create(320, 200);
    gfx_fill_gradient(s, 0, 0, 320, 200, GFX_RGB(20, 24, 48), GFX_RGB(70, 30, 70), GFX_GRADIENT_VERTICAL);

    uint32_t values[] = { 40, 25, 20, 15 };
    gfx_color_t colors[] = { GFX_RGB(230, 90, 70), GFX_RGB(240, 190, 60), GFX_RGB(80, 180, 120), GFX_RGB(80, 140, 230) };
    gfx_draw_pie(s, 60, 70, 40, values, colors, 4);
    gfx_draw_pie_isometric(s, 160, 70, 45, 12, values, colors, 4);

    gfx_draw_circle(s, 260, 70, 35, GFX_COLOR_WHITE);
    gfx_fill_circle(s, 260, 70, 20, GFX_RGB(240, 190, 60));
    gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_fill_rect(s, 230, 60, 60, 25, GFX_ARGB(110, 80, 140, 230));
    gfx_set_blend(s, GFX_BLEND_COPY);

    for (int i = 0; i < 12; i++)
        gfx_draw_line(s, 10, 190, 10 + i * 25, 130, GFX_RGB(100 + i * 12, 200, 255 - i * 10));

    gfx_set_text_scale(s, 2);
    gfx_draw_text(s, 20, 120, "libc + kernel gfx", GFX_COLOR_WHITE);
    gfx_set_text_scale(s, 1);
    gfx_draw_text_opaque(s, 20, 145, " batched draw commands ", GFX_COLOR_BLACK, GFX_RGB(240, 190, 60));
    gfx_draw_rect(s, 0, 0, 320, 200, GFX_COLOR_WHITE);
    CHECK(gfx_flush(s) == 0);

    static uint32_t px[320 * 200];
    CHECK(gfx_read_pixels(s, 0, 0, 320, 200, px, 0) == 0);
    FILE* f = fopen("gfx_test.ppm", "w");
    fprintf(f, "P6\n320 200\n255\n");
    for (int i = 0; i < 320 * 200; i++) {
        unsigned char rgb[3] = { GFX_RED(px[i]), GFX_GREEN(px[i]), GFX_BLUE(px[i]) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    gfx_surface_destroy(s);
}

int main(void) {
    test_basics();
    test_text();
    test_pixels_and_blit();
    test_errors_and_limits();
    test_fuzz();
    demo_picture();
    printf("gfx: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

