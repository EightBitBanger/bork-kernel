#ifndef _KERNEL_GFX_H_
#define _KERNEL_GFX_H_

// 2D graphical effects interface
//
// The GFX subsystem handles pixel related drawing and other 
// related graphical device functions.
//
//   GfxSurface   a block of 32 bpp ARGB pixels: width, height, stride.
//                The back buffer, every window's frame buffer and every
//                image are surfaces. A surface either owns its pixels
//                (gfx_surface_create) or wraps memory someone else owns
//                (gfx_surface_wrap).
//
//   GfxContext   where and how to draw a target surface, a clip rectangle,
//                an origin (translation), a blend mode and a font. Contexts 
//                are plain structs, usually on the stack, and each owner 
//                keeps its own, so the console, the compositor and a window 
//                procedure can never trample each other's clip or target the 
//                way the old global draw_* state could. gfx_save / gfx_restore 
//                push and pop the state.
//
//   GfxFont      a fixed-width bitmap font (the built-in char ROM by default)
//
// Coordinates passed to drawing functions are LOGICAL: the context origin is
// added to them before clipping. Clip rectangles are given in logical
// coordinates too, but stored in surface (device) coordinates, so moving
// the origin later does not move an existing clip.
//
// Colors are 0xAARRGGBB. Under GFX_BLEND_COPY (the default) pixels are
// stored as given, alpha included. Under GFX_BLEND_ALPHA they are blended
// over what is already there; fully opaque colors (alpha 0xFF) look the
// same in both modes.
//
// The display (back buffer, VRAM, presenting) is in <kernel/gfx/gfx_display.h>.
//
// Threading: a context and its surface belong to whoever draws with them.
// Nothing in here takes a lock.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//
// Colors
//

typedef uint32_t gfx_color_t;

#define GFX_ARGB(a, r, g, b) \
    ((gfx_color_t)(((uint32_t)(a) & 0xFF) << 24 | ((uint32_t)(r) & 0xFF) << 16 | \
                   ((uint32_t)(g) & 0xFF) << 8  |  ((uint32_t)(b) & 0xFF)))
#define GFX_RGB(r, g, b)         GFX_ARGB(0xFF, r, g, b)

#define GFX_ALPHA(c)             (((c) >> 24) & 0xFF)
#define GFX_RED(c)               (((c) >> 16) & 0xFF)
#define GFX_GREEN(c)             (((c) >> 8)  & 0xFF)
#define GFX_BLUE(c)              ( (c)        & 0xFF)

#define GFX_COLOR_BLACK          0xFF000000u
#define GFX_COLOR_WHITE          0xFFFFFFFFu
#define GFX_COLOR_TRANSPARENT    0x00000000u

// `src` over `dest` (the result is always opaque)
static inline gfx_color_t gfx_blend_pixel(gfx_color_t src, gfx_color_t dest) {
    int32_t alpha = (int32_t)GFX_ALPHA(src);
    if (alpha == 0)   return dest;
    if (alpha == 255) return src;

    // Signed: a source channel darker than the destination is negative
    int32_t r = (int32_t)GFX_RED(dest)   + (((int32_t)GFX_RED(src)   - (int32_t)GFX_RED(dest))   * alpha) / 255;
    int32_t g = (int32_t)GFX_GREEN(dest) + (((int32_t)GFX_GREEN(src) - (int32_t)GFX_GREEN(dest)) * alpha) / 255;
    int32_t b = (int32_t)GFX_BLUE(dest)  + (((int32_t)GFX_BLUE(src)  - (int32_t)GFX_BLUE(dest))  * alpha) / 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Channels as 0.0 .. 1.0 (clamped)
gfx_color_t gfx_color_from_float(float a, float r, float g, float b);

// The color 3/4 of the way from `color` to black, same alpha (3D shading)
gfx_color_t gfx_color_darken(gfx_color_t color);

//
// Rectangles
//

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} GfxRect;

static inline GfxRect gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    GfxRect r = { x, y, w, h };
    return r;
}

static inline bool gfx_rect_empty(const GfxRect* r) {
    return r->w <= 0 || r->h <= 0;
}

static inline bool gfx_rect_contains(const GfxRect* r, int32_t x, int32_t y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static inline bool gfx_rect_intersects(const GfxRect* a, const GfxRect* b) {
    return !(a->x + a->w <= b->x || b->x + b->w <= a->x ||
             a->y + a->h <= b->y || b->y + b->h <= a->y);
}

// a ∩ b. Empty (w = h = 0) when they do not overlap. Returns !empty.
bool gfx_rect_intersect(const GfxRect* a, const GfxRect* b, GfxRect* out);

// Smallest rectangle holding both (an empty one is ignored)
GfxRect gfx_rect_union(const GfxRect* a, const GfxRect* b);

//
// Surfaces
//

#define GFX_SURFACE_OWNED   0x01    // pixels came from gfx_surface_create

typedef struct {
    uint32_t* pixels;
    int32_t   width;
    int32_t   height;
    int32_t   stride;               // Pixels per row (>= width)
    uint32_t  flags;                // GFX_SURFACE_*
} GfxSurface;

// Point `surface` at memory owned elsewhere. stride 0 means stride = width.
// NULL pixels make a zero-sized surface that every drawing call ignores.
void gfx_surface_wrap(GfxSurface* surface, uint32_t* pixels, int32_t width, int32_t height, int32_t stride);

// Allocate width * height pixels (not cleared). False when out of memory.
bool gfx_surface_create(GfxSurface* surface, int32_t width, int32_t height);

// Free the pixels if the surface owns them, and zero it
void gfx_surface_destroy(GfxSurface* surface);

// Pointer to pixel (x, y), or NULL when it is outside the surface
static inline uint32_t* gfx_surface_pixel(const GfxSurface* s, int32_t x, int32_t y) {
    if (s->pixels == NULL || x < 0 || y < 0 || x >= s->width || y >= s->height)
        return NULL;
    return &s->pixels[(size_t)y * (size_t)s->stride + (size_t)x];
}

static inline GfxRect gfx_surface_bounds(const GfxSurface* s) {
    return gfx_rect(0, 0, s->width, s->height);
}

//
// Fonts
//
// Glyphs are stored column-major, one byte per column: bit n of a column
// is pixel row n (so glyph_height <= 8). Glyph i starts at glyphs[i * glyph_width].

typedef struct {
    const uint8_t* glyphs;
    uint16_t       first;           // Character code of glyph 0
    uint16_t       count;           // Glyphs in the table
    uint8_t        glyph_width;     // Columns stored per glyph
    uint8_t        glyph_height;    // Rows used per column (<= 8)
    uint8_t        advance;         // Pen advance per character
    uint8_t        line_height;     // Cell height (opaque text fills this much)
    uint8_t        fallback;        // Shown for characters the font lacks
} GfxFont;

// The built-in 6 x 8 char ROM font
const GfxFont* gfx_font_default(void);

int32_t gfx_text_width(const GfxFont* font, const char* text);
int32_t gfx_text_width_n(const GfxFont* font, const char* text, size_t length);

//
// Contexts
//

typedef enum {
    GFX_BLEND_COPY  = 0,            // Store colors as given
    GFX_BLEND_ALPHA = 1,            // Blend by source alpha
} GfxBlend;

typedef enum {
    GFX_GRADIENT_VERTICAL   = 0,    // `from` at the top, `to` at the bottom
    GFX_GRADIENT_HORIZONTAL = 1,    // `from` on the left, `to` on the right
} GfxGradient;

#define GFX_STATE_DEPTH  8

typedef struct {
    GfxRect        clip;            // Device coordinates
    int32_t        origin_x;
    int32_t        origin_y;
    GfxBlend       blend;
    const GfxFont* font;
    uint8_t        text_scale;      // Each font pixel becomes scale x scale (>= 1)
} GfxState;

typedef struct {
    GfxSurface*    target;
    GfxState       state;
    GfxState       stack[GFX_STATE_DEPTH];
    uint8_t        depth;
} GfxContext;

// Target `surface`: clip = whole surface, origin (0, 0), COPY, default font
void gfx_context_init(GfxContext* ctx, GfxSurface* surface);

// Change the target. The clip, origin and the rest are kept (the clip is
// cut down to whatever part of it the new surface has, at draw time).
void gfx_set_target(GfxContext* ctx, GfxSurface* surface);
GfxSurface* gfx_get_target(const GfxContext* ctx);

// Clip to `rect` (logical coordinates). NULL: the whole target.
void gfx_set_clip(GfxContext* ctx, const GfxRect* rect);

// Narrow the current clip to its overlap with `rect` (logical coordinates)
void gfx_intersect_clip(GfxContext* ctx, const GfxRect* rect);

// The effective clip in logical coordinates (already cut to the target)
GfxRect gfx_get_clip(const GfxContext* ctx);

void gfx_set_origin(GfxContext* ctx, int32_t x, int32_t y);
void gfx_translate(GfxContext* ctx, int32_t dx, int32_t dy);
void gfx_get_origin(const GfxContext* ctx, int32_t* x, int32_t* y);

// Returns the previous mode
GfxBlend gfx_set_blend(GfxContext* ctx, GfxBlend blend);

// NULL selects the default font. Returns the previous one.
const GfxFont* gfx_set_font(GfxContext* ctx, const GfxFont* font);
const GfxFont* gfx_get_font(const GfxContext* ctx);

// Integer text magnification (1 = native). Clamped to 1 .. 8. Returns the
// previous scale. Advance and line height scale with it.
uint8_t gfx_set_text_scale(GfxContext* ctx, uint8_t scale);
uint8_t gfx_get_text_scale(const GfxContext* ctx);

// Size of one character cell / of a string, in pixels, as this context
// would draw it (font and text scale applied)
int32_t gfx_context_char_width(const GfxContext* ctx);
int32_t gfx_context_line_height(const GfxContext* ctx);
int32_t gfx_context_text_width(const GfxContext* ctx, const char* text);

// Push / pop clip, origin, blend, font and text scale. False when the stack is full
// (save) or empty (restore); the state is then left as it is.
bool gfx_save(GfxContext* ctx);
bool gfx_restore(GfxContext* ctx);

//
// Drawing
//

void gfx_clear(GfxContext* ctx, gfx_color_t color);

void gfx_set_pixel(GfxContext* ctx, int32_t x, int32_t y, gfx_color_t color);
gfx_color_t gfx_get_pixel(const GfxContext* ctx, int32_t x, int32_t y);  // 0 outside

void gfx_draw_line(GfxContext* ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, gfx_color_t color);
void gfx_draw_hline(GfxContext* ctx, int32_t x, int32_t y, int32_t w, gfx_color_t color);
void gfx_draw_vline(GfxContext* ctx, int32_t x, int32_t y, int32_t h, gfx_color_t color);

void gfx_fill_rect(GfxContext* ctx, int32_t x, int32_t y, int32_t w, int32_t h, gfx_color_t color);
void gfx_draw_rect(GfxContext* ctx, int32_t x, int32_t y, int32_t w, int32_t h, gfx_color_t color);
void gfx_fill_gradient(GfxContext* ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                       gfx_color_t from, gfx_color_t to, GfxGradient direction);

void gfx_draw_circle(GfxContext* ctx, int32_t cx, int32_t cy, int32_t radius, gfx_color_t color);
void gfx_fill_circle(GfxContext* ctx, int32_t cx, int32_t cy, int32_t radius, gfx_color_t color);

// Pie chart: slice i gets values[i] / sum of the circle in colors[i],
// clockwise from 12 o'clock. At most GFX_PIE_MAX_SLICES slices.
#define GFX_PIE_MAX_SLICES  32
void gfx_draw_pie(GfxContext* ctx, int32_t cx, int32_t cy, int32_t radius,
                  const uint32_t* values, const gfx_color_t* colors, size_t count);

// Same, tilted (half height) with a `depth` pixel side
void gfx_draw_pie_isometric(GfxContext* ctx, int32_t cx, int32_t cy, int32_t radius, int32_t depth,
                            const uint32_t* values, const gfx_color_t* colors, size_t count);

//
// Text (current font). Characters are drawn left to right, no wrapping.
//

// Transparent background
void gfx_draw_text(GfxContext* ctx, int32_t x, int32_t y, const char* text, gfx_color_t color);
void gfx_draw_text_n(GfxContext* ctx, int32_t x, int32_t y, const char* text, size_t length, gfx_color_t color);

// Each character cell (advance x line_height) is filled with `bg` first
void gfx_draw_text_opaque(GfxContext* ctx, int32_t x, int32_t y, const char* text,
                          gfx_color_t fg, gfx_color_t bg);

void gfx_draw_char(GfxContext* ctx, int32_t x, int32_t y, uint8_t ch, gfx_color_t fg);
void gfx_draw_char_opaque(GfxContext* ctx, int32_t x, int32_t y, uint8_t ch, gfx_color_t fg, gfx_color_t bg);

//
// Blits. src_rect selects part of the source (NULL: all of it) and is cut
// to the source bounds. (x, y) is where its top-left corner lands.
//

// Uses the context's blend mode
void gfx_blit(GfxContext* ctx, const GfxSurface* src, const GfxRect* src_rect, int32_t x, int32_t y);

// Pixels equal to `key` are skipped; the rest follow the blend mode
void gfx_blit_keyed(GfxContext* ctx, const GfxSurface* src, const GfxRect* src_rect,
                    int32_t x, int32_t y, gfx_color_t key);

// Convenience for raw pixel arrays (sprites, images): stride = width
void gfx_draw_pixels(GfxContext* ctx, const uint32_t* pixels, int32_t width, int32_t height,
                     int32_t x, int32_t y, gfx_color_t key, bool keyed);

#endif
