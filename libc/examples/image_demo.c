/*
 * Image demo: load an image file as a resource by path, look it up by
 * name, and draw it with the gfx calls.
 *
 * It loads IMAGE_FILE from the program's working directory (the directory
 * it was started from). Copy any picture there, for example one of the
 * desktop icons from sys/images, renamed to IMAGE_FILE. If there is none,
 * the demo draws a small one and saves it as a PNG (os/image.h; needs
 * SEC_FILE_WRITE for that).
 *
 * Needs SEC_GFX and SEC_FILE_READ; SEC_GFX_SCREEN to show the result when
 * the window manager doesn't take the surface.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <os/gfx.h>
#include <os/image.h>
#include <os/os.h>
#include <os/resource.h>

#define IMAGE_FILE  "demo_image.png"
#define IMAGE_NAME  "demo_image"        // Its resource name: the extension left out

#define W 320
#define H 200

// Draw a 16 x 16 face and save it as a PNG
static int write_demo_image(const char* path) {
    static const char* rows[16] = {
        "................",
        ".....######.....",
        "...##YYYYYY##...",
        "..#YYYYYYYYYY#..",
        ".#YYYYYYYYYYYY#.",
        ".#YYY##YY##YYY#.",
        "#YYYY##YY##YYYY#",
        "#YYYYYYYYYYYYYY#",
        "#YYYYYYYYYYYYYY#",
        "#YY#YYYYYYYY#YY#",
        ".#YY#YYYYYY#YY#.",
        ".#YYY######YYY#.",
        "..#YYYYYYYYYY#..",
        "...##YYYYYY##...",
        ".....######.....",
        "................",
    };
    // Opaque black is see-through when drawn the way the dwm draws images
    uint32_t px[16 * 16];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            px[y * 16 + x] = rows[y][x] == '#' ? 0xFF402010u : rows[y][x] == 'Y' ? 0xFFFFD040u : 0xFF000000u;
    return bitmap_save_png(path, px, 16, 16, 0);
}

int main(void) {
    if (resource_load(IMAGE_FILE) < 0) {
        if (errno != ENOENT) {
            perror("image_demo: resource_load");
            return 1;
        }
        printf("image_demo: no %s here, writing one\n", IMAGE_FILE);
        if (write_demo_image(IMAGE_FILE) < 0 || resource_load(IMAGE_FILE) < 0) {
            perror("image_demo: " IMAGE_FILE);
            return 1;
        }
    }

    // Loaded by path above; from here on it is known by name
    int image = resource_get(IMAGE_NAME);
    resource_info_t info;
    if (image < 0 || resource_info(image, &info) < 0) {
        perror("image_demo: resource_get");
        return 1;
    }

    gfx_info_t screen;
    gfx_surface_t* s = gfx_surface_create(W, H);
    if (gfx_get_info(&screen) < 0 || s == NULL) {
        perror("image_demo: gfx");
        return 1;
    }

    int32_t sw = (int32_t)info.width, sh = (int32_t)info.height;
    int32_t bx = 20, by = 60, dx = 3, dy = 2;
    char text[64];

    for (int frame = 0; frame < 300; frame++) {
        gfx_fill_gradient(s, 0, 0, W, H, GFX_RGB(30, 60, 90), GFX_RGB(10, 20, 40), GFX_GRADIENT_VERTICAL);

        // A row of whole copies, and its left half on its own
        for (int i = 0; i < 6; i++)
            gfx_draw_resource(s, image, 10 + i * (sw + 6), 10);
        gfx_rect_t left = gfx_rect(0, 0, sw / 2, sh);
        gfx_draw_resource_rect(s, image, &left, 10, 20 + sh);

        // One bouncing around
        bx += dx; by += dy;
        if (bx < 0 || bx > W - sw) dx = -dx;
        if (by < 20 + 2 * sh || by > H - sh - 14) dy = -dy;
        gfx_draw_resource(s, image, bx, by);

        snprintf(text, sizeof(text), " %s  %ux%u  handle %d ", IMAGE_NAME,
                 (unsigned)info.width, (unsigned)info.height, image);
        gfx_draw_text_opaque(s, 4, H - 12, text, GFX_COLOR_BLACK, GFX_RGB(240, 190, 60));
        gfx_draw_rect(s, 0, 0, W, H, GFX_COLOR_WHITE);

        if (gfx_present(s, ((int32_t)screen.width - W) / 2, ((int32_t)screen.height - H) / 2) < 0) {
            perror("image_demo: gfx_present");
            break;
        }
        os_sleep_ms(16);
    }

    gfx_surface_destroy(s);
    return 0;
}

