/*
 * Sprite demo: load a sprite resource by path, look it up by name, and draw
 * it with the gfx calls.
 *
 * It loads SPRITE_FILE from the program's working directory (the directory
 * it was started from). Copy any sprite there, for example one of the
 * desktop icons from sys/images, renamed to SPRITE_FILE. If there is none,
 * the demo writes a small one itself (needs SEC_FILE_WRITE for that).
 *
 * Needs SEC_GFX and SEC_FILE_READ; SEC_GFX_SCREEN to show the result when
 * the window manager doesn't take the surface.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <os/gfx.h>
#include <os/os.h>
#include <os/resource.h>

#define SPRITE_FILE "demo_sprite"       // Also its resource name

#define W 320
#define H 200

// Write a 16 x 16 sprite resource (see the kernel's resources/
// resource_manager.h and resources/classes/image.h for the format)
static void put16(FILE* f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }

static int write_demo_sprite(const char* path) {
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
    // Index 0 is opaque black: the format's see-through color
    static const unsigned palette[3] = { 0xFF000000u, 0xFF402010u, 0xFFFFD040u };

    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    unsigned payload = 6 + 2 * (3 * 2 + 16 * 16);
    fwrite("RSRC", 1, 4, f);
    put16(f, 1);                // Format version
    put16(f, 1);                // Type: sprite
    put16(f, payload & 0xFFFF); put16(f, payload >> 16);
    put16(f, 0); put16(f, 0);   // Reserved
    put16(f, 16); put16(f, 16); put16(f, 3);
    for (int i = 0; i < 3; i++) {
        put16(f, palette[i] & 0xFFFF);
        put16(f, palette[i] >> 16);
    }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            put16(f, rows[y][x] == '#' ? 1 : rows[y][x] == 'Y' ? 2 : 0);
    return fclose(f);
}

int main(void) {
    if (resource_load(SPRITE_FILE) < 0) {
        if (errno != ENOENT) {
            perror("sprite_demo: resource_load");
            return 1;
        }
        printf("sprite_demo: no %s here, writing one\n", SPRITE_FILE);
        if (write_demo_sprite(SPRITE_FILE) < 0 || resource_load(SPRITE_FILE) < 0) {
            perror("sprite_demo: " SPRITE_FILE);
            return 1;
        }
    }

    // Loaded by path above; from here on it is known by name
    int sprite = resource_get(SPRITE_FILE);
    resource_info_t info;
    if (sprite < 0 || resource_info(sprite, &info) < 0) {
        perror("sprite_demo: resource_get");
        return 1;
    }

    gfx_info_t screen;
    gfx_surface_t* s = gfx_surface_create(W, H);
    if (gfx_get_info(&screen) < 0 || s == NULL) {
        perror("sprite_demo: gfx");
        return 1;
    }

    int32_t sw = (int32_t)info.width, sh = (int32_t)info.height;
    int32_t bx = 20, by = 60, dx = 3, dy = 2;
    char text[64];

    for (int frame = 0; frame < 300; frame++) {
        gfx_fill_gradient(s, 0, 0, W, H, GFX_RGB(30, 60, 90), GFX_RGB(10, 20, 40), GFX_GRADIENT_VERTICAL);

        // A row of whole copies, and its left half on its own
        for (int i = 0; i < 6; i++)
            gfx_draw_resource(s, sprite, 10 + i * (sw + 6), 10);
        gfx_rect_t left = gfx_rect(0, 0, sw / 2, sh);
        gfx_draw_resource_rect(s, sprite, &left, 10, 20 + sh);

        // One bouncing around
        bx += dx; by += dy;
        if (bx < 0 || bx > W - sw) dx = -dx;
        if (by < 20 + 2 * sh || by > H - sh - 14) dy = -dy;
        gfx_draw_resource(s, sprite, bx, by);

        snprintf(text, sizeof(text), " %s  %ux%u  handle %d ", SPRITE_FILE,
                 (unsigned)info.width, (unsigned)info.height, sprite);
        gfx_draw_text_opaque(s, 4, H - 12, text, GFX_COLOR_BLACK, GFX_RGB(240, 190, 60));
        gfx_draw_rect(s, 0, 0, W, H, GFX_COLOR_WHITE);

        if (gfx_present(s, ((int32_t)screen.width - W) / 2, ((int32_t)screen.height - H) / 2) < 0) {
            perror("sprite_demo: gfx_present");
            break;
        }
        os_sleep_ms(16);
    }

    gfx_surface_destroy(s);
    return 0;
}

