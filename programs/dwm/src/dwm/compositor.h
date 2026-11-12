#ifndef DWM_COMPOSITOR_H
#define DWM_COMPOSITOR_H

/*
 * Compositor: owns the screen surface and the dirty rectangles.
 *
 * Anything that changes what the screen shows marks that area dirty
 * (compositor_invalidate). Once per frame compositor_render() redraws each
 * dirty rectangle from the bottom up (desktop background, windows in
 * stacking order with their frames, the mouse cursor on top) and presents
 * just those rectangles.
 */

#include <stdbool.h>
#include <stdint.h>
#include <os/gfx.h>

#include "../resources/imagelib.h"

// Create the screen surface (full display size). False with errno on failure.
bool compositor_init(int32_t width, int32_t height);
void compositor_shutdown(void);

gfx_surface_t* compositor_screen(void);
int32_t        compositor_width(void);
int32_t        compositor_height(void);

// Mark a screen region (screen coordinates) to be redrawn
void compositor_invalidate(gfx_rect_t rect);
void compositor_invalidate_all(void);
bool compositor_dirty(void);

// Redraw and present every dirty region. 0, or -1 with errno if presenting
// failed (the regions are dropped either way).
int  compositor_render(void);

// True while compositor_render() is painting (window procedures run then)
bool compositor_painting(void);

// Mouse cursor: drawn above everything, hot spot at its top-left pixel.
// NULL image: a plain 8 x 8 white square.
void compositor_set_cursor(const image_t* image);
void compositor_move_cursor(int32_t x, int32_t y);
void compositor_show_cursor(bool show);    // Hidden while a window has the mouse locked

#endif

