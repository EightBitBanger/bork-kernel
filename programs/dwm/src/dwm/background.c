/*
 * Desktop background (background.h).
 *
 * Two buffers while a wallpaper is set:
 *
 *   source     the caller's pixels, copied (kept so the layout can be done
 *              again when the color or the style changes)
 *   backdrop   screen-sized: the color, with the wallpaper laid out on it
 *              and blended in. Fully opaque.
 *
 * Painting a dirty rectangle draws that rectangle of the backdrop with
 * gfx_draw_pixels (its stride is the screen's width), so a repaint costs
 * one copy whatever the style. Laying out happens only on a change.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "background.h"
#include "compositor.h"
#include "theme.h"

static uint32_t*             source   = NULL;
static int32_t               source_w = 0, source_h = 0;
static uint32_t*             backdrop = NULL;   // compositor_width() x compositor_height()
static int32_t               backdrop_w = 0, backdrop_h = 0;
static dwm_wallpaper_style_t style    = DWM_WALLPAPER_FILL;

//
// Layout
//

// `over` (0xAARRGGBB) blended onto opaque `under`; the result is opaque
static inline uint32_t blend(uint32_t over, uint32_t under) {
    uint32_t a = over >> 24;
    if (a == 0xFF)
        return over;
    if (a == 0)
        return under;
    uint32_t na = 255 - a;
    uint32_t rb = ((over & 0x00FF00FFu) * a + (under & 0x00FF00FFu) * na) >> 8;
    uint32_t g  = ((over & 0x0000FF00u) * a + (under & 0x0000FF00u) * na) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

// The source scaled (nearest pixel) to dw x dh with its corner at (dx, dy),
// cut to the backdrop, blended onto what is there
static void place_scaled(int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
    if (dw <= 0 || dh <= 0)
        return;
    int32_t x0 = dx < 0 ? 0 : dx;
    int32_t y0 = dy < 0 ? 0 : dy;
    int32_t x1 = dx + dw > backdrop_w ? backdrop_w : dx + dw;
    int32_t y1 = dy + dh > backdrop_h ? backdrop_h : dy + dh;
    if (x0 >= x1 || y0 >= y1)
        return;

    // 16.16 steps through the source, sampling each pixel's middle
    uint32_t step_x = (uint32_t)(((uint64_t)source_w << 16) / (uint32_t)dw);
    uint32_t step_y = (uint32_t)(((uint64_t)source_h << 16) / (uint32_t)dh);
    uint32_t start_x = (uint32_t)(x0 - dx) * step_x + step_x / 2;
    uint32_t fy = (uint32_t)(y0 - dy) * step_y + step_y / 2;

    for (int32_t y = y0; y < y1; y++, fy += step_y) {
        int32_t sy = (int32_t)(fy >> 16);
        if (sy >= source_h) sy = source_h - 1;
        const uint32_t* src = source + (size_t)sy * (size_t)source_w;
        uint32_t* dst = backdrop + (size_t)y * (size_t)backdrop_w;
        uint32_t fx = start_x;
        for (int32_t x = x0; x < x1; x++, fx += step_x) {
            int32_t sx = (int32_t)(fx >> 16);
            if (sx >= source_w) sx = source_w - 1;
            dst[x] = blend(src[sx], dst[x]);
        }
    }
}

// The source at its own size with its corner at (dx, dy), cut to the backdrop
static void place(int32_t dx, int32_t dy) {
    int32_t x0 = dx < 0 ? 0 : dx;
    int32_t y0 = dy < 0 ? 0 : dy;
    int32_t x1 = dx + source_w > backdrop_w ? backdrop_w : dx + source_w;
    int32_t y1 = dy + source_h > backdrop_h ? backdrop_h : dy + source_h;
    for (int32_t y = y0; y < y1; y++) {
        const uint32_t* src = source + (size_t)(y - dy) * (size_t)source_w + (x0 - dx);
        uint32_t* dst = backdrop + (size_t)y * (size_t)backdrop_w;
        for (int32_t x = x0; x < x1; x++)
            dst[x] = blend(*src++, dst[x]);
    }
}

// Fill the backdrop: the color, then the source laid out by `style`
static void layout(void) {
    if (backdrop == NULL || source == NULL)
        return;
    uint32_t color = dwm_theme()->desktop.background | 0xFF000000u;
    size_t total = (size_t)backdrop_w * (size_t)backdrop_h;
    for (size_t i = 0; i < total; i++)
        backdrop[i] = color;

    int32_t sw = backdrop_w, sh = backdrop_h;
    int32_t w = source_w, h = source_h;
    switch (style) {
        case DWM_WALLPAPER_CENTER:
            place((sw - w) / 2, (sh - h) / 2);
            break;

        case DWM_WALLPAPER_TILE:
            for (int32_t y = 0; y < sh; y += h)
                for (int32_t x = 0; x < sw; x += w)
                    place(x, y);
            break;

        case DWM_WALLPAPER_STRETCH:
            place_scaled(0, 0, sw, sh);
            break;

        case DWM_WALLPAPER_FIT:
        case DWM_WALLPAPER_FILL: {
            // Compare sw / w with sh / h without dividing: which side limits
            bool width_limits = (int64_t)sw * h <= (int64_t)sh * w;
            if (style == DWM_WALLPAPER_FILL)
                width_limits = !width_limits;
            int32_t dw, dh;
            if (width_limits) {
                dw = sw;
                dh = (int32_t)(((int64_t)h * sw + w / 2) / w);
            } else {
                dh = sh;
                dw = (int32_t)(((int64_t)w * sh + h / 2) / h);
            }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            place_scaled((sw - dw) / 2, (sh - dh) / 2, dw, dh);
            break;
        }
    }
}

//
// Public
//

int dwm_set_wallpaper(const uint32_t* pixels, int32_t width, int32_t height,
                      dwm_wallpaper_style_t new_style) {
    if (pixels == NULL || width <= 0 || height <= 0 ||
        width > DWM_WALLPAPER_MAX_SIDE || height > DWM_WALLPAPER_MAX_SIDE ||
        (uint32_t)new_style > (uint32_t)DWM_WALLPAPER_FILL) {
        errno = EINVAL;
        return -1;
    }
    int32_t bw = compositor_width(), bh = compositor_height();
    if (bw <= 0 || bh <= 0) {
        errno = EINVAL;         // No screen yet (before dwm_init)
        return -1;
    }

    size_t bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
    uint32_t* copy = (uint32_t*)malloc(bytes);
    if (copy == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(copy, pixels, bytes);

    if (backdrop == NULL || backdrop_w != bw || backdrop_h != bh) {
        uint32_t* b = (uint32_t*)malloc((size_t)bw * (size_t)bh * sizeof(uint32_t));
        if (b == NULL) {
            free(copy);
            errno = ENOMEM;
            return -1;
        }
        free(backdrop);
        backdrop = b;
        backdrop_w = bw;
        backdrop_h = bh;
    }

    free(source);
    source   = copy;
    source_w = width;
    source_h = height;
    style    = new_style;
    layout();
    compositor_invalidate_all();
    return 0;
}

void dwm_set_wallpaper_style(dwm_wallpaper_style_t new_style) {
    if ((uint32_t)new_style > (uint32_t)DWM_WALLPAPER_FILL || new_style == style)
        return;
    style = new_style;
    if (source != NULL) {
        layout();
        compositor_invalidate_all();
    }
}

void dwm_clear_wallpaper(void) {
    if (source == NULL)
        return;
    background_shutdown();
    compositor_invalidate_all();
}

bool dwm_has_wallpaper(void) {
    return source != NULL;
}

//
// Internal
//

void background_paint(gfx_surface_t* screen, gfx_rect_t area) {
    if (backdrop == NULL || source == NULL) {
        gfx_clear(screen, dwm_theme()->desktop.background);
        return;
    }
    // Cut to the backdrop (it is the screen's size, so this only guards)
    int32_t x0 = area.x < 0 ? 0 : area.x;
    int32_t y0 = area.y < 0 ? 0 : area.y;
    int32_t x1 = area.x + area.w > backdrop_w ? backdrop_w : area.x + area.w;
    int32_t y1 = area.y + area.h > backdrop_h ? backdrop_h : area.y + area.h;
    if (x0 >= x1 || y0 >= y1)
        return;
    gfx_blend_t old = gfx_set_blend(screen, GFX_BLEND_COPY);
    gfx_draw_pixels(screen, backdrop + (size_t)y0 * (size_t)backdrop_w + x0,
                    x1 - x0, y1 - y0, backdrop_w, x0, y0);
    gfx_set_blend(screen, old);
}

void background_color_changed(void) {
    if (source != NULL) {
        layout();
        compositor_invalidate_all();
    }
}

void background_shutdown(void) {
    free(source);
    free(backdrop);
    source = backdrop = NULL;
    source_w = source_h = backdrop_w = backdrop_h = 0;
}
