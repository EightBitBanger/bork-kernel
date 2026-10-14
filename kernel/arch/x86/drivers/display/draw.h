#ifndef DRAWING_2D_H
#define DRAWING_2D_H

// Legacy drawing API (compatibility layer)
//
// These are the old global-state draw_* calls, now implemented on top of
// <kernel/gfx/gfx.h>. They draw through one BOUND GfxContext: by default an
// internal context targeting the display back buffer. The DWM binds a
// window's paint context while that window's procedure handles REDRAW, so
// old window procedures that call draw_* still land in their own window.
//
// New code should use a GfxContext directly (dwm_window_paint_context()
// inside a window procedure, or its own context on its own surface).
//
// Each call keeps its historical blending behavior regardless of the
// bound context's blend mode: *_blend and the gradients alpha-blend,
// everything else stores colors as given.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_display.h>
#include <kernel/boot/x86/multiboot_info.h>

#define COLOR(val) (uint16_t)((val) & 0xFFFF), (uint16_t)((val) >> 16)

struct ClippingPlane {
    int32_t min_x;
    int32_t min_y;
    int32_t max_x;
    int32_t max_y;
};

static inline uint32_t blend_pixels(uint32_t src, uint32_t dest) {
    return gfx_blend_pixel(src, dest);
}

// The context draw_* calls go through. Pass NULL to bind the internal
// default context again. Returns the previously bound one.
GfxContext* draw_bind_context(GfxContext* ctx);
GfxContext* draw_get_context(void);

void draw_pixel(int x, int y, uint32_t color);

void draw_set_glyph_scheme(uint32_t foreground, uint32_t background, uint32_t transparent);

// Clip is in target (device) coordinates, as it always was
void draw_set_clip_rect(int32_t x, int32_t y, int32_t w, int32_t h);
void draw_set_origin(int32_t x, int32_t y);

void draw_set_buffer_default(void);
void draw_set_buffer(uint32_t* target_buffer, uint32_t width, uint32_t height);

// Deprecated boot-time setup; kmain now calls gfx_display_init() directly.
// Kept so older callers still bring the display up.
void draw_set_info(uint32_t mbi);
void draw_set_frame_front_buffer(uint32_t buffer_ptr);
void draw_set_frame_back_buffer(uint32_t buffer_ptr);

void draw_flush_region(int x, int y, int width, int height);
void draw_flush_display(void);

uint32_t make_color(float a, float r, float g, float b);

void draw_line(int x0, int y0, int x1, int y1, uint32_t color);

void draw_rect(int x, int y, int width, int height, uint32_t color);
void draw_rect_filled(int x, int y, int width, int height, uint32_t color);
void draw_rect_filled_blend(int x, int y, int width, int height, uint32_t color);
void draw_rect_gradient_vertical_blend(int x, int y, int width, int height, uint32_t color_from, uint32_t color_to);
void draw_rect_gradient_horizontal_blend(int x, int y, int width, int height, uint32_t color_from, uint32_t color_to);

void draw_circle(int xc, int yc, int r, uint32_t color);

void draw_text(int16_t x, int16_t y, const char* text, uint32_t color);

void draw_glyph(const uint8_t* glyph_map, int glyph_index, uint16_t glyph_width, uint16_t glyph_height, int x, int y, uint32_t fg_color, uint32_t bg_color, int transparent_bg);
void draw_sprite(const uint32_t* sprite_data, int sprite_w, int sprite_h, int x, int y, uint32_t colorkey);
void draw_sprite_blend(const uint32_t* sprite_data, int sprite_w, int sprite_h, int x, int y, uint32_t colorkey);

void draw_pie_chart(int center_x, int center_y, int radius, const uint32_t* slices, const uint32_t* colors, size_t count);
void draw_pie_chart_isometric(int center_x, int center_y, int radius, int depth, const uint32_t* slices, const uint32_t* colors, size_t count);

#endif
