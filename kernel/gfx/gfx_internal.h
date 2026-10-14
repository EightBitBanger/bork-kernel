#ifndef _KERNEL_GFX_INTERNAL_H_
#define _KERNEL_GFX_INTERNAL_H_

// Shared by the gfx_*.c files only

#include <kernel/gfx/gfx.h>

// The clip actually in force: the stored clip cut to the target's bounds,
// in device coordinates. False when nothing can be drawn.
static inline bool gfx_device_clip(const GfxContext* ctx, GfxRect* out) {
    if (ctx == NULL || ctx->target == NULL || ctx->target->pixels == NULL)
        return false;
    GfxRect bounds = gfx_surface_bounds(ctx->target);
    return gfx_rect_intersect(&ctx->state.clip, &bounds, out);
}

// Logical rectangle -> device, cut to the clip. False when nothing is left.
static inline bool gfx_device_rect(const GfxContext* ctx, int32_t x, int32_t y, int32_t w, int32_t h, GfxRect* out) {
    GfxRect clip;
    if (w <= 0 || h <= 0 || !gfx_device_clip(ctx, &clip))
        return false;
    GfxRect r = gfx_rect(x + ctx->state.origin_x, y + ctx->state.origin_y, w, h);
    return gfx_rect_intersect(&r, &clip, out);
}

static inline uint32_t* gfx_row(const GfxSurface* s, int32_t x, int32_t y) {
    return &s->pixels[(size_t)y * (size_t)s->stride + (size_t)x];
}

static inline void gfx_row_fill(uint32_t* dest, gfx_color_t color, size_t count) {
    __asm__ volatile("cld\n\trep stosl"
                     : "+D"(dest), "+c"(count)
                     : "a"(color)
                     : "memory");
}

static inline void gfx_row_copy(uint32_t* dest, const uint32_t* src, size_t count) {
    __asm__ volatile("cld\n\trep movsl"
                     : "+D"(dest), "+S"(src), "+c"(count)
                     :
                     : "memory");
}

static inline void gfx_row_blend(uint32_t* dest, gfx_color_t color, size_t count) {
    for (size_t i = 0; i < count; i++)
        dest[i] = gfx_blend_pixel(color, dest[i]);
}

// One pixel, device coordinates, already known to be inside the clip
static inline void gfx_plot(const GfxContext* ctx, int32_t x, int32_t y, gfx_color_t color) {
    uint32_t* p = gfx_row(ctx->target, x, y);
    *p = (ctx->state.blend == GFX_BLEND_ALPHA) ? gfx_blend_pixel(color, *p) : color;
}

#endif
