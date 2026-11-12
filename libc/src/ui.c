/*
 * Shared UI elements (os/ui.h): the edit box and label wrapping.
 *
 * The edit box keeps everything as byte offsets into its text: `cursor` is
 * where typing goes, `anchor` the other end of the selection. One function,
 * relayout(), turns the text and placement into the box's rectangle and
 * keeps the cursor scrolled into view; every change ends with it, so what
 * is drawn and what is hit-tested always agree.
 */

#include <string.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/ui.h>
#include "internal.h"

#define PAD_X   4       // Border to text, left and right
#define PAD_Y   3       // ... above and below

static inline size_t min_sz(size_t a, size_t b) { return a < b ? a : b; }
static inline size_t max_sz(size_t a, size_t b) { return a > b ? a : b; }

//
// Edit box: geometry
//

static size_t visible_chars(const ui_edit_t* e) {
    int32_t n = (e->rect.w - 2 * PAD_X) / (e->cw > 0 ? e->cw : 6);
    return n > 1 ? (size_t)n : 1;
}

static void fix_scroll(ui_edit_t* e) {
    size_t fit = visible_chars(e);
    if (e->scroll > e->cursor)
        e->scroll = e->cursor;
    if (e->cursor > e->scroll + fit)
        e->scroll = e->cursor - fit;
    // No empty space after the end while text is hidden on the left
    if (e->len < fit)
        e->scroll = 0;
    else if (e->scroll > e->len - fit)
        e->scroll = e->len - fit;
}

// Where the text's line is: the built-in line at text_y, or a taller font's
// line centered on it (as gfx text draws it)
static void text_line(const ui_edit_t* e, int32_t* top, int32_t* height) {
    int32_t h = __gfx_line_height();
    if (h < e->lh)
        h = e->lh;
    *top = e->text_y + (e->lh - h) / 2;
    *height = h;
}

static void relayout(ui_edit_t* e) {
    int32_t w = (int32_t)e->len * e->cw + 2 * PAD_X;
    if (w > e->max_w) w = e->max_w;
    if (w < e->min_w) w = e->min_w;

    int32_t x = e->center_x - w / 2;
    if (x + w > e->bounds.x + e->bounds.w) x = e->bounds.x + e->bounds.w - w;
    if (x < e->bounds.x)                   x = e->bounds.x;

    int32_t top, h;
    text_line(e, &top, &h);
    e->rect = gfx_rect(x, top - PAD_Y, w, h + 2 * PAD_Y);
    fix_scroll(e);
}

// The character boundary nearest to screen x
static size_t index_at(const ui_edit_t* e, int32_t x) {
    int32_t rel = x - (e->rect.x + PAD_X);
    if (rel < 0)
        return e->scroll > 0 ? e->scroll - 1 : 0;       // Left of the text: scroll that way
    size_t i = e->scroll + (size_t)((rel + e->cw / 2) / e->cw);
    return min_sz(i, e->len);
}

//
// Edit box: text changes
//

static size_t sel_lo(const ui_edit_t* e) { return min_sz(e->cursor, e->anchor); }
static size_t sel_hi(const ui_edit_t* e) { return max_sz(e->cursor, e->anchor); }

bool ui_edit_has_selection(const ui_edit_t* e) {
    return e->cursor != e->anchor;
}

static void remove_range(ui_edit_t* e, size_t from, size_t to) {
    if (to <= from)
        return;
    memmove(e->text + from, e->text + to, e->len - to + 1);     // Terminator included
    e->len -= to - from;
    e->cursor = e->anchor = from;
}

static void remove_selection(ui_edit_t* e) {
    remove_range(e, sel_lo(e), sel_hi(e));
}

static void insert_char(ui_edit_t* e, char c) {
    remove_selection(e);
    if (e->len >= e->max_len)
        return;
    memmove(e->text + e->cursor + 1, e->text + e->cursor, e->len - e->cursor + 1);
    e->text[e->cursor] = c;
    e->len++;
    e->cursor++;
    e->anchor = e->cursor;
}

// Cursor to `to`; the selection follows when extending, else it goes
static void move_to(ui_edit_t* e, size_t to, bool extend) {
    e->cursor = min_sz(to, e->len);
    if (!extend)
        e->anchor = e->cursor;
}

static bool word_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80;
}

// A password's words aren't shown, so its word moves go to the ends:
// stopping inside would tell where its spaces are
static size_t word_left(const ui_edit_t* e, size_t i) {
    if (e->flags & UI_EDIT_SECRET)
        return 0;
    while (i > 0 && !word_char(e->text[i - 1])) i--;
    while (i > 0 && word_char(e->text[i - 1])) i--;
    return i;
}

static size_t word_right(const ui_edit_t* e, size_t i) {
    if (e->flags & UI_EDIT_SECRET)
        return e->len;
    while (i < e->len && !word_char(e->text[i])) i++;
    while (i < e->len && word_char(e->text[i])) i++;
    return i;
}

//
// Edit box: public
//

void ui_edit_init(ui_edit_t* e, const char* text, size_t max_len, uint32_t flags) {
    memset(e, 0, sizeof(*e));
    e->max_len = (max_len == 0 || max_len > UI_EDIT_MAX - 1) ? UI_EDIT_MAX - 1 : max_len;
    e->flags = flags;
    if (text != NULL) {
        e->len = min_sz(strlen(text), e->max_len);
        memcpy(e->text, text, e->len);
    }
    e->text[e->len] = '\0';

    // Cells as gfx text draws them (a font's are wider than the built-in's);
    // the line stays the built-in one, which gfx text centers a font on
    gfx_info_t info;
    e->lh = (gfx_get_info(&info) == 0 && info.line_height != 0) ? (int32_t)info.line_height : 8;
    e->cw = __gfx_cell_width();
    e->min_w = e->max_w = 2 * PAD_X + e->cw;
    e->bounds = gfx_rect(-GFX_COORD_MAX / 2, -GFX_COORD_MAX / 2, GFX_COORD_MAX, GFX_COORD_MAX);
    ui_edit_select_all(e);
}

void ui_edit_place(ui_edit_t* e, int32_t center_x, int32_t text_y, int32_t min_w, int32_t max_w,
                   gfx_rect_t bounds) {
    e->center_x = center_x;
    e->text_y   = text_y;
    e->min_w    = min_w;
    e->max_w    = max_w > min_w ? max_w : min_w;
    e->bounds   = bounds;
    relayout(e);
}

void ui_edit_select_all(ui_edit_t* e) {
    e->anchor = 0;
    e->cursor = e->len;         // The end shows: that is where typing after it goes
    relayout(e);
}

ui_edit_result_t ui_edit_key(ui_edit_t* e, uint16_t key, uint8_t mods) {
    bool shift = (mods & INPUT_MOD_SHIFT) != 0;
    bool ctrl  = (mods & INPUT_MOD_CTRL) != 0;

    switch (key) {
        case KEY_ENTER:     return UI_EDIT_ACCEPT;
        case KEY_ESCAPE:    return UI_EDIT_CANCEL;

        case KEY_LEFT:
            if (ctrl)
                move_to(e, word_left(e, e->cursor), shift);
            else if (ui_edit_has_selection(e) && !shift)
                move_to(e, sel_lo(e), false);       // Collapse to its start
            else
                move_to(e, e->cursor > 0 ? e->cursor - 1 : 0, shift);
            break;
        case KEY_RIGHT:
            if (ctrl)
                move_to(e, word_right(e, e->cursor), shift);
            else if (ui_edit_has_selection(e) && !shift)
                move_to(e, sel_hi(e), false);
            else
                move_to(e, e->cursor + 1, shift);
            break;
        case KEY_HOME:      move_to(e, 0, shift);       break;
        case KEY_END:       move_to(e, e->len, shift);  break;

        case KEY_BACKSPACE:
            if (ui_edit_has_selection(e))
                remove_selection(e);
            else if (e->cursor > 0)
                remove_range(e, ctrl ? word_left(e, e->cursor) : e->cursor - 1, e->cursor);
            break;
        case KEY_DELETE:
            if (ui_edit_has_selection(e))
                remove_selection(e);
            else if (e->cursor < e->len)
                remove_range(e, e->cursor, ctrl ? word_right(e, e->cursor) : e->cursor + 1);
            break;

        default:
            if (ctrl && (key == 'a' || key == 'A' || key == 1)) {
                ui_edit_select_all(e);
                return UI_EDIT_HANDLED;
            }
            if (key < 0x20 || key >= 0x7F || (mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT)))
                return UI_EDIT_IGNORED;
            if ((e->flags & UI_EDIT_NAME) && key == '/')
                return UI_EDIT_HANDLED;             // Swallowed: can't be in a name
            insert_char(e, (char)key);
            break;
    }
    relayout(e);
    return UI_EDIT_HANDLED;
}

bool ui_edit_contains(const ui_edit_t* e, int32_t x, int32_t y) {
    return x >= e->rect.x && y >= e->rect.y && x < e->rect.x + e->rect.w && y < e->rect.y + e->rect.h;
}

bool ui_edit_click(ui_edit_t* e, int32_t x, int32_t y, uint8_t mods, uint8_t clicks) {
    if (!ui_edit_contains(e, x, y))
        return false;
    if (clicks >= 2) {
        ui_edit_select_all(e);
        return true;
    }
    move_to(e, index_at(e, x), (mods & INPUT_MOD_SHIFT) != 0);
    relayout(e);
    return true;
}

void ui_edit_drag(ui_edit_t* e, int32_t x) {
    move_to(e, index_at(e, x), true);
    relayout(e);
}

void ui_edit_paint(const ui_edit_t* e, gfx_surface_t* s, const ui_edit_style_t* st) {
    gfx_rect_t b = e->rect;
    bool saved = gfx_save(s) == 0;
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_COPY);

    gfx_fill_rect(s, b.x, b.y, b.w, b.h, st->background);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, st->border);

    // Text, selection and cursor stay inside the border
    if (saved) {
        gfx_rect_t inner = gfx_rect(b.x + 1, b.y + 1, b.w - 2, b.h - 2);
        gfx_intersect_clip(s, &inner);
    }

    int32_t tx = b.x + PAD_X, ty = e->text_y, cw = e->cw;
    int32_t line_y, line_h;
    text_line(e, &line_y, &line_h);
    size_t fit = visible_chars(e);
    size_t first = e->scroll, last = min_sz(e->len, e->scroll + fit);

    if (ui_edit_has_selection(e)) {
        size_t lo = max_sz(sel_lo(e), first), hi = min_sz(sel_hi(e), last);
        if (hi > lo)
            gfx_fill_rect(s, tx + (int32_t)(lo - first) * cw - 1, line_y - 1,
                          (int32_t)(hi - lo) * cw + 2, line_h + 2, st->selection);
    }
    if (last > first && (e->flags & UI_EDIT_SECRET)) {
        char stars[UI_EDIT_MAX];
        memset(stars, '*', last - first);
        gfx_draw_text_n(s, tx, ty, stars, last - first, st->text);
    } else if (last > first) {
        gfx_draw_text_n(s, tx, ty, e->text + first, last - first, st->text);
    }
    if (!ui_edit_has_selection(e))
        gfx_draw_vline(s, tx + (int32_t)(e->cursor - first) * cw, line_y - 1, line_h + 2, st->cursor);

    if (saved)
        gfx_restore(s);         // Clip and blend as they were
    else
        gfx_set_blend(s, old);
}

//
// Text wrapping
//

size_t ui_wrap_text(const char* text, size_t fit, ui_line_t* out, size_t max, bool* more) {
    size_t lines = 0, len = strlen(text);
    const char* p = text;
    if (fit == 0)
        fit = 1;
    *more = false;
    while (len > 0) {
        if (lines == max) {
            *more = true;
            break;
        }
        size_t n = len;
        if (n > fit) {
            n = fit;
            for (size_t i = fit; i > fit / 2; i--) {
                if (p[i - 1] == ' ') {
                    n = i;
                    break;
                }
            }
        }
        out[lines].text = p;
        out[lines].len  = n;
        lines++;
        p += n;
        len -= n;
    }
    return lines;
}

size_t ui_line_copy(const ui_line_t* line, bool cut, size_t fit, char* buf, size_t size) {
    if (size == 0)
        return 0;
    size_t len = min_sz(line->len, size - 1);
    memcpy(buf, line->text, len);
    while (len > 1 && buf[len - 1] == ' ')
        len--;                                  // The space it broke at
    if (cut && fit >= 2 && size > 2) {
        if (len > fit - 2) len = fit - 2;       // The rest doesn't fit either
        if (len > size - 3) len = size - 3;
        memcpy(buf + len, "..", 2);
        len += 2;
    }
    buf[len] = '\0';
    return len;
}

size_t ui_fit_text(const char* text, size_t fit, char* buf, size_t size) {
    if (size == 0)
        return 0;
    size_t len = strlen(text);
    if (fit > size - 1)
        fit = size - 1;
    if (len > fit) {
        size_t keep = fit >= 2 ? fit - 2 : 0;
        memcpy(buf, text, keep);
        memcpy(buf + keep, "..", fit - keep);
        len = fit;
    } else {
        memcpy(buf, text, len);
    }
    buf[len] = '\0';
    return len;
}
