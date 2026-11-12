/*
 * Alt+Tab window switcher.
 *
 * Alt+Tab opens a list of the windows that have a taskbar button, front
 * one first and minimized ones after the rest, with the next one picked.
 * Tab (Shift+Tab: back) moves down the list while Alt stays held; letting
 * go of Alt (KEY_ALT_UP) or Enter brings the picked window to the front,
 * restoring it when minimized. Esc, or a click, calls it off.
 *
 * The list is taken when it opens. A window that closes meanwhile is just
 * skipped when it is chosen (its line stays until the list closes).
 *
 * input.c gives every key to switcher_key() while it is open, and asks
 * switcher_mouse() first about the mouse. A kernel without KEY_ALT_UP
 * still works: the first mouse event that comes without Alt held chooses.
 */

#include <string.h>

#include "compositor.h"
#include "dirty.h"
#include "internal.h"
#include "theme.h"

#define MAX_ENTRIES     64
#define MAX_SHOWN       12      // Lines at once; the list scrolls past that
#define BORDER          1
#define PAD             6       // Inside the border
#define LINE_PAD_Y      3       // Above and below a title
#define LINE_PAD_X      10
#define MIN_WIDTH       240
#define MAX_WIDTH       480
#define HEADER          "Switch to"

static struct {
    bool         open;
    dwm_handle_t windows[MAX_ENTRIES];
    size_t       count;
    size_t       pick;
    size_t       first;         // First line shown
    gfx_rect_t   rect;          // Screen
} sw;

//
// The list
//

static bool listed(const dwm_window_t* w) {
    return w->handle != 0 && !w->doomed && !w->destroying && window_title_height(w) > 0 &&
           (w->visible || w->minimized);
}

// Front to back: the showing windows top down, then the minimized ones
static void collect(void) {
    sw.count = 0;
    dwm_window_t* top = window_bottom();
    while (top != NULL && top->above != NULL)
        top = top->above;
    for (int pass = 0; pass < 2; pass++) {
        for (dwm_window_t* w = top; w != NULL && sw.count < MAX_ENTRIES; w = w->below) {
            if (listed(w) && w->minimized == (pass == 1))
                sw.windows[sw.count++] = w->handle;
        }
    }
}

static int32_t line_height(void) {
    return dwm_text_height() + 2 * LINE_PAD_Y;
}

static size_t shown(void) {
    return sw.count < MAX_SHOWN ? sw.count : MAX_SHOWN;
}

static void place(void) {
    int32_t lh = line_height();
    int32_t w = dwm_text_width(HEADER, strlen(HEADER)) + 2 * LINE_PAD_X;
    for (size_t i = 0; i < sw.count; i++) {
        const char* title = dwm_window_get_title(sw.windows[i]);
        int32_t tw = dwm_text_width(title, strlen(title)) + 2 * LINE_PAD_X;
        if (tw > w)
            w = tw;
    }
    w += 2 * (BORDER + PAD);
    if (w < MIN_WIDTH) w = MIN_WIDTH;
    if (w > MAX_WIDTH) w = MAX_WIDTH;
    int32_t h = 2 * (BORDER + PAD) + lh * (int32_t)(shown() + 1);

    gfx_rect_t work = dwm_work_area();
    sw.rect = gfx_rect(work.x + (work.w - w) / 2, work.y + (work.h - h) / 2, w, h);
}

static void scroll_to_pick(void) {
    size_t n = shown();
    if (sw.pick < sw.first)
        sw.first = sw.pick;
    else if (sw.pick >= sw.first + n)
        sw.first = sw.pick + 1 - n;
}

static void step(bool back) {
    if (sw.count == 0)
        return;
    sw.pick = back ? (sw.pick + sw.count - 1) % sw.count : (sw.pick + 1) % sw.count;
    scroll_to_pick();
    compositor_invalidate(sw.rect);
}

//
// Opening and closing
//

bool switcher_open(void) {
    return sw.open;
}

void switcher_start(bool back) {
    if (sw.open) {
        step(back);
        return;
    }
    collect();
    if (sw.count == 0)
        return;
    sw.open  = true;
    sw.first = 0;
    // The window in front is first: start on the one after it (or before,
    // going back). Nothing focused (the desktop): the first one is the pick.
    dwm_window_t* front = window_get(sw.windows[0]);
    bool front_focused = front != NULL && front->visible && dwm_window_get_focus() == front->handle;
    sw.pick = 0;
    if (front_focused && sw.count > 1)
        sw.pick = back ? sw.count - 1 : 1;
    scroll_to_pick();
    place();
    compositor_invalidate(sw.rect);
}

static void close_list(void) {
    if (!sw.open)
        return;
    sw.open = false;
    compositor_invalidate(sw.rect);
}

void switcher_cancel(void) {
    close_list();
}

void switcher_choose(void) {
    if (!sw.open)
        return;
    dwm_handle_t window = sw.windows[sw.pick];
    close_list();
    dwm_window_t* w = window_get(window);
    if (w == NULL)
        return;                         // Closed while the list was up
    if (!w->visible)
        dwm_window_show(window, true);  // Restores a minimized one, raises and focuses
    dwm_window_raise(window);
    if (!(w->style & DWM_STYLE_NOFOCUS))
        dwm_window_focus(window);
}

//
// Input
//

void switcher_key(uint16_t key, uint8_t mods) {
    switch (key) {
        case KEY_TAB:       step((mods & INPUT_MOD_SHIFT) != 0); break;
        case KEY_DOWN:
        case KEY_RIGHT:     step(false);                         break;
        case KEY_UP:
        case KEY_LEFT:      step(true);                          break;
        case KEY_ENTER:
        case KEY_ALT_UP:    switcher_choose();                   break;
        case KEY_ESCAPE:    switcher_cancel();                   break;
        default:
            if (!(mods & INPUT_MOD_ALT))
                switcher_choose();      // Alt is up (a kernel that doesn't say so)
            break;
    }
}

bool switcher_mouse(uint8_t mods, uint8_t pressed) {
    if (!sw.open)
        return false;
    if (pressed != 0)
        switcher_cancel();
    else if (!(mods & INPUT_MOD_ALT))
        switcher_choose();
    return true;
}

//
// Painting
//

void switcher_paint(gfx_surface_t* s, gfx_rect_t area) {
    if (!sw.open || !rect_overlaps(area, sw.rect))
        return;

    const dwm_theme_t* theme = dwm_theme();
    const dwm_theme_menu_t* t = &theme->menu;
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_rect_t r = sw.rect;

    gfx_fill_rect(s, r.x + BORDER, r.y + BORDER, r.w - 2 * BORDER, r.h - 2 * BORDER, t->background);
    gfx_draw_rect(s, r.x, r.y, r.w, r.h, t->border);

    int32_t lh = line_height();
    int32_t x = r.x + BORDER + PAD, y = r.y + BORDER + PAD;
    int32_t w = r.w - 2 * (BORDER + PAD);

    dwm_text_draw(s, x + LINE_PAD_X, y + LINE_PAD_Y, HEADER, strlen(HEADER), theme->client.text_muted);
    gfx_draw_hline(s, x, y + lh - 1, w, t->separator);
    y += lh;

    size_t end = sw.first + shown();
    for (size_t i = sw.first; i < end; i++, y += lh) {
        dwm_window_t* win = window_get(sw.windows[i]);
        if (i == sw.pick) {
            gfx_fill_gradient(s, x, y, w, lh, theme->frame.active_low, theme->frame.active_high,
                              GFX_GRADIENT_VERTICAL);
        }
        if (win == NULL)
            continue;                   // Closed meanwhile: an empty line
        uint32_t color = win->minimized ? theme->client.text_muted : t->text;
        size_t len = dwm_text_fit(win->title, strlen(win->title), w - 2 * LINE_PAD_X);
        dwm_text_draw(s, x + LINE_PAD_X, y + LINE_PAD_Y, win->title, len, color);
    }

    gfx_set_blend(s, old);
}
