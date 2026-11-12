/*
 * Graphics demo: a small animated scene drawn into a kernel surface and
 * presented in the middle of the screen, about 60 frames a second.
 */

#include <stdio.h>
#include <os/gfx.h>
#include <os/os.h>

#define W 320
#define H 200

int main(void) {
    gfx_info_t info;
    if (gfx_get_info(&info) < 0 || info.width == 0) {
        perror("gfx_get_info");
        return 1;
    }

    gfx_surface_t* s = gfx_surface_create(W, H);
    if (s == NULL) {
        perror("gfx_surface_create");
        return 1;
    }

    int32_t screen_x = ((int32_t)info.width - W) / 2;
    int32_t screen_y = ((int32_t)info.height - H) / 2;
    int32_t bx = 40, by = 30, dx = 3, dy = 2;
    uint32_t values[] = { 5, 3, 2 };
    gfx_color_t colors[] = { GFX_RGB(230, 90, 70), GFX_RGB(240, 190, 60), GFX_RGB(80, 140, 230) };

    for (int frame = 0; frame < 600; frame++) {
        gfx_fill_gradient(s, 0, 0, W, H, GFX_RGB(20, 24, 48), GFX_RGB(60, 30, 70), GFX_GRADIENT_VERTICAL);

        // Pie whose slices grow and shrink
        values[0] = 5 + (uint32_t)(frame % 60) / 6;
        gfx_draw_pie_isometric(s, 250, 60, 40, 10, values, colors, 3);

        // Bouncing ball with a translucent shadow
        bx += dx; by += dy;
        if (bx < 12 || bx > W - 12) dx = -dx;
        if (by < 12 || by > H - 40) dy = -dy;
        gfx_set_blend(s, GFX_BLEND_ALPHA);
        gfx_fill_circle(s, bx + 4, by + 4, 12, GFX_ARGB(90, 0, 0, 0));
        gfx_set_blend(s, GFX_BLEND_COPY);
        gfx_fill_circle(s, bx, by, 12, GFX_RGB(80, 200, 140));

        char status[48];
        snprintf(status, sizeof(status), " frame %3d  ball %3d,%3d ", frame, bx, by);
        gfx_draw_text_opaque(s, 8, H - 14, status, GFX_COLOR_BLACK, GFX_RGB(240, 190, 60));
        gfx_draw_rect(s, 0, 0, W, H, GFX_COLOR_WHITE);

        if (gfx_present(s, screen_x, screen_y) < 0) {
            perror("gfx_present");
            break;
        }
        os_sleep_ms(16);
    }

    gfx_surface_destroy(s);
    return 0;
}

