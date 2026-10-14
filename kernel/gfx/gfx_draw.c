// Shapes, gradients, charts and blits

#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_internal.h>

#include <kernel/util/math.h>
#include <kernel/util/string.h>

//
// Pixels
//

void gfx_clear(GfxContext* ctx, gfx_color_t color) {
    GfxRect clip;
    if (!gfx_device_clip(ctx, &clip)) return;

    for (int32_t y = clip.y; y < clip.y + clip.h; y++)
        gfx_row_fill(gfx_row(ctx->target, clip.x, y), color, (size_t)clip.w);
}

void gfx_set_pixel(GfxContext* ctx, int32_t x, int32_t y, gfx_color_t color) {
    GfxRect r;
    if (gfx_device_rect(ctx, x, y, 1, 1, &r))
        gfx_plot(ctx, r.x, r.y, color);
}

gfx_color_t gfx_get_pixel(const GfxContext* ctx, int32_t x, int32_t y) {
    if (ctx == NULL || ctx->target == NULL) return 0;
    const uint32_t* p = gfx_surface_pixel(ctx->target, x + ctx->state.origin_x, y + ctx->state.origin_y);
    return (p != NULL) ? *p : 0;
}

//
// Rectangles
//

void gfx_fill_rect(GfxContext* ctx, int32_t x, int32_t y, int32_t w, int32_t h, gfx_color_t color) {
    GfxRect r;
    if (!gfx_device_rect(ctx, x, y, w, h, &r)) return;

    bool blend = (ctx->state.blend == GFX_BLEND_ALPHA) && GFX_ALPHA(color) != 0xFF;
    if (blend && GFX_ALPHA(color) == 0) return;

    for (int32_t row = r.y; row < r.y + r.h; row++) {
        uint32_t* dest = gfx_row(ctx->target, r.x, row);
        if (blend) gfx_row_blend(dest, color, (size_t)r.w);
        else       gfx_row_fill(dest, color, (size_t)r.w);
    }
}

void gfx_draw_hline(GfxContext* ctx, int32_t x, int32_t y, int32_t w, gfx_color_t color) {
    gfx_fill_rect(ctx, x, y, w, 1, color);
}

void gfx_draw_vline(GfxContext* ctx, int32_t x, int32_t y, int32_t h, gfx_color_t color) {
    gfx_fill_rect(ctx, x, y, 1, h, color);
}

void gfx_draw_rect(GfxContext* ctx, int32_t x, int32_t y, int32_t w, int32_t h, gfx_color_t color) {
    if (w <= 0 || h <= 0) return;

    gfx_draw_hline(ctx, x, y, w, color);
    if (h > 1) gfx_draw_hline(ctx, x, y + h - 1, w, color);
    if (h > 2) {
        gfx_draw_vline(ctx, x, y + 1, h - 2, color);
        if (w > 1) gfx_draw_vline(ctx, x + w - 1, y + 1, h - 2, color);
    }
}

// Channels as 16.16 fixed point, stepped along the gradient
typedef struct {
    int32_t a, r, g, b;
} GfxColorStep;

static inline void gfx_step_init(GfxColorStep* cur, GfxColorStep* step,
                                 gfx_color_t from, gfx_color_t to, int32_t length, int32_t skip) {
    int32_t div = (length > 1) ? length - 1 : 1;

    // Multiply, not shift: the deltas can be negative
    step->a = (((int32_t)GFX_ALPHA(to) - (int32_t)GFX_ALPHA(from)) * 65536) / div;
    step->r = (((int32_t)GFX_RED(to)   - (int32_t)GFX_RED(from))   * 65536) / div;
    step->g = (((int32_t)GFX_GREEN(to) - (int32_t)GFX_GREEN(from)) * 65536) / div;
    step->b = (((int32_t)GFX_BLUE(to)  - (int32_t)GFX_BLUE(from))  * 65536) / div;

    cur->a = (int32_t)GFX_ALPHA(from) * 65536 + step->a * skip;
    cur->r = (int32_t)GFX_RED(from)   * 65536 + step->r * skip;
    cur->g = (int32_t)GFX_GREEN(from) * 65536 + step->g * skip;
    cur->b = (int32_t)GFX_BLUE(from)  * 65536 + step->b * skip;
}

static inline gfx_color_t gfx_step_color(const GfxColorStep* c) {
    return GFX_ARGB(c->a >> 16, c->r >> 16, c->g >> 16, c->b >> 16);
}

static inline void gfx_step_next(GfxColorStep* c, const GfxColorStep* s) {
    c->a += s->a; c->r += s->r; c->g += s->g; c->b += s->b;
}

void gfx_fill_gradient(GfxContext* ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                       gfx_color_t from, gfx_color_t to, GfxGradient direction) {
    GfxRect r;
    if (!gfx_device_rect(ctx, x, y, w, h, &r)) return;

    bool blend = (ctx->state.blend == GFX_BLEND_ALPHA);
    GfxColorStep cur, step;

    if (direction == GFX_GRADIENT_VERTICAL) {
        // Start where the clipped area begins, not at the top of the rect
        gfx_step_init(&cur, &step, from, to, h, r.y - (y + ctx->state.origin_y));

        for (int32_t row = r.y; row < r.y + r.h; row++) {
            uint32_t* dest = gfx_row(ctx->target, r.x, row);
            gfx_color_t color = gfx_step_color(&cur);
            if (blend) gfx_row_blend(dest, color, (size_t)r.w);
            else       gfx_row_fill(dest, color, (size_t)r.w);
            gfx_step_next(&cur, &step);
        }
        return;
    }

    GfxColorStep start;
    gfx_step_init(&start, &step, from, to, w, r.x - (x + ctx->state.origin_x));

    for (int32_t row = r.y; row < r.y + r.h; row++) {
        uint32_t* dest = gfx_row(ctx->target, r.x, row);
        cur = start;
        for (int32_t i = 0; i < r.w; i++) {
            gfx_color_t color = gfx_step_color(&cur);
            dest[i] = blend ? gfx_blend_pixel(color, dest[i]) : color;
            gfx_step_next(&cur, &step);
        }
    }
}

//
// Lines (Cohen-Sutherland clip, then Bresenham)
//

#define GFX_OUT_LEFT    1
#define GFX_OUT_RIGHT   2
#define GFX_OUT_TOP     4
#define GFX_OUT_BOTTOM  8

static inline int gfx_outcode(const GfxRect* c, int32_t x, int32_t y) {
    int code = 0;
    if (x < c->x)              code |= GFX_OUT_LEFT;
    else if (x >= c->x + c->w) code |= GFX_OUT_RIGHT;
    if (y < c->y)              code |= GFX_OUT_TOP;
    else if (y >= c->y + c->h) code |= GFX_OUT_BOTTOM;
    return code;
}

void gfx_draw_line(GfxContext* ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, gfx_color_t color) {
    GfxRect c;
    if (!gfx_device_clip(ctx, &c)) return;

    x0 += ctx->state.origin_x; y0 += ctx->state.origin_y;
    x1 += ctx->state.origin_x; y1 += ctx->state.origin_y;

    int code0 = gfx_outcode(&c, x0, y0);
    int code1 = gfx_outcode(&c, x1, y1);
    int32_t max_x = c.x + c.w - 1, max_y = c.y + c.h - 1;

    while (code0 | code1) {
        if (code0 & code1) return;      // Entirely outside

        int out = code0 ? code0 : code1;
        int32_t x = 0, y = 0;

        if (out & GFX_OUT_TOP) {
            x = x0 + (x1 - x0) * (c.y - y0) / (y1 - y0);   y = c.y;
        } else if (out & GFX_OUT_BOTTOM) {
            x = x0 + (x1 - x0) * (max_y - y0) / (y1 - y0); y = max_y;
        } else if (out & GFX_OUT_RIGHT) {
            y = y0 + (y1 - y0) * (max_x - x0) / (x1 - x0); x = max_x;
        } else {
            y = y0 + (y1 - y0) * (c.x - x0) / (x1 - x0);   x = c.x;
        }

        if (out == code0) { x0 = x; y0 = y; code0 = gfx_outcode(&c, x0, y0); }
        else              { x1 = x; y1 = y; code1 = gfx_outcode(&c, x1, y1); }
    }

    int32_t dx = abs(x1 - x0), dy = abs(y1 - y0);
    int32_t sx = (x0 < x1) ? 1 : -1, sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx - dy;

    while (1) {
        gfx_plot(ctx, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

//
// Circles
//

static inline void gfx_plot_clipped(const GfxContext* ctx, const GfxRect* clip, int32_t x, int32_t y, gfx_color_t color) {
    if (gfx_rect_contains(clip, x, y))
        gfx_plot(ctx, x, y, color);
}

void gfx_draw_circle(GfxContext* ctx, int32_t cx, int32_t cy, int32_t radius, gfx_color_t color) {
    GfxRect c;
    if (radius < 0 || !gfx_device_clip(ctx, &c)) return;

    cx += ctx->state.origin_x;
    cy += ctx->state.origin_y;

    int32_t x = 0, y = radius, d = 3 - 2 * radius;
    while (y >= x) {
        gfx_plot_clipped(ctx, &c, cx + x, cy + y, color);
        gfx_plot_clipped(ctx, &c, cx - x, cy + y, color);
        gfx_plot_clipped(ctx, &c, cx + x, cy - y, color);
        gfx_plot_clipped(ctx, &c, cx - x, cy - y, color);
        gfx_plot_clipped(ctx, &c, cx + y, cy + x, color);
        gfx_plot_clipped(ctx, &c, cx - y, cy + x, color);
        gfx_plot_clipped(ctx, &c, cx + y, cy - x, color);
        gfx_plot_clipped(ctx, &c, cx - y, cy - x, color);
        x++;
        if (d > 0) { y--; d += 4 * (x - y) + 10; }
        else       { d += 4 * x + 6; }
    }
}

void gfx_fill_circle(GfxContext* ctx, int32_t cx, int32_t cy, int32_t radius, gfx_color_t color) {
    if (radius < 0) return;

    // One span per row: no pixel is covered twice (matters for ALPHA)
    int32_t r2 = radius * radius;
    int32_t span = 0;
    for (int32_t dy = -radius; dy <= radius; dy++) {
        // Half width of this row; grows to the middle row, then shrinks
        while ((span + 1) * (span + 1) + dy * dy <= r2) span++;
        while (span > 0 && span * span + dy * dy > r2) span--;
        gfx_fill_rect(ctx, cx - span, cy + dy, span * 2 + 1, 1, color);
    }
}

//
// Pie charts
//

// Cumulative slice ends as angles in [0, 65536], the scale iatan2 returns
static size_t gfx_pie_thresholds(const uint32_t* values, size_t count, uint32_t* out) {
    if (values == NULL || count == 0) return 0;
    if (count > GFX_PIE_MAX_SLICES) count = GFX_PIE_MAX_SLICES;

    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) total += values[i];
    if (total == 0) return 0;

    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += values[i];
        out[i] = (uint32_t)((sum << 16) / total);
    }
    return count;
}

static inline gfx_color_t gfx_pie_color(uint32_t angle, const uint32_t* thresholds,
                                        const gfx_color_t* colors, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (angle <= thresholds[i]) return colors[i];
    }
    return colors[count - 1];
}

void gfx_draw_pie(GfxContext* ctx, int32_t cx, int32_t cy, int32_t radius,
                  const uint32_t* values, const gfx_color_t* colors, size_t count) {
    uint32_t thresholds[GFX_PIE_MAX_SLICES];
    if (colors == NULL || radius <= 0) return;
    count = gfx_pie_thresholds(values, count, thresholds);
    if (count == 0) return;

    GfxRect r;
    if (!gfx_device_rect(ctx, cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1, &r)) return;

    int32_t xc = cx + ctx->state.origin_x, yc = cy + ctx->state.origin_y;
    int32_t r2 = radius * radius;

    for (int32_t py = r.y; py < r.y + r.h; py++) {
        int32_t dy = py - yc;
        for (int32_t px = r.x; px < r.x + r.w; px++) {
            int32_t dx = px - xc;
            if (dx * dx + dy * dy >= r2) continue;
            gfx_plot(ctx, px, py, gfx_pie_color(iatan2(dx, -dy), thresholds, colors, count));
        }
    }
}

void gfx_draw_pie_isometric(GfxContext* ctx, int32_t cx, int32_t cy, int32_t radius, int32_t depth,
                            const uint32_t* values, const gfx_color_t* colors, size_t count) {
    uint32_t thresholds[GFX_PIE_MAX_SLICES];
    if (colors == NULL || radius <= 0) return;
    if (depth < 0) depth = 0;
    count = gfx_pie_thresholds(values, count, thresholds);
    if (count == 0) return;

    int32_t rx = radius, ry = radius / 2;
    GfxRect r;
    if (!gfx_device_rect(ctx, cx - rx, cy - ry, rx * 2 + 1, ry * 2 + depth + 1, &r)) return;

    int32_t xc = cx + ctx->state.origin_x, yc = cy + ctx->state.origin_y;
    int32_t r2 = rx * rx;

    for (int32_t py = r.y; py < r.y + r.h; py++) {
        for (int32_t px = r.x; px < r.x + r.w; px++) {
            int32_t dx = px - xc;
            int32_t z = -1, dy = 0;

            // Top face first, then the side, one slice of depth at a time
            for (int32_t layer = 0; layer <= depth; layer++) {
                int32_t d = py - (yc + layer);
                if (layer > 0 && py <= yc) break;
                if (dx * dx + 4 * d * d < r2) { z = layer; dy = d; break; }
            }
            if (z < 0) continue;

            gfx_color_t color = gfx_pie_color(iatan2(dx, -(dy * 2)), thresholds, colors, count);
            gfx_plot(ctx, px, py, (z > 0) ? gfx_color_darken(color) : color);
        }
    }
}

//
// Blits
//

static void gfx_blit_internal(GfxContext* ctx, const GfxSurface* src, const GfxRect* src_rect,
                              int32_t x, int32_t y, bool keyed, gfx_color_t key) {
    if (src == NULL || src->pixels == NULL) return;

    // Source area, cut to the source
    GfxRect area = gfx_surface_bounds(src);
    if (src_rect != NULL && !gfx_rect_intersect(src_rect, &area, &area)) return;

    // Where the area's corner lands after cutting it to the source
    int32_t dest_x = x + (area.x - (src_rect ? src_rect->x : 0));
    int32_t dest_y = y + (area.y - (src_rect ? src_rect->y : 0));

    GfxRect d;
    if (!gfx_device_rect(ctx, dest_x, dest_y, area.w, area.h, &d)) return;

    // Matching source corner for the clipped destination
    int32_t sx = area.x + (d.x - (dest_x + ctx->state.origin_x));
    int32_t sy = area.y + (d.y - (dest_y + ctx->state.origin_y));
    bool blend = (ctx->state.blend == GFX_BLEND_ALPHA);

    for (int32_t row = 0; row < d.h; row++) {
        uint32_t*       dest = gfx_row(ctx->target, d.x, d.y + row);
        const uint32_t* from = gfx_row(src, sx, sy + row);

        if (!keyed && !blend) {
            gfx_row_copy(dest, from, (size_t)d.w);
            continue;
        }
        for (int32_t i = 0; i < d.w; i++) {
            gfx_color_t c = from[i];
            if (keyed && c == key) continue;
            dest[i] = blend ? gfx_blend_pixel(c, dest[i]) : c;
        }
    }
}

void gfx_blit(GfxContext* ctx, const GfxSurface* src, const GfxRect* src_rect, int32_t x, int32_t y) {
    gfx_blit_internal(ctx, src, src_rect, x, y, false, 0);
}

void gfx_blit_keyed(GfxContext* ctx, const GfxSurface* src, const GfxRect* src_rect,
                    int32_t x, int32_t y, gfx_color_t key) {
    gfx_blit_internal(ctx, src, src_rect, x, y, true, key);
}

void gfx_draw_pixels(GfxContext* ctx, const uint32_t* pixels, int32_t width, int32_t height,
                     int32_t x, int32_t y, gfx_color_t key, bool keyed) {
    // Read only: the cast just lets a const image travel as a surface
    GfxSurface src;
    gfx_surface_wrap(&src, (uint32_t*)pixels, width, height, width);
    gfx_blit_internal(ctx, &src, NULL, x, y, keyed, key);
}
