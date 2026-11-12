#ifndef _OS_UI_H
#define _OS_UI_H

/*
 * Small UI elements shared by the window manager and programs, drawn with
 * os/gfx.h onto any surface (the dwm's screen, a program's window).
 *
 * Edit box: one line of editable text with a cursor and a selection.
 *
 *     ui_edit_t box;
 *     ui_edit_init(&box, "notes.txt", NAME_MAX, UI_EDIT_NAME);   // all selected
 *     ui_edit_place(&box, tile_center_x, label_y, 30, 140, window_area);
 *
 *     // key from the event:
 *     switch (ui_edit_key(&box, ev.key, ev.mods)) {
 *         case UI_EDIT_ACCEPT: ... box.text ...; break;
 *         case UI_EDIT_CANCEL: ...; break;
 *         default: repaint;
 *     }
 *     // mouse down / moves with the button held:
 *     if (ui_edit_click(&box, x, y, mods, clicks)) ...   // inside: cursor moved
 *     ui_edit_drag(&box, x);                              // extend the selection
 *
 *     ui_edit_paint(&box, surface, &style);
 *
 * Keys: Left / Right (Ctrl: by word), Home / End move the cursor, with
 * Shift they extend the selection; Backspace / Delete remove the selection
 * or one character; Ctrl+A selects all; printable ASCII replaces the
 * selection or is inserted at the cursor. Enter accepts, Escape cancels.
 * Mouse: a click puts the cursor at the nearest character boundary (Shift:
 * extends the selection), a double click selects everything, a drag
 * selects.
 *
 * The box sizes itself to its text, between min_w and max_w, centered on a
 * point and kept inside an area (ui_edit_place; each edit re-sizes it, so
 * `rect` is always where it is drawn). A text longer than the box scrolls
 * to keep the cursor in view.
 *
 * Text wrapping: ui_wrap_text breaks a label into lines of at most `fit`
 * characters, after a space where one is near; ui_line_copy turns a line
 * into text to draw; ui_fit_text cuts a single line with "..".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>

//
// Edit box
//

#define UI_EDIT_MAX     256         // Bytes of text, terminator included

// Flags
#define UI_EDIT_NAME    0x0001      // A file name: no '/'
#define UI_EDIT_SECRET  0x0002      // A password: drawn as '*', Ctrl+Left / Right jump to the ends

typedef enum {
    UI_EDIT_IGNORED = 0,            // Not a key the box uses
    UI_EDIT_HANDLED,                // Used (text, cursor or selection may have changed)
    UI_EDIT_ACCEPT,                 // Enter
    UI_EDIT_CANCEL,                 // Escape
} ui_edit_result_t;

typedef struct {
    uint32_t background;
    uint32_t border;
    uint32_t text;
    uint32_t cursor;
    uint32_t selection;
} ui_edit_style_t;

typedef struct {
    char       text[UI_EDIT_MAX];
    size_t     len;
    size_t     max_len;             // At most UI_EDIT_MAX - 1
    uint32_t   flags;               // UI_EDIT_*
    size_t     cursor;              // 0 .. len
    size_t     anchor;              // Other end of the selection (== cursor: none)
    size_t     scroll;              // First character shown
    gfx_rect_t rect;                // Where it is drawn (ui_edit_place)

    // Placement (ui_edit_place)
    int32_t    center_x, text_y, min_w, max_w;
    gfx_rect_t bounds;
    int32_t    cw, lh;              // Font cell
} ui_edit_t;

// Start editing `text` (cut to max_len; 0: as much as fits), all of it
// selected, so typing replaces it
void ui_edit_init(ui_edit_t* e, const char* text, size_t max_len, uint32_t flags);

// Size and place the box: centered on center_x, its text's top at text_y,
// min_w .. max_w wide, moved sideways to stay inside `bounds`. Cheap; call
// it again whenever what it is placed on moves.
void ui_edit_place(ui_edit_t* e, int32_t center_x, int32_t text_y, int32_t min_w, int32_t max_w,
                   gfx_rect_t bounds);

ui_edit_result_t ui_edit_key(ui_edit_t* e, uint16_t key, uint8_t mods);

// A button went down at (x, y). False when that is outside the box
// (nothing changes). clicks: 2 for a double click.
bool ui_edit_click(ui_edit_t* e, int32_t x, int32_t y, uint8_t mods, uint8_t clicks);

// The pointer moved to x while the button that clicked in the box is held
void ui_edit_drag(ui_edit_t* e, int32_t x);

bool ui_edit_contains(const ui_edit_t* e, int32_t x, int32_t y);
bool ui_edit_has_selection(const ui_edit_t* e);
void ui_edit_select_all(ui_edit_t* e);

// Draw it at `rect`. The surface's clip and blend mode are left as they were.
void ui_edit_paint(const ui_edit_t* e, gfx_surface_t* s, const ui_edit_style_t* style);

//
// Text wrapping
//

typedef struct {
    const char* text;
    size_t      len;
} ui_line_t;

// Break `text` into lines of at most `fit` characters. How many (at most
// `max`); *more: some text didn't fit in `max` lines.
size_t ui_wrap_text(const char* text, size_t fit, ui_line_t* out, size_t max, bool* more);

// A line from ui_wrap_text as text to draw into buf: the space it broke at
// dropped, and ".." at the end when `cut` (the last line, with *more).
// Returns its length.
size_t ui_line_copy(const ui_line_t* line, bool cut, size_t fit, char* buf, size_t size);

// `text` on one line of at most `fit` characters, ending ".." when cut.
// Returns its length.
size_t ui_fit_text(const char* text, size_t fit, char* buf, size_t size);

#endif
