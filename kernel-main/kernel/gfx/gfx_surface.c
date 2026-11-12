// Colors, rectangles, surfaces and contexts

#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_internal.h>

#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// Colors
//

static inline uint32_t gfx_unit_to_byte(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (uint32_t)(v * 255.0f + 0.5f);
}

gfx_color_t gfx_color_from_float(float a, float r, float g, float b) {
    return GFX_ARGB(gfx_unit_to_byte(a), gfx_unit_to_byte(r), gfx_unit_to_byte(g), gfx_unit_to_byte(b));
}

gfx_color_t gfx_color_darken(gfx_color_t c) {
    uint32_t r = GFX_RED(c),  g = GFX_GREEN(c), b = GFX_BLUE(c);
    return (c & 0xFF000000u) | ((r - (r >> 2)) << 16) | ((g - (g >> 2)) << 8) | (b - (b >> 2));
}

//
// Rectangles
//

bool gfx_rect_intersect(const GfxRect* a, const GfxRect* b, GfxRect* out) {
    int32_t x1 = (a->x > b->x) ? a->x : b->x;
    int32_t y1 = (a->y > b->y) ? a->y : b->y;
    int32_t x2 = (a->x + a->w < b->x + b->w) ? a->x + a->w : b->x + b->w;
    int32_t y2 = (a->y + a->h < b->y + b->h) ? a->y + a->h : b->y + b->h;

    if (a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0 || x1 >= x2 || y1 >= y2) {
        *out = gfx_rect(0, 0, 0, 0);
        return false;
    }
    *out = gfx_rect(x1, y1, x2 - x1, y2 - y1);
    return true;
}

GfxRect gfx_rect_union(const GfxRect* a, const GfxRect* b) {
    if (gfx_rect_empty(a)) return *b;
    if (gfx_rect_empty(b)) return *a;

    int32_t x1 = (a->x < b->x) ? a->x : b->x;
    int32_t y1 = (a->y < b->y) ? a->y : b->y;
    int32_t x2 = (a->x + a->w > b->x + b->w) ? a->x + a->w : b->x + b->w;
    int32_t y2 = (a->y + a->h > b->y + b->h) ? a->y + a->h : b->y + b->h;
    return gfx_rect(x1, y1, x2 - x1, y2 - y1);
}

//
// Surfaces
//

void gfx_surface_wrap(GfxSurface* surface, uint32_t* pixels, int32_t width, int32_t height, int32_t stride) {
    if (surface == NULL) return;

    if (pixels == NULL || width <= 0 || height <= 0) {
        memset(surface, 0, sizeof(GfxSurface));
        return;
    }
    surface->pixels = pixels;
    surface->width  = width;
    surface->height = height;
    surface->stride = (stride >= width) ? stride : width;
    surface->flags  = 0;
}

bool gfx_surface_create(GfxSurface* surface, int32_t width, int32_t height) {
    if (surface == NULL) return false;
    memset(surface, 0, sizeof(GfxSurface));
    if (width <= 0 || height <= 0) return false;

    uint64_t bytes = (uint64_t)width * (uint64_t)height * sizeof(uint32_t);
    if (bytes > 0xFFFFFFFFull) return false;

    uint32_t* pixels = (uint32_t*)malloc((uint32_t)bytes);
    if (pixels == NULL) return false;

    gfx_surface_wrap(surface, pixels, width, height, width);
    surface->flags = GFX_SURFACE_OWNED;
    return true;
}

void gfx_surface_destroy(GfxSurface* surface) {
    if (surface == NULL) return;
    if ((surface->flags & GFX_SURFACE_OWNED) && surface->pixels != NULL)
        free(surface->pixels);
    memset(surface, 0, sizeof(GfxSurface));
}

//
// Contexts
//

void gfx_context_init(GfxContext* ctx, GfxSurface* surface) {
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(GfxContext));

    ctx->target       = surface;
    ctx->state.blend  = GFX_BLEND_COPY;
    ctx->state.font   = gfx_font_default();
    ctx->state.text_scale = 1;

    // Unbounded: the target's own bounds are applied at draw time, so a
    // later gfx_set_target to a bigger surface is not cut short
    gfx_set_clip(ctx, NULL);
}

void gfx_set_target(GfxContext* ctx, GfxSurface* surface) {
    if (ctx != NULL) ctx->target = surface;
}

GfxSurface* gfx_get_target(const GfxContext* ctx) {
    return (ctx != NULL) ? ctx->target : NULL;
}

void gfx_set_clip(GfxContext* ctx, const GfxRect* rect) {
    if (ctx == NULL) return;

    if (rect == NULL) {
        // Large enough for any target this kernel can address
        ctx->state.clip = gfx_rect(0, 0, 0x7FFF, 0x7FFF);
        return;
    }
    ctx->state.clip = gfx_rect(rect->x + ctx->state.origin_x, rect->y + ctx->state.origin_y,
                               rect->w, rect->h);
}

void gfx_intersect_clip(GfxContext* ctx, const GfxRect* rect) {
    if (ctx == NULL || rect == NULL) return;

    GfxRect r = gfx_rect(rect->x + ctx->state.origin_x, rect->y + ctx->state.origin_y, rect->w, rect->h);
    GfxRect out;
    gfx_rect_intersect(&ctx->state.clip, &r, &out);
    ctx->state.clip = out;
}

GfxRect gfx_get_clip(const GfxContext* ctx) {
    GfxRect clip;
    if (!gfx_device_clip(ctx, &clip))
        return gfx_rect(0, 0, 0, 0);
    clip.x -= ctx->state.origin_x;
    clip.y -= ctx->state.origin_y;
    return clip;
}

void gfx_set_origin(GfxContext* ctx, int32_t x, int32_t y) {
    if (ctx == NULL) return;
    ctx->state.origin_x = x;
    ctx->state.origin_y = y;
}

void gfx_translate(GfxContext* ctx, int32_t dx, int32_t dy) {
    if (ctx == NULL) return;
    ctx->state.origin_x += dx;
    ctx->state.origin_y += dy;
}

void gfx_get_origin(const GfxContext* ctx, int32_t* x, int32_t* y) {
    if (x != NULL) *x = (ctx != NULL) ? ctx->state.origin_x : 0;
    if (y != NULL) *y = (ctx != NULL) ? ctx->state.origin_y : 0;
}

GfxBlend gfx_set_blend(GfxContext* ctx, GfxBlend blend) {
    if (ctx == NULL) return GFX_BLEND_COPY;
    GfxBlend old = ctx->state.blend;
    ctx->state.blend = blend;
    return old;
}

const GfxFont* gfx_set_font(GfxContext* ctx, const GfxFont* font) {
    if (ctx == NULL) return NULL;
    const GfxFont* old = ctx->state.font;
    ctx->state.font = (font != NULL) ? font : gfx_font_default();
    return old;
}

const GfxFont* gfx_get_font(const GfxContext* ctx) {
    return (ctx != NULL && ctx->state.font != NULL) ? ctx->state.font : gfx_font_default();
}

uint8_t gfx_set_text_scale(GfxContext* ctx, uint8_t scale) {
    if (ctx == NULL) return 1;
    uint8_t old = ctx->state.text_scale;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    ctx->state.text_scale = scale;
    return old;
}

uint8_t gfx_get_text_scale(const GfxContext* ctx) {
    return (ctx != NULL && ctx->state.text_scale > 0) ? ctx->state.text_scale : 1;
}

int32_t gfx_context_char_width(const GfxContext* ctx) {
    return (int32_t)gfx_get_font(ctx)->advance * gfx_get_text_scale(ctx);
}

int32_t gfx_context_line_height(const GfxContext* ctx) {
    return (int32_t)gfx_get_font(ctx)->line_height * gfx_get_text_scale(ctx);
}

int32_t gfx_context_text_width(const GfxContext* ctx, const char* text) {
    return gfx_text_width(gfx_get_font(ctx), text) * gfx_get_text_scale(ctx);
}

bool gfx_save(GfxContext* ctx) {
    if (ctx == NULL || ctx->depth >= GFX_STATE_DEPTH) return false;
    ctx->stack[ctx->depth++] = ctx->state;
    return true;
}

bool gfx_restore(GfxContext* ctx) {
    if (ctx == NULL || ctx->depth == 0) return false;
    ctx->state = ctx->stack[--ctx->depth];
    return true;
}
