#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/events.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/timer.h>
#include <kernel/util/string.h>

bool rects_intersect(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) {
    return !(x1 + w1 <= x2 || x2 + w2 <= x1 || y1 + h1 <= y2 || y2 + h2 <= y1);
}

void get_rect_intersection(int x1, int y1, int w1, int h1, 
                           int x2, int y2, int w2, int h2, 
                           int *out_x, int *out_y, int *out_w, int *out_h) {
    int ix1 = (x1 > x2) ? x1 : x2;
    int iy1 = (y1 > y2) ? y1 : y2;
    int ix2 = (x1 + w1 < x2 + w2) ? x1 + w1 : x2 + w2;
    int iy2 = (y1 + h1 < y2 + h2) ? y1 + h1 : y2 + h2;
    if (ix1 < ix2 && iy1 < iy2) {
        *out_x = ix1;
        *out_y = iy1;
        *out_w = ix2 - ix1;
        *out_h = iy2 - iy1;
    } else {
        *out_w = 0;
        *out_h = 0;
    }
}

void dwm_draw_redraw(int16_t x, int16_t y, int16_t w, int16_t h) {
    dwm_invalidate_region(x, y, w, h);
}

// Mark a screen rectangle for repaint and flush this frame.
//
// Every dirty rect is repainted and flushed separately, so overlapping rects
// cost their overlap twice. A window drag invalidates the old and new window
// position for every mouse event, which used to add two nearly identical
// window-sized rects per event and repaint the window that many times per
// frame. A new rect is now merged with any rect it overlaps or touches when
// the union covers no more area than the two rects separately, so those
// collapse into one. Merging repeats because the grown rect may reach others.
void dwm_invalidate_region(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) return;

    // Bounds clipping (in int: x + w can overflow int16_t)
    int nx1 = x, ny1 = y, nx2 = (int)x + w, ny2 = (int)y + h;
    if (nx1 < 0) nx1 = 0;
    if (ny1 < 0) ny1 = 0;
    
    int display_w = display_get_width();
    int display_h = display_get_height();

    if (nx2 > display_w) nx2 = display_w;
    if (ny2 > display_h) ny2 = display_h;
    if (nx2 <= nx1 || ny2 <= ny1) return;

    struct Rect* regions = context.window_context.dirty_regions;
    int* count = &context.window_context.dirty_count;

    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < *count; i++) {
            int rx1 = regions[i].x;
            int ry1 = regions[i].y;
            int rx2 = rx1 + regions[i].w;
            int ry2 = ry1 + regions[i].h;

            // Must overlap or share an edge
            if (rx1 > nx2 || nx1 > rx2 || ry1 > ny2 || ny1 > ry2) continue;

            int ux1 = (rx1 < nx1) ? rx1 : nx1;
            int uy1 = (ry1 < ny1) ? ry1 : ny1;
            int ux2 = (rx2 > nx2) ? rx2 : nx2;
            int uy2 = (ry2 > ny2) ? ry2 : ny2;

            int area_new   = (nx2 - nx1) * (ny2 - ny1);
            int area_old   = (rx2 - rx1) * (ry2 - ry1);
            int area_union = (ux2 - ux1) * (uy2 - uy1);
            if (area_union > area_new + area_old) continue;

            // Absorb rect i into the new rect and drop it from the list
            nx1 = ux1; ny1 = uy1; nx2 = ux2; ny2 = uy2;
            regions[i] = regions[*count - 1];
            (*count)--;
            merged = true;
            break;
        }
    }

    if (*count < MAX_DIRTY_RECTS) {
        regions[*count].x = nx1;
        regions[*count].y = ny1;
        regions[*count].w = nx2 - nx1;
        regions[*count].h = ny2 - ny1;
        (*count)++;
        return;
    }

    // List full: fall back to one bounding box
    int min_x = nx1, min_y = ny1, max_x = nx2, max_y = ny2;
    for (int i = 0; i < *count; i++) {
        int rx1 = regions[i].x;
        int ry1 = regions[i].y;
        int rx2 = rx1 + regions[i].w;
        int ry2 = ry1 + regions[i].h;

        if (rx1 < min_x) min_x = rx1;
        if (ry1 < min_y) min_y = ry1;
        if (rx2 > max_x) max_x = rx2;
        if (ry2 > max_y) max_y = ry2;
    }
    
    regions[0].x = min_x;
    regions[0].y = min_y;
    regions[0].w = max_x - min_x;
    regions[0].h = max_y - min_y;
    *count = 1;
}

void dwm_calculate_icon_bounds(struct IconObject* icon) {
    size_t length = strlen(icon->name);
    int16_t text_width = length * 6;
    int16_t text_height = 8;        
    int16_t icon_text_height = 45;  
    
    if (text_width > icon->width) {
        icon->bounds_x = (icon->width - text_width) / 2;
        icon->bounds_w = text_width;
    } else {
        icon->bounds_x = 0;
        icon->bounds_w = icon->width;
    }
    
    icon->bounds_y = 0;
    icon->bounds_h = icon_text_height + text_height;
}

void dwm_get_absolute_position(struct WindowObject* window, int* out_x, int* out_y) {
    if (window->parent == NULL) {
        *out_x = window->x;
        *out_y = window->y;
        return;
    }
    
    int abs_x = window->local_x;
    int abs_y = window->local_y;
    struct WindowObject* p = window->parent;
    while (p != NULL) {
        abs_x += p->surface_x;
        abs_y += p->surface_y;
        p = p->parent;
    }
    
    *out_x = abs_x;
    *out_y = abs_y;
}

// Ease out: cover 1/divisor of the remaining distance per frame, never less
// than 2 px (so the tail of the slide doesn't crawl) and never past target
int dwm_slide_step(int cur, int target, int divisor) {
    int d = target - cur;
    if (d == 0) return cur;
    if (divisor < 1) divisor = 1;

    int s = d / divisor;
    if (s > -2 && s < 2) s = (d > 0) ? 2 : -2;
    if ((d > 0 && s > d) || (d < 0 && s < d)) s = d;
    return cur + s;
}

// Move a top-level window to (x, y): repaint the spot it leaves and the one
// it lands on. Children follow through dwm_sync_child_positions each frame.
void dwm_window_place(struct WindowObject* window, int x, int y) {
    if (window == NULL) return;
    if ((int)window->x == x && (int)window->y == y) return;

    int b = window->border_width;
    dwm_invalidate_region(window->x - b, window->y - b, window->w + (b * 2), window->h + (b * 2));

    window->x = (uint16_t)x;
    window->y = (uint16_t)y;
    window->surface_x = window->x;
    window->surface_y = window->y + window->titlebar_height + (window->titlebar_height ? 1 : 0);
    window->flags |= (DWM_WFLAG_REFRESH | DWM_WFLAG_REDECORATE);

    dwm_invalidate_region(window->x - b, window->y - b, window->w + (b * 2), window->h + (b * 2));
}
