/*
 * Resource tests: os/resource.h and gfx_draw_resource* on PNG files, which
 * os/image.h writes (bitmap_save_png) and reads back (stb_image), drawn
 * through the kernel's own gfx interpreter (gfx_shim.c).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/gfx.h>
#include <os/image.h>
#include <os/resource.h>
#include <os/security.h>

int malloc_check(void);
uint32_t __test_gfx_screen_pixel(int32_t x, int32_t y);

static int failures = 0, checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); } \
} while (0)

#define WHITE   0xFFFFFFFFu
#define KEY     0xFF000000u     // Opaque black: see-through when drawn
#define RED     0xFFFF0000u
#define HGREEN  0x8000FF00u     // Half transparent green
#define BLUE    0xFF0000FFu
#define CLEAR   0x00000000u     // Fully transparent

// A w x h PNG, every pixel `fill` (pixels == NULL) or the given ones;
// `cut` drops that many bytes off the end (a damaged file)
static void write_png(const char* path, int32_t w, int32_t h, const uint32_t* pixels, uint32_t fill, uint32_t cut) {
    uint32_t* px = (uint32_t*)pixels;
    if (px == NULL) {
        px = (uint32_t*)malloc((size_t)w * (size_t)h * 4u);
        for (int32_t i = 0; i < w * h; i++) px[i] = fill;
    }
    if (bitmap_save_png(path, px, w, h, 0) != 0) { printf("cannot write %s\n", path); exit(1); }
    if (px != pixels) free(px);
    if (cut > 0) {
        struct stat st;
        stat(path, &st);
        truncate(path, (off_t)(st.st_size - cut));
    }
}

// 4 x 3:   K R G B
//          R R R R
//          B K 0 B      (0: fully transparent)
static const uint32_t tile[12] = { KEY, RED, HGREEN, BLUE,  RED, RED, RED, RED,  BLUE, KEY, CLEAR, BLUE };

static void test_image(void) {
    mkdir("res", 0755);
    write_png("res/round.png", 4, 3, tile, 0, 0);
    bitmap_t b;
    CHECK(bitmap_load("res/round.png", &b) == 0 && b.width == 4 && b.height == 3);
    CHECK(b.pixels != NULL && memcmp(b.pixels, tile, sizeof(tile)) == 0);
    bitmap_free(&b);
    CHECK(b.pixels == NULL && b.width == 0);

    // A stride wider than the image: only width pixels of each row
    uint32_t wide[2 * 5] = { RED, BLUE, 1, 2, 3,  BLUE, RED, 4, 5, 6 };
    CHECK(bitmap_save_png("res/stride.png", wide, 2, 2, 5) == 0);
    CHECK(bitmap_load("res/stride.png", &b) == 0 && b.width == 2 && b.height == 2 &&
          b.pixels[0] == RED && b.pixels[1] == BLUE && b.pixels[2] == BLUE && b.pixels[3] == RED);
    bitmap_free(&b);

    CHECK(bitmap_save_png("res/bad.png", wide, 0, 2, 0) == -1 && errno == EINVAL);
    CHECK(bitmap_save_png("res/bad.png", wide, 5, 2, 2) == -1 && errno == EINVAL);
    CHECK(bitmap_load("res/missing.png", &b) == -1 && errno == ENOENT);
}

static void test_load_and_get(void) {
    mkdir("res/other", 0755);
    write_png("res/tile.png", 4, 3, tile, 0, 0);

    CHECK(resource_get("tile") == -1 && errno == ENOENT);
    CHECK(resource_load("res/tile.png") == 0);
    int h = resource_get("tile");                           // The extension is not part of the name
    CHECK(h >= 1);

    resource_info_t info;
    CHECK(resource_info(h, &info) == 0 && info.type == RESOURCE_IMAGE && info.width == 4 && info.height == 3);
    CHECK(resource_info(h + 1, &info) == -1 && errno == EBADF);
    CHECK(resource_info(h, NULL) == -1 && errno == EFAULT);
    CHECK(resource_pixels(h) != NULL && resource_pixels(h)[1] == RED && resource_pixels(h + 1) == NULL);

    // Same file again: fine, same handle. Same name, other file: EEXIST.
    CHECK(resource_load("res/tile.png") == 0 && resource_get("tile") == h);
    write_png("res/other/tile.png", 2, 2, NULL, RED, 0);
    CHECK(resource_load("res/other/tile.png") == -1 && errno == EEXIST);
    write_png("res/other/tile", 2, 2, NULL, RED, 0);       // No extension: the same name
    CHECK(resource_load("res/other/tile") == -1 && errno == EEXIST);
    CHECK(resource_open("res/tile.png") == h);

    // Only a picture's extension comes off
    write_png("res/icon.v2", 2, 2, NULL, RED, 0);
    CHECK(resource_load("res/icon.v2") == 0 && resource_get("icon.v2") > 0 && resource_get("icon") == -1);
    CHECK(resource_get("res/tile") == -1 && errno == ENOENT);
    CHECK(resource_get("") == -1 && errno == EINVAL);
    char long_name[80];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(resource_get(long_name) == -1 && errno == ENAMETOOLONG);
    char long_path[100] = "res/";
    strcat(long_path, long_name);
    write_png(long_path, 1, 1, NULL, RED, 0);
    CHECK(resource_load(long_path) == -1 && errno == ENAMETOOLONG);
    CHECK(resource_load(NULL) == -1 && errno == EFAULT);
}

static void test_bad_files(void) {
    CHECK(resource_load("res/missing.png") == -1 && errno == ENOENT);
    CHECK(resource_load("res/other") == -1 && errno == EISDIR);

    FILE* f = fopen("res/notes.txt", "w");
    fputs("not an image at all, just some text", f);
    fclose(f);
    CHECK(resource_load("res/notes.txt") == -1 && errno == ENOEXEC);

    // The old sprite resource format is not an image any more
    f = fopen("res/old_sprite", "w");
    fputs("RSRC\x01\x00\x01\x00", f);
    fclose(f);
    CHECK(resource_load("res/old_sprite") == -1 && errno == ENOEXEC);

    write_png("res/cut.png", 4, 3, tile, 0, 12);
    CHECK(resource_load("res/cut.png") == -1 && errno == ENOEXEC);

    // A valid picture, but wider than any surface may be
    write_png("res/wide.png", 5000, 1, NULL, RED, 0);
    CHECK(resource_load("res/wide.png") == -1 && errno == ENOEXEC);

    // Nothing above was registered
    CHECK(resource_get("notes") == -1 && resource_get("wide") == -1 && resource_get("cut") == -1 &&
          resource_get("old_sprite") == -1);
}

static void test_draw(void) {
    int h = resource_get("tile");
    gfx_surface_t* s = gfx_surface_create(20, 10);
    CHECK(s != NULL && h > 0);

    gfx_clear(s, WHITE);
    gfx_draw_resource(s, h, 2, 1);
    CHECK(gfx_flush(s) == 0);

    // Row 0: K R G B at x = 2..5
    CHECK(gfx_get_pixel(s, 2, 1) == WHITE);                 // Key: see-through
    CHECK(gfx_get_pixel(s, 3, 1) == RED);
    uint32_t g = gfx_get_pixel(s, 4, 1);                    // Half green over white
    CHECK(GFX_GREEN(g) == 0xFF && GFX_RED(g) > 0x60 && GFX_RED(g) < 0xA0 && GFX_ALPHA(g) == 0xFF);
    CHECK(gfx_get_pixel(s, 5, 1) == BLUE);
    // Row 1 all red, row 2: B K 0 B (both see-through)
    for (int x = 2; x < 6; x++)
        CHECK(gfx_get_pixel(s, x, 2) == RED);
    CHECK(gfx_get_pixel(s, 2, 3) == BLUE && gfx_get_pixel(s, 3, 3) == WHITE &&
          gfx_get_pixel(s, 4, 3) == WHITE && gfx_get_pixel(s, 5, 3) == BLUE);
    // Nothing outside it
    CHECK(gfx_get_pixel(s, 1, 1) == WHITE && gfx_get_pixel(s, 6, 1) == WHITE && gfx_get_pixel(s, 2, 4) == WHITE);

    // The surface's own blend mode is untouched (COPY): a fill with a
    // translucent color still replaces
    gfx_fill_rect(s, 10, 0, 1, 1, 0x40112233);
    CHECK(gfx_get_pixel(s, 10, 0) == 0x40112233);

    // Part of it, clipped by the surface edge, through origin and clip
    gfx_clear(s, WHITE);
    gfx_rect_t part = gfx_rect(1, 1, 3, 2);                 // R R R / K 0 B
    gfx_draw_resource_rect(s, h, &part, 18, 8);             // Only 2 x 2 of it fits
    CHECK(gfx_flush(s) == 0);
    CHECK(gfx_get_pixel(s, 18, 8) == RED && gfx_get_pixel(s, 19, 8) == RED);
    CHECK(gfx_get_pixel(s, 18, 9) == WHITE && gfx_get_pixel(s, 19, 9) == WHITE);
    gfx_draw_resource_rect(s, h, NULL, 0, 0);
    gfx_set_origin(s, 10, 0);
    gfx_rect_t clip = gfx_rect(0, 0, 2, 10);
    gfx_set_clip(s, &clip);
    gfx_draw_resource(s, h, 0, 0);
    gfx_set_clip(s, NULL);
    gfx_set_origin(s, 0, 0);
    CHECK(gfx_flush(s) == 0);
    CHECK(gfx_get_pixel(s, 1, 0) == RED && gfx_get_pixel(s, 11, 0) == RED && gfx_get_pixel(s, 12, 1) == WHITE);

    // A bad handle is reported by the next flush; the other draws still happen
    gfx_clear(s, WHITE);
    gfx_draw_resource(s, 63, 0, 0);
    gfx_draw_resource(s, h, 0, 0);
    CHECK(gfx_flush(s) == -1 && errno == EBADF);
    CHECK(gfx_get_pixel(s, 1, 0) == RED);

    // Onto the screen
    gfx_clear(s, WHITE);
    gfx_draw_resource(s, h, 0, 0);
    CHECK(gfx_present(s, 100, 50) == 0);
    CHECK(__test_gfx_screen_pixel(101, 50) == RED && __test_gfx_screen_pixel(100, 50) == WHITE &&
          __test_gfx_screen_pixel(103, 50) == BLUE);

    gfx_surface_destroy(s);
}

static void test_limits_and_security(void) {
    // Loading reads the file, so it needs file read clearance; what is
    // loaded is this process's own memory
    write_png("res/late.png", 1, 1, NULL, RED, 0);
    CHECK(sec_set(SEC_ALL & ~SEC_FILE_READ) == 0);
    CHECK(resource_load("res/late.png") == -1 && errno == EPERM);
    CHECK(resource_get("tile") > 0);
    CHECK(sec_set(SEC_ALL) == 0);

    // Fill the table: RESOURCE_MAX in all
    int n = 0;
    char path[32];
    errno = 0;
    for (int i = 0; i < RESOURCE_MAX + 2; i++) {
        snprintf(path, sizeof(path), "res/s%02d.png", i);
        write_png(path, 1, 1, NULL, RED, 0);
        if (resource_load(path) == 0) n++;
        else break;
    }
    CHECK(errno == EMFILE);
    CHECK(resource_get("s00") > 0 && resource_get("tile") > 0);
    CHECK(resource_load("res/tile.png") == 0);              // Already loaded is still fine

    (void)n;
}

int main(void) {
    test_image();
    test_load_and_get();
    test_bad_files();
    test_draw();
    test_limits_and_security();
    CHECK(malloc_check() == 0);
    printf("resources: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

