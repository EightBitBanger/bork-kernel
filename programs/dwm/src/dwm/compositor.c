/*
 * Compositor (compositor.h).
 *
 * Each dirty rectangle is painted bottom to top, all clipped to it:
 *
 *   1. desktop background (background.h: color or wallpaper), icons and
 *      the selection rectangle
 *   2. every visible window in stacking order: frame, then the client area
 *      (filled with the theme's client background, then the procedure's
 *      DWM_EVENT_PAINT), then the resize grip
 *   3. the taskbar (taskbar.c) and an open start menu (startmenu.c)
 *   4. an open context menu (menu.c), the plate by a file drag (drag.c)
 *   5. an administrator prompt (consent.c), the rest of the screen dimmed
 *   6. the mouse cursor
 *
 * and then presented on its own. Painting goes through the screen surface's
 * clip and origin: for a client area the clip is the dirty part of it and
 * the origin its top-left corner, so a procedure draws in client
 * coordinates and cannot reach outside its window. Its drawing state is
 * saved and restored around the call.
 */

#include <errno.h>
#include <string.h>

#include "compositor.h"
#include "background.h"
#include "consent.h"
#include "dirty.h"
#include "internal.h"
#include "theme.h"

#define CURSOR_FALLBACK     8
#define TITLE_PADDING       7

static gfx_surface_t*  screen   = NULL;
static dirty_t         dirty;
static bool            painting = false;

static const image_t*  cursor_image = NULL;
static int32_t         cursor_x = 0, cursor_y = 0;
static bool            cursor_hidden = false;

bool compositor_init(int32_t width, int32_t height) {
    screen = gfx_surface_create(width, height);
    if (screen == NULL)
        return false;
    dirty_init(&dirty, width, height);
    dirty_add_all(&dirty);
    return true;
}

void compositor_shutdown(void) {
    if (screen != NULL)
        gfx_surface_destroy(screen);
    screen = NULL;
    dirty_clear(&dirty);
}

gfx_surface_t* compositor_screen(void) { return screen; }
int32_t compositor_width(void)         { return dirty.width; }
int32_t compositor_height(void)        { return dirty.height; }
bool compositor_painting(void)         { return painting; }

void compositor_invalidate(gfx_rect_t rect) {
    if (screen != NULL)
        dirty_add(&dirty, rect);
}

void compositor_invalidate_all(void) {
    if (screen != NULL)
        dirty_add_all(&dirty);
}

bool compositor_dirty(void) {
    return !dirty_empty(&dirty);
}

//
// Cursor
//

static gfx_rect_t cursor_rect(void) {
    if (cursor_image != NULL)
        return gfx_rect(cursor_x, cursor_y, cursor_image->width, cursor_image->height);
    return gfx_rect(cursor_x, cursor_y, CURSOR_FALLBACK, CURSOR_FALLBACK);
}

void compositor_set_cursor(const image_t* image) {
    if (image == cursor_image)
        return;
    compositor_invalidate(cursor_rect());
    cursor_image = image;
    compositor_invalidate(cursor_rect());
}

void compositor_show_cursor(bool show) {
    if (show == !cursor_hidden)
        return;
    cursor_hidden = !show;
    compositor_invalidate(cursor_rect());
}

void compositor_move_cursor(int32_t x, int32_t y) {
    if (x == cursor_x && y == cursor_y)
        return;
    compositor_invalidate(cursor_rect());
    cursor_x = x;
    cursor_y = y;
    compositor_invalidate(cursor_rect());
}

//
// Painting
//

// Clip to `r` (screen coordinates) with the origin at the screen's corner
static void clip_screen(gfx_rect_t r) {
    gfx_set_origin(screen, 0, 0);
    gfx_set_clip(screen, &r);
}

static void draw_close_box(const dwm_window_t* w, gfx_rect_t box) {
    const image_t* image = imagelib_get("ui_close");
    if (image != NULL) {
        image_draw(screen, image, box.x, box.y);
        return;
    }
    (void)w;
    gfx_fill_rect(screen, box.x, box.y, box.w, box.h, GFX_RGB(176, 32, 32));
    gfx_draw_rect(screen, box.x, box.y, box.w, box.h, GFX_RGB(60, 8, 8));
    gfx_draw_line(screen, box.x + 3, box.y + 3, box.x + box.w - 4, box.y + box.h - 4, GFX_COLOR_WHITE);
    gfx_draw_line(screen, box.x + box.w - 4, box.y + 3, box.x + 3, box.y + box.h - 4, GFX_COLOR_WHITE);
}

static void draw_minimize_box(gfx_rect_t box) {
    const image_t* image = imagelib_get("ui_minimize");
    if (image != NULL) {
        image_draw(screen, image, box.x, box.y);
        return;
    }
    const dwm_theme_t* theme = dwm_theme();
    gfx_fill_rect(screen, box.x, box.y, box.w, box.h, theme->frame.background);
    gfx_draw_rect(screen, box.x, box.y, box.w, box.h, theme->frame.border);
    gfx_fill_rect(screen, box.x + 3, box.y + box.h - 4, box.w - 6, 2, theme->frame.title_text);
}

static void draw_frame(const dwm_window_t* w, gfx_rect_t area) {
    const dwm_theme_t* theme = dwm_theme();
    gfx_rect_t frame = window_frame(w);

    if (window_border(w) > 0)
        gfx_draw_rect(screen, frame.x, frame.y, frame.w, frame.h, theme->frame.border);

    gfx_rect_t bar = window_titlebar(w);
    if (bar.h == 0)
        return;

    bool focused = window_focused(w);
    gfx_fill_gradient(screen, bar.x, bar.y, bar.w, bar.h,
                      focused ? theme->frame.active_low  : theme->frame.inactive_low,
                      focused ? theme->frame.active_high : theme->frame.inactive_high,
                      GFX_GRADIENT_VERTICAL);
    gfx_draw_hline(screen, bar.x, bar.y + bar.h - 1, bar.w, theme->frame.border);

    // Title, cut short before the minimize and close boxes
    gfx_rect_t close = window_close_box(w);
    gfx_rect_t minimize = window_minimize_box(w);
    int32_t text_right = (minimize.w > 0) ? minimize.x - TITLE_PADDING
                       : (close.w > 0)    ? close.x - TITLE_PADDING
                                          : bar.x + bar.w - TITLE_PADDING;
    int32_t room = text_right - (bar.x + TITLE_PADDING);
    if (room > 0 && w->title[0] != '\0') {
        size_t len = dwm_text_fit(w->title, strlen(w->title), room);
        dwm_text_draw(screen, bar.x + TITLE_PADDING, bar.y + (bar.h - dwm_text_height()) / 2,
                      w->title, len, theme->frame.title_text);
    }

    if (minimize.w > 0 && rect_overlaps(area, minimize))
        draw_minimize_box(minimize);
    if (close.w > 0 && rect_overlaps(area, close))
        draw_close_box(w, close);
}

static void draw_grip(const dwm_window_t* w) {
    gfx_rect_t g = window_grip(w);
    if (g.w == 0)
        return;
    uint32_t color = dwm_theme()->frame.background;
    for (int32_t i = 3; i < DWM_GRIP_SIZE; i += 4)
        gfx_draw_line(screen, g.x + i, g.y + g.h - 1, g.x + g.w - 1, g.y + i, color);
}

static void paint_client(const dwm_window_t* w, gfx_rect_t area) {
    gfx_rect_t client = window_client(w);
    gfx_rect_t part = rect_intersect(area, client);
    if (rect_empty(part))
        return;

    clip_screen(part);
    gfx_clear(screen, dwm_theme()->client.background);

    gfx_save(screen);
    gfx_set_origin(screen, client.x, client.y);

    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = DWM_EVENT_PAINT;
    ev.surface = screen;
    ev.rect    = gfx_rect(part.x - client.x, part.y - client.y, part.w, part.h);
    window_call(w->handle, &ev);

    gfx_restore(screen);
    gfx_set_blend(screen, GFX_BLEND_COPY);      // In case the procedure left it changed
    gfx_set_text_scale(screen, 1);
}

static void paint(gfx_rect_t area) {
    clip_screen(area);
    background_paint(screen, area);     // The color, or the wallpaper
    icons_paint(screen, area);

    for (dwm_window_t* w = window_bottom(); w != NULL; w = w->above) {
        if (!w->visible)
            continue;
        gfx_rect_t frame = window_frame(w);
        if (!rect_overlaps(area, frame))
            continue;

        clip_screen(rect_intersect(area, frame));
        draw_frame(w, area);
        paint_client(w, area);

        clip_screen(rect_intersect(area, frame));
        draw_grip(w);
    }

    // The taskbar over every window, the start menu over it
    clip_screen(area);
    taskbar_paint(screen, area);
    start_paint(screen, area);

    // An open context menu, above everything
    clip_screen(area);
    menu_paint(screen, area);
    drag_paint(screen, area);       // What letting go of a drag does
    switcher_paint(screen, area);   // The Alt+Tab list

    // An admin prompt over all of it: nothing a program draws can cover it
    clip_screen(area);
    consent_paint(screen, area);

    gfx_rect_t c = cursor_rect();
    if (!cursor_hidden && rect_overlaps(area, c)) {
        clip_screen(area);
        if (cursor_image != NULL)
            image_draw(screen, cursor_image, c.x, c.y);
        else
            gfx_fill_rect(screen, c.x, c.y, c.w, c.h, GFX_COLOR_WHITE);
    }
}

int compositor_render(void) {
    if (screen == NULL || dirty_empty(&dirty))
        return 0;

    // Take the list: procedures may invalidate while painting, which then
    // goes into the next frame instead of the one being drawn
    dirty_t frame = dirty;
    dirty_clear(&dirty);

    int rc = 0;
    int err = 0;
    painting = true;
    for (size_t i = 0; i < frame.count; i++) {
        gfx_rect_t r = frame.rects[i];
        paint(r);
        if (gfx_present_rect(screen, &r, r.x, r.y) < 0 && rc == 0) {
            rc = -1;
            err = errno;
        }
    }
    painting = false;

    gfx_set_origin(screen, 0, 0);
    gfx_set_clip(screen, NULL);

    window_reap();
    if (rc < 0)
        errno = err;
    return rc;
}

