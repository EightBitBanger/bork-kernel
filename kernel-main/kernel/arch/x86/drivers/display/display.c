
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_display.h>

#include <kernel/console/display.h>
#include <kernel/malloc.h>

#include <kernel/util/string.h>
#include <kernel/util/timer.h>

//
// Text console on the display back buffer
//
// The console keeps its text as a grid of character cells, not as pixels:
//
//   text_cells    rows * columns characters, used as a RING of rows. Logical
//                 row r (0 = top of the screen) lives at physical row
//                 (ring_top + r) % rows. Scrolling one line is just
//                 ring_top++ and blanking the row that comes into view: O(1),
//                 no pixels move.
//
//   shown_cells   what is currently drawn on screen, per screen cell
//
//   row_dirty     screen rows that may differ from shown_cells
//
// display_present() compares the dirty rows against shown_cells and redraws
// only the cells that actually changed, then presents one span per changed
// row.
//
// Updates are batched: between display_begin_update() and
// display_end_update() nothing is drawn, so print() of a 40-line listing
// scrolls 40 times in the grid but repaints the screen once.
//
// Drawing goes through the console's OWN GfxContext on the display back
// surface, with its own font (the char ROM with a 9 pixel line), so nothing
// any other drawing code does to its contexts (clip, origin, target) can reach it.
//
// Text is drawn at CONSOLE_TEXT_SCALE, the same magnification the panic
// screen uses (PANIC_SCALE in panic/panic_error.c), so both read the same.
// Every size the console reports (glyph width/height, columns, rows) is in
// scaled pixels; nothing outside this file needs to know about the scale.
//

// Same as PANIC_SCALE
#define CONSOLE_TEXT_SCALE   2

// Unscaled cell height: 8 glyph rows plus one blank row between lines
#define CONSOLE_LINE_HEIGHT  9

// Kept global: other code reads the cursor directly
uint16_t cursor_position;   // Column
uint16_t cursor_line;       // Row

// Screen size in pixels (global: other code reads it directly)
uint32_t display_width;
uint32_t display_height;

// Console size in character cells
uint32_t console_width;     // Columns
uint32_t console_height;    // Rows

// Size of one character cell on screen, in pixels (scale applied)
uint8_t console_glyph_width  = 6 * CONSOLE_TEXT_SCALE;
uint8_t console_glyph_height = CONSOLE_LINE_HEIGHT * CONSOLE_TEXT_SCALE;

#define CONSOLE_FOREGROUND   0xFFCACACAu
#define CONSOLE_BACKGROUND   0xFF040404u

static GfxContext console_gc;
static GfxFont    console_font;

static uint8_t* text_cells   = NULL;
static uint8_t* shown_cells  = NULL;
static uint8_t* row_dirty    = NULL;
static uint32_t ring_top     = 0;
static uint32_t update_depth = 0;
static bool     repaint_all  = true;

// Blinking cursor: an underline bar in the bottom of the cursor cell (the
// blank pixel row between lines, plus the lowest glyph row)
#define CONSOLE_CURSOR_BLINK_MS  530
#define CONSOLE_CURSOR_HEIGHT    (2 * CONSOLE_TEXT_SCALE)

static bool     cursor_enabled    = false;
static bool     cursor_blink_on   = true;
static uint64_t cursor_blink_ms   = 0;

// Where the bar is drawn right now, so it can be erased when it moves
static bool     cursor_drawn      = false;
static uint16_t cursor_drawn_col  = 0;
static uint16_t cursor_drawn_line = 0;

static inline uint8_t* ring_row(uint32_t row) {
    return &text_cells[((ring_top + row) % console_height) * console_width];
}

// Anything the font has no glyph for is shown as '?'
static inline uint8_t cell_char(char ch) {
    uint8_t c = (uint8_t)ch;
    return (c >= console_font.first && c - console_font.first < console_font.count) ? c : (uint8_t)'?';
}

static void mark_all_rows_dirty(void) {
    memset(row_dirty, 1, console_height);
}

static void display_present(void) {
    if (text_cells == NULL || !gfx_display_ready())
        return;     // Nothing to draw into yet; the dirty state is kept

    const int32_t gw = console_glyph_width;
    const int32_t gh = console_glyph_height;
    bool full = repaint_all;

    if (full) {
        // Wipe everything, including the pixels outside the cell grid and
        // whatever was drawn here before the console owned the screen
        gfx_clear(&console_gc, CONSOLE_BACKGROUND);
        memset(shown_cells, ' ', (size_t)console_width * console_height);
        cursor_drawn = false;   // Wiped with everything else
        mark_all_rows_dirty();
        repaint_all = false;
    }

    for (uint32_t row = 0; row < console_height; row++) {
        if (!row_dirty[row])
            continue;
        row_dirty[row] = 0;

        const uint8_t* text = ring_row(row);
        uint8_t* shown = &shown_cells[row * console_width];
        int32_t first = -1, last = -1;

        for (uint32_t col = 0; col < console_width; col++) {
            if (text[col] == shown[col])
                continue;
            // The context's text scale fills the whole gw x gh cell
            gfx_draw_char_opaque(&console_gc, (int32_t)col * gw, (int32_t)row * gh, text[col],
                                 CONSOLE_FOREGROUND, CONSOLE_BACKGROUND);
            shown[col] = text[col];
            if (first < 0) first = (int32_t)col;
            last = (int32_t)col;
        }

        if (!full && first >= 0)
            gfx_display_present_rect(first * gw, (int32_t)row * gh, (last - first + 1) * gw, gh);
    }

    // Cursor, drawn over the cell text. shown_cells only tracks characters,
    // so the cell under an old cursor is redrawn from the text to erase it.
    bool want = cursor_enabled && cursor_blink_on &&
                cursor_position < console_width && cursor_line < console_height;
    bool moved = cursor_drawn_col != cursor_position || cursor_drawn_line != cursor_line;

    if (cursor_drawn && (!want || moved)) {
        uint8_t ch = ring_row(cursor_drawn_line)[cursor_drawn_col];
        int32_t x = (int32_t)cursor_drawn_col * gw;
        int32_t y = (int32_t)cursor_drawn_line * gh;
        gfx_draw_char_opaque(&console_gc, x, y, ch, CONSOLE_FOREGROUND, CONSOLE_BACKGROUND);
        shown_cells[cursor_drawn_line * console_width + cursor_drawn_col] = ch;
        if (!full)
            gfx_display_present_rect(x, y, gw, gh);
        cursor_drawn = false;
    }

    if (want) {
        // Redrawn every present: a glyph drawn into its cell above wipes it
        int32_t x = (int32_t)cursor_position * gw;
        int32_t y = (int32_t)cursor_line * gh;
        gfx_fill_rect(&console_gc, x, y + gh - CONSOLE_CURSOR_HEIGHT, gw, CONSOLE_CURSOR_HEIGHT,
                      CONSOLE_FOREGROUND);
        if (!full)
            gfx_display_present_rect(x, y, gw, gh);
        cursor_drawn      = true;
        cursor_drawn_col  = cursor_position;
        cursor_drawn_line = cursor_line;
    }

    if (full)
        gfx_display_present_all();
}

static inline void present_if_idle(void) {
    if (update_depth == 0)
        display_present();
}

// gfx_display_init() must have run first (kmain does that)
void display_init(void) {
    display_width  = (uint32_t)gfx_display_width();
    display_height = (uint32_t)gfx_display_height();

    // Unscaled font metrics; the context applies the scale when drawing
    console_font = *gfx_font_default();
    console_font.line_height = CONSOLE_LINE_HEIGHT;     // One blank pixel row between lines

    gfx_context_init(&console_gc, gfx_display_back());
    gfx_set_font(&console_gc, &console_font);
    gfx_set_text_scale(&console_gc, CONSOLE_TEXT_SCALE);

    // On-screen cell size, as the context will draw it
    console_glyph_width  = (uint8_t)gfx_context_char_width(&console_gc);
    console_glyph_height = (uint8_t)gfx_context_line_height(&console_gc);

    // Columns across, rows down
    console_width  = display_width  / console_glyph_width;
    console_height = display_height / console_glyph_height;

    cursor_line = 0;
    cursor_position = 0;

    free(text_cells);
    free(shown_cells);
    free(row_dirty);

    size_t cells = (size_t)console_width * console_height;
    text_cells  = (uint8_t*)malloc(cells);
    shown_cells = (uint8_t*)malloc(cells);
    row_dirty   = (uint8_t*)malloc(console_height);

    if (text_cells == NULL || shown_cells == NULL || row_dirty == NULL || cells == 0) {
        free(text_cells);
        free(shown_cells);
        free(row_dirty);
        text_cells = shown_cells = row_dirty = NULL;
        return;     // Console output is dropped; nothing else breaks
    }

    memset(text_cells, ' ', cells);
    ring_top     = 0;
    update_depth = 0;
    repaint_all  = true;
    cursor_drawn = false;
    mark_all_rows_dirty();
}

uint32_t display_get_device_address(void) {
    return 0;
}

void display_busy_wait(void) {}

uint16_t display_get_width(void) {
    return (uint16_t)display_width;
}

uint16_t display_get_height(void) {
    return (uint16_t)display_height;
}

// On-screen cell size in pixels (text scale included)
uint8_t display_get_glyph_width(void) {
    return console_glyph_width;
}

uint8_t display_get_glyph_height(void) {
    return console_glyph_height;
}

// Characters per line
uint16_t display_get_columns(void) {
    return console_width;
}

// Lines per screen
uint16_t display_get_rows(void) {
    return console_height;
}

// Write a character at the cursor. The cursor does not move.
void display_putc(const char ch) {
    if (text_cells == NULL || cursor_position >= console_width || cursor_line >= console_height)
        return;

    ring_row(cursor_line)[cursor_position] = cell_char(ch);
    row_dirty[cursor_line] = 1;
    present_if_idle();
}

// Scroll the text up one line. The cursor does not move.
void display_newline(void) {
    if (text_cells == NULL)
        return;

    ring_top = (ring_top + 1) % console_height;
    memset(ring_row(console_height - 1), ' ', console_width);
    mark_all_rows_dirty();
    present_if_idle();
}

void display_clear(void) {
    if (text_cells == NULL)
        return;

    memset(text_cells, ' ', (size_t)console_width * console_height);
    ring_top = 0;

    // Also wipes non-text pixels
    repaint_all = true;
    present_if_idle();
}

void display_invalidate(void) {
    repaint_all = true;
    present_if_idle();
}

void display_begin_update(void) {
    update_depth++;
}

void display_end_update(void) {
    if (update_depth == 0)
        return;
    if (--update_depth == 0)
        display_present();
}

void display_cursor_set_position(uint16_t position) {
    if (position >= console_width)
        position = console_width ? (uint16_t)(console_width - 1) : 0;
    cursor_position = position;
}

void display_cursor_set_line(uint16_t line) {
    if (line >= console_height)
        line = console_height ? (uint16_t)(console_height - 1) : 0;
    cursor_line = line;
}

uint16_t display_cursor_get_position(void) {
    return cursor_position;
}

uint16_t display_cursor_get_line(void) {
    return cursor_line;
}

void display_cursor_blink_reset(void) {
    cursor_blink_on = true;
    cursor_blink_ms = timer_get_ms();
}

void display_cursor_enable(bool enable) {
    if (cursor_enabled == enable)
        return;
    cursor_enabled = enable;
    display_cursor_blink_reset();
    present_if_idle();
}

void display_cursor_blink_tick(void) {
    if (!cursor_enabled)
        return;

    uint64_t now = timer_get_ms();
    if (now - cursor_blink_ms < CONSOLE_CURSOR_BLINK_MS)
        return;

    cursor_blink_ms = now;
    cursor_blink_on = !cursor_blink_on;
    present_if_idle();
}

void console_set_blink_rate(uint8_t rate) {
    (void)rate;
}
