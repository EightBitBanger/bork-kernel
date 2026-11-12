#ifndef DIRTY_H
#define DIRTY_H

/*
 * Dirty rectangles: the parts of the screen that changed since the last
 * repaint, so a frame redraws and presents only those.
 *
 *     dirty_add(&d, where_the_cursor_was);
 *     dirty_add(&d, where_the_cursor_is);
 *     for (size_t i = 0; i < d.count; i++)
 *         repaint(d.rects[i]);
 *     dirty_clear(&d);
 *
 * Every rectangle is cut to the screen. A new one is merged with another
 * when the box around both wastes little (side by side, nearly the same
 * area, one inside the other); where it overlaps another badly, only its
 * parts outside that one are kept. So the list holds disjoint, tight
 * rectangles and no pixel is drawn twice per frame. When the list is full,
 * a new rectangle is merged into the one it grows least.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>

#define DIRTY_MAX   32

typedef struct {
    int32_t    width;           // Screen size: everything is cut to it
    int32_t    height;
    size_t     count;
    gfx_rect_t rects[DIRTY_MAX];
} dirty_t;

void dirty_init(dirty_t* d, int32_t width, int32_t height);

// Mark a region (screen coordinates). Empty or off-screen regions are ignored.
void dirty_add(dirty_t* d, gfx_rect_t r);

// Mark the whole screen
void dirty_add_all(dirty_t* d);

bool dirty_empty(const dirty_t* d);
void dirty_clear(dirty_t* d);

// Rectangle helpers
bool       rect_empty(gfx_rect_t r);
bool       rect_overlaps(gfx_rect_t a, gfx_rect_t b);
gfx_rect_t rect_union(gfx_rect_t a, gfx_rect_t b);
gfx_rect_t rect_intersect(gfx_rect_t a, gfx_rect_t b);     // Empty (w = h = 0) if they don't meet

#endif
