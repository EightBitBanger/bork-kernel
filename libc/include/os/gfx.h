#ifndef _OS_GFX_H
#define _OS_GFX_H

/*
 * 2D graphics on the kernel's gfx layer.
 *
 * A gfx_surface_t is a block of 32 bpp 0xAARRGGBB pixels that the kernel
 * owns, with its own drawing state: clip rectangle, origin, blend mode,
 * text scale and a save/restore stack (the kernel's GfxContext). Drawing
 * calls do not reach the kernel one by one: they are recorded in the
 * surface's command buffer and run together, by the kernel's own drawing
 * code, when the buffer fills up or the program calls gfx_flush() or
 * gfx_present(). A frame is typically one system call.
 *
 *     gfx_surface_t* s = gfx_surface_create(320, 200);
 *     gfx_clear(s, GFX_RGB(16, 16, 32));
 *     gfx_fill_rect(s, 10, 10, 100, 50, GFX_RGB(200, 60, 60));
 *     gfx_draw_text(s, 12, 70, "hello", GFX_COLOR_WHITE);
 *     gfx_present(s, 100, 100);           // flush, then show at (100, 100)
 *     gfx_surface_destroy(s);
 *
 * Drawing calls return nothing. If a recorded command fails (an argument
 * out of range, a bad blit source, ...), the kernel skips that one command
 * and the next gfx_flush() / gfx_present() returns -1 with errno set
 * (EINVAL, EBADF, EFAULT, ...). Everything else in the batch still runs.
 *
 * Coordinates are logical: the surface's origin is added before clipping.
 * They must lie within +-GFX_COORD_MAX, sizes within +-GFX_SIZE_MAX.
 *
 * Surfaces are freed when the process exits. Not thread-safe: use one
 * surface from one thread at a time.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <os/syscall.h>

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

#define GFX_COORD_MAX            SYS_GFX_COORD_MAX
#define GFX_SIZE_MAX             SYS_GFX_SIZE_MAX
#define GFX_PIE_MAX_SLICES       SYS_GFX_PIE_MAX

typedef enum {
    GFX_BLEND_COPY  = SYS_GFX_BLEND_COPY,      // Store colors as given (default)
    GFX_BLEND_ALPHA = SYS_GFX_BLEND_ALPHA,     // Blend over what is there by source alpha
} gfx_blend_t;

typedef enum {
    GFX_GRADIENT_VERTICAL   = SYS_GFX_GRADIENT_VERTICAL,     // `from` at the top
    GFX_GRADIENT_HORIZONTAL = SYS_GFX_GRADIENT_HORIZONTAL,   // `from` on the left
} gfx_gradient_t;

typedef struct {
    int32_t x, y, w, h;
} gfx_rect_t;

static inline gfx_rect_t gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    gfx_rect_t r = { x, y, w, h };
    return r;
}

typedef struct {
    uint32_t width;         // Screen size in pixels (0 = no display)
    uint32_t height;
    uint32_t bpp;
    uint32_t char_width;    // Built-in font cell at text scale 1
    uint32_t line_height;
    uint32_t max_dim;       // Largest surface width / height
    uint32_t max_surfaces;  // Surfaces per process
    uint32_t max_bytes;     // Pixel memory per process
} gfx_info_t;

typedef struct gfx_surface gfx_surface_t;

//
// Display and surfaces
//

int gfx_get_info(gfx_info_t* out);

// New surface, opaque black. NULL with errno: EINVAL (size), ENOMEM
// (memory or the per-process budget), EMFILE (too many surfaces).
gfx_surface_t* gfx_surface_create(int32_t width, int32_t height);

// Free the surface (pending commands are dropped). 0, or -1 with errno.
int gfx_surface_destroy(gfx_surface_t* s);

int32_t gfx_surface_width(const gfx_surface_t* s);
int32_t gfx_surface_height(const gfx_surface_t* s);

// Run the recorded commands now. 0, or -1 with errno for the first failure
// since the last flush (see above).
int gfx_flush(gfx_surface_t* s);

// Flush, then show the surface on screen with its top-left corner at (x, y).
// gfx_present_rect shows only `src` (surface coordinates) with that area's
// corner at (x, y). Where it ends up depends on the kernel: in a window of
// this program when the window manager supports it, otherwise directly on
// the screen.
int gfx_present(gfx_surface_t* s, int32_t x, int32_t y);
int gfx_present_rect(gfx_surface_t* s, const gfx_rect_t* src, int32_t x, int32_t y);

// Copy pixels straight into / out of the surface (no clip, origin or
// blending). The rectangle must lie inside the surface; stride is in
// pixels, 0 meaning w. Pending commands are flushed first, so reads see
// everything drawn so far.
int gfx_write_pixels(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h,
                     const uint32_t* pixels, int32_t stride);
int gfx_read_pixels(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h,
                    uint32_t* pixels, int32_t stride);

// Single pixel read (flushes). 0 outside the surface or on error.
gfx_color_t gfx_get_pixel(gfx_surface_t* s, int32_t x, int32_t y);

//
// Drawing state
//

// Clip to `rect` in logical coordinates (NULL: the whole surface). The
// clip is stored in surface coordinates, so moving the origin later does
// not move it.
void gfx_set_clip(gfx_surface_t* s, const gfx_rect_t* rect);
void gfx_intersect_clip(gfx_surface_t* s, const gfx_rect_t* rect);

void gfx_set_origin(gfx_surface_t* s, int32_t x, int32_t y);
void gfx_translate(gfx_surface_t* s, int32_t dx, int32_t dy);
void gfx_get_origin(const gfx_surface_t* s, int32_t* x, int32_t* y);

// Return the previous value
gfx_blend_t gfx_set_blend(gfx_surface_t* s, gfx_blend_t blend);
int         gfx_set_text_scale(gfx_surface_t* s, int scale);     // 1 .. 8 (clamped)

// Push / pop clip, origin, blend and text scale (8 deep). 0, or -1 when
// the stack is full (save) or empty (restore).
int gfx_save(gfx_surface_t* s);
int gfx_restore(gfx_surface_t* s);

//
// Drawing
//

void gfx_clear(gfx_surface_t* s, gfx_color_t color);      // The whole clip area
void gfx_set_pixel(gfx_surface_t* s, int32_t x, int32_t y, gfx_color_t color);

void gfx_draw_line(gfx_surface_t* s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, gfx_color_t color);
void gfx_draw_hline(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, gfx_color_t color);
void gfx_draw_vline(gfx_surface_t* s, int32_t x, int32_t y, int32_t h, gfx_color_t color);

void gfx_draw_rect(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h, gfx_color_t color);
void gfx_fill_rect(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h, gfx_color_t color);
void gfx_fill_gradient(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h,
                       gfx_color_t from, gfx_color_t to, gfx_gradient_t direction);

void gfx_draw_circle(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t radius, gfx_color_t color);
void gfx_fill_circle(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t radius, gfx_color_t color);

// Slice i gets values[i] / sum of the circle, in colors[i], clockwise from
// 12 o'clock. At most GFX_PIE_MAX_SLICES.
void gfx_draw_pie(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t radius,
                  const uint32_t* values, const gfx_color_t* colors, size_t count);
void gfx_draw_pie_isometric(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t radius, int32_t depth,
                            const uint32_t* values, const gfx_color_t* colors, size_t count);

//
// Text, left to right, no wrapping: in the kernel's built-in 6 x 8 font, or
// in a TrueType font once the program has one (gfx_set_font). With a font,
// each byte still takes one fixed cell, gfx_char_width() wide and
// gfx_line_height() high, so text laid out by columns stays in place; the
// cell is sized from the font's '0'. Programs using os/wm.h get the font
// the window manager hands out when they connect.
//
// y is always the top of a built-in font line (8 pixels at text scale 1):
// a font's line is centered on it, so text placed for the built-in font
// stays where it was whatever font or size is set. Space lines
// gfx_line_height() apart, and center text in a box with gfx_text_y().
//

struct font;    // os/font.h

// Draw text with `font` (os/font.h) at `px` pixels high, in every surface
// of this program; NULL goes back to the built-in font. The font must stay
// open while it is set. 0, or -1 with errno (ENOMEM), keeping the built-in.
int          gfx_set_font(struct font* font, int32_t px);

// The font set (NULL: the built-in one); its size into *px
struct font* gfx_get_font(int32_t* px);

void gfx_draw_text(gfx_surface_t* s, int32_t x, int32_t y, const char* text, gfx_color_t color);
void gfx_draw_text_n(gfx_surface_t* s, int32_t x, int32_t y, const char* text, size_t length, gfx_color_t color);
void gfx_draw_text_opaque(gfx_surface_t* s, int32_t x, int32_t y, const char* text, gfx_color_t fg, gfx_color_t bg);
void gfx_draw_char(gfx_surface_t* s, int32_t x, int32_t y, unsigned char ch, gfx_color_t color);
void gfx_draw_char_opaque(gfx_surface_t* s, int32_t x, int32_t y, unsigned char ch, gfx_color_t fg, gfx_color_t bg);

// Cell size and string width at the current text scale
int32_t gfx_char_width(const gfx_surface_t* s);
int32_t gfx_line_height(const gfx_surface_t* s);
int32_t gfx_text_width(const gfx_surface_t* s, const char* text);

// The y that centers a line of text in a box `height` high starting at `top`
int32_t gfx_text_y(const gfx_surface_t* s, int32_t top, int32_t height);

//
// Images
//

// Copy part of `src` (src_rect, NULL: all of it) onto `dst` with its corner
// at (x, y), through dst's clip, origin and blend mode. Pixels equal to key
// are skipped by the keyed form. Both surfaces are flushed, so the blit
// sees everything drawn on src so far. src and dst must differ.
void gfx_blit(gfx_surface_t* dst, gfx_surface_t* src, const gfx_rect_t* src_rect, int32_t x, int32_t y);
void gfx_blit_keyed(gfx_surface_t* dst, gfx_surface_t* src, const gfx_rect_t* src_rect,
                    int32_t x, int32_t y, gfx_color_t key);

// Draw a loaded resource (os/resource.h) with its top-left corner at
// (x, y), the way the window manager draws icons: alpha blended, with
// opaque black (SYS_GFX_SPRITE_KEY, "see-through") skipped. The surface's
// blend mode is left as it was. gfx_draw_resource_rect draws only `src`
// (resource coordinates, cut to the image; NULL: all of it). Runs at once,
// like gfx_draw_pixels. A bad handle is reported by the next flush /
// present (EBADF). They live in resource.c with the resource table.
void gfx_draw_resource(gfx_surface_t* s, int resource, int32_t x, int32_t y);
void gfx_draw_resource_rect(gfx_surface_t* s, int resource, const gfx_rect_t* src, int32_t x, int32_t y);

// Draw an image from this program's memory (stride in pixels, 0 = width)
// through the surface's clip, origin and blend mode. Runs immediately (the
// pixels are read before this returns), so the buffer may be reused at once.
void gfx_draw_pixels(gfx_surface_t* s, const uint32_t* pixels, int32_t width, int32_t height,
                     int32_t stride, int32_t x, int32_t y);
void gfx_draw_pixels_keyed(gfx_surface_t* s, const uint32_t* pixels, int32_t width, int32_t height,
                           int32_t stride, int32_t x, int32_t y, gfx_color_t key);

#endif


