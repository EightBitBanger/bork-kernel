/*
 * minesweeper: the classic mine-clearing game, for the desktop.
 *
 *     minesweeper [beginner | intermediate | expert | COLUMNS ROWS MINES]
 *
 * Clear the field without stepping on a mine. Each uncovered square shows
 * how many of its eight neighbours hide a mine; a square with none opens up
 * the area around it. The first click is always safe. The counter on the
 * left is the mines left to find (mines minus flags), the one on the right
 * the seconds since the first click. Click the face for a new game.
 *
 * Levels: Beginner 9 x 9 with 10 mines, Intermediate 16 x 16 with 40,
 * Expert 30 x 16 with 99, or a custom field from the command line (9 to 30
 * columns, 9 to 24 rows, 10 mines up to (columns - 1) * (rows - 1)).
 *
 * Mouse: left click uncovers a square; right click puts a flag on it (and a
 * question mark next, when Marks are on). Both buttons together, the middle
 * button, or a left click on a number uncovers its neighbours once the
 * right number of flags is around it. Right click outside the field for the
 * Game menu (new game, level, marks).
 *
 * Keys: arrows move a cursor over the field, Space or Enter uncovers (or
 * clears around a number), F flags, N or Escape starts a new game, 1 2 3
 * pick the level, M turns question marks on or off.
 *
 * Drawn in the classic grey look, with its own pixel digits, so it looks
 * the same in any desktop font. Built on os/wm.h, the same way as notepad
 * and calc. Needs SEC_GFX and SEC_IPC (the default clearance has both).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/os.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

#ifndef APP_NAME
#define APP_NAME        "minesweeper"
#endif

// Field limits (the same as the classic game's custom field)
#define MIN_COLS        9
#define MAX_COLS        30
#define MIN_ROWS        9
#define MAX_ROWS        24
#define MIN_MINES       10
#define MAX_CELLS       (MAX_COLS * MAX_ROWS)

// Layout
#define CELL_BIG        20          // Square size when the field fits on the screen
#define CELL_SMALL      16          // Otherwise
#define PAD             6           // Grey margin around the panels
#define OUTER           3           // Raised edge around the window
#define HEADER_BEVEL    2
#define HEADER_H        37          // Header panel, inside its sunken edge
#define BOARD_BEVEL     3
#define FACE_SIZE       26
#define LED_DIGIT_W     13
#define LED_DIGIT_H     23
#define LED_W           (3 * LED_DIGIT_W + 2)
#define LED_H           (LED_DIGIT_H + 2)
#define MAX_SECONDS     999

// Classic colours
#define C_FACE          GFX_RGB(192, 192, 192)
#define C_LIGHT         GFX_COLOR_WHITE
#define C_SHADOW        GFX_RGB(128, 128, 128)
#define C_BLACK         GFX_COLOR_BLACK
#define C_EXPLODED      GFX_RGB(255, 0, 0)
#define C_FLAG          GFX_RGB(255, 0, 0)
#define C_LED_ON        GFX_RGB(255, 0, 0)
#define C_LED_OFF       GFX_RGB(64, 0, 0)
#define C_SMILEY        GFX_RGB(255, 255, 0)
#define C_CURSOR        GFX_RGB(0, 0, 255)

static const gfx_color_t number_colors[9] = {
    0,
    GFX_RGB(0, 0, 255),         // 1
    GFX_RGB(0, 128, 0),         // 2
    GFX_RGB(255, 0, 0),         // 3
    GFX_RGB(0, 0, 128),         // 4
    GFX_RGB(128, 0, 0),         // 5
    GFX_RGB(0, 128, 128),       // 6
    GFX_RGB(0, 0, 0),           // 7
    GFX_RGB(128, 128, 128),     // 8
};

enum {
    CMD_NEW = 1,
    CMD_BEGINNER,
    CMD_INTERMEDIATE,
    CMD_EXPERT,
    CMD_MARKS,
    CMD_EXIT,
};

typedef enum {
    LEVEL_BEGINNER,
    LEVEL_INTERMEDIATE,
    LEVEL_EXPERT,
    LEVEL_CUSTOM,
} level_t;

static const struct {
    const char* name;
    int         cols, rows, mines;
} levels[3] = {
    { "beginner",      9,  9, 10 },
    { "intermediate", 16, 16, 40 },
    { "expert",       30, 16, 99 },
};

typedef enum {
    COVERED,
    UNCOVERED,
    FLAGGED,
    QUESTION,
} cell_state_t;

typedef enum {
    GAME_READY,         // No click yet: mines are laid on the first one
    GAME_PLAYING,
    GAME_WON,
    GAME_LOST,
} game_state_t;

//
// State
//

static struct {
    level_t      level;
    int          cols, rows, mines;
    bool         marks;                 // Right click cycles through a question mark too
    game_state_t state;
    uint8_t      mine[MAX_CELLS];
    uint8_t      count[MAX_CELLS];      // Mines among the neighbours
    uint8_t      cell[MAX_CELLS];       // cell_state_t
    uint8_t      wrong[MAX_CELLS];      // Lost: a flag with no mine under it
    int          exploded;              // Lost: the square that went off, -1: none
    int          flags;
    int          uncovered;
    uint64_t     start_ms;
    int          seconds;
    int          best[3];               // Best time per level this session, 0: none
} game;

// Mouse and keyboard
static struct {
    bool left, right, middle;           // Held, pressed over the window
    bool chording;                      // Both buttons (or the middle one) went down over the field
    bool swallow;                       // A chord just ended: ignore releases until all are up
    bool face_down;                     // Left button went down on the face
    int  hot;                           // Square under the pointer while a button is held, -1: none
    bool on_face;                       // Pointer over the face
    bool cursor_shown;                  // The keyboard cursor is drawn
    int  cursor;                        // Its square
} ui = { .hot = -1 };

static wm_window_t  main_win;
static bool         running = true;
static bool         dirty;
static int          cell = CELL_BIG;
static uint32_t     rng_state;

//
// Random numbers (xorshift32: libc's rand() range is too small to rely on)
//

static void rng_seed(uint32_t seed) {
    rng_state = seed ? seed : 0x9E3779B9u;
}

static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

// 0 .. n - 1
static int rng_below(int n) {
    return (int)(rng_next() % (uint32_t)n);
}

//
// The game
//

static int cells(void) {
    return game.cols * game.rows;
}

static int idx(int col, int row) {
    return row * game.cols + col;
}

// The neighbours of square i into out[8]; how many
static int neighbours(int i, int* out) {
    int col = i % game.cols, row = i / game.cols, n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int c = col + dx, r = row + dy;
            if ((dx || dy) && c >= 0 && c < game.cols && r >= 0 && r < game.rows)
                out[n++] = idx(c, r);
        }
    }
    return n;
}

static void new_game(void) {
    memset(game.mine, 0, sizeof(game.mine));
    memset(game.count, 0, sizeof(game.count));
    memset(game.cell, COVERED, sizeof(game.cell));
    memset(game.wrong, 0, sizeof(game.wrong));
    game.state = GAME_READY;
    game.exploded = -1;
    game.flags = 0;
    game.uncovered = 0;
    game.seconds = 0;
    ui.hot = -1;
    ui.face_down = false;
    if (ui.cursor >= cells())
        ui.cursor = 0;
    dirty = true;
}

// Lay the mines, keeping `safe` (and its neighbours, when there is room) free
static void lay_mines(int safe) {
    static int pool[MAX_CELLS];
    int nb[8], nn = neighbours(safe, nb);
    bool keep_area = cells() - (nn + 1) >= game.mines;

    int n = 0;
    for (int i = 0; i < cells(); i++) {
        bool excluded = i == safe;
        for (int k = 0; keep_area && k < nn && !excluded; k++)
            excluded = nb[k] == i;
        if (!excluded)
            pool[n++] = i;
    }
    // A partial Fisher-Yates shuffle: the first `mines` entries are the mines
    for (int k = 0; k < game.mines && k < n; k++) {
        int j = k + rng_below(n - k);
        int t = pool[k]; pool[k] = pool[j]; pool[j] = t;
        game.mine[pool[k]] = 1;
    }
    for (int i = 0; i < cells(); i++) {
        int m = neighbours(i, nb), c = 0;
        for (int k = 0; k < m; k++)
            c += game.mine[nb[k]];
        game.count[i] = (uint8_t)c;
    }
}

static void update_clock(void) {
    if (game.state != GAME_PLAYING)
        return;
    uint64_t s = 1 + (os_time_ms() - game.start_ms) / 1000;     // Starts at 1, as it always has
    int secs = s > MAX_SECONDS ? MAX_SECONDS : (int)s;
    if (secs != game.seconds) {
        game.seconds = secs;
        dirty = true;
    }
}

static void win(void) {
    game.state = GAME_WON;
    for (int i = 0; i < cells(); i++)
        if (game.mine[i])
            game.cell[i] = FLAGGED;
    game.flags = game.mines;
    dirty = true;

    char text[96];
    const char* unit = game.seconds == 1 ? "second" : "seconds";
    if (game.level == LEVEL_CUSTOM) {
        snprintf(text, sizeof(text), "Cleared in %d %s.", game.seconds, unit);
    } else {
        int* best = &game.best[game.level];
        if (*best == 0 || game.seconds < *best) {
            bool first = *best == 0;
            *best = game.seconds;
            snprintf(text, sizeof(text), "Cleared in %d %s%s.", game.seconds, unit,
                     first ? "" : ", a new best time");
        } else {
            snprintf(text, sizeof(text), "Cleared in %d %s. Best: %d.", game.seconds, unit, *best);
        }
    }
    wm_notify("You win!", text);
}

static void lose(int at) {
    game.state = GAME_LOST;
    game.exploded = at;
    for (int i = 0; i < cells(); i++) {
        if (game.mine[i] && game.cell[i] != FLAGGED)
            game.cell[i] = UNCOVERED;
        else if (!game.mine[i] && game.cell[i] == FLAGGED)
            game.wrong[i] = 1;
    }
    dirty = true;
}

// Uncover square i, opening up the area around squares with no mines near
static void uncover(int i) {
    if (game.state == GAME_WON || game.state == GAME_LOST)
        return;
    if (game.cell[i] != COVERED && game.cell[i] != QUESTION)
        return;
    if (game.state == GAME_READY) {
        lay_mines(i);
        game.state = GAME_PLAYING;
        game.start_ms = os_time_ms();
        game.seconds = 0;
        update_clock();
    }
    if (game.mine[i]) {
        lose(i);
        return;
    }

    static int stack[MAX_CELLS];
    int sp = 0;
    game.cell[i] = UNCOVERED;
    game.uncovered++;
    stack[sp++] = i;
    while (sp > 0) {
        int j = stack[--sp];
        if (game.count[j] != 0)
            continue;
        int nb[8], n = neighbours(j, nb);
        for (int k = 0; k < n; k++) {
            int c = nb[k];
            if (game.cell[c] == COVERED || game.cell[c] == QUESTION) {
                game.cell[c] = UNCOVERED;
                game.uncovered++;
                stack[sp++] = c;            // Each square goes on at most once
            }
        }
    }
    dirty = true;
    if (game.uncovered == cells() - game.mines)
        win();
}

// A number with as many flags around it: uncover the rest of its neighbours
static void chord(int i) {
    if (game.state != GAME_PLAYING || game.cell[i] != UNCOVERED || game.count[i] == 0)
        return;
    int nb[8], n = neighbours(i, nb), flagged = 0;
    for (int k = 0; k < n; k++)
        flagged += game.cell[nb[k]] == FLAGGED;
    if (flagged != game.count[i])
        return;
    for (int k = 0; k < n && game.state == GAME_PLAYING; k++)
        uncover(nb[k]);
}

static void toggle_flag(int i) {
    if (game.state == GAME_WON || game.state == GAME_LOST)
        return;
    switch (game.cell[i]) {
        case COVERED:
            game.cell[i] = FLAGGED;
            game.flags++;
            break;
        case FLAGGED:
            game.cell[i] = game.marks ? QUESTION : COVERED;
            game.flags--;
            break;
        case QUESTION:
            game.cell[i] = COVERED;
            break;
    }
    dirty = true;
}

// Uncover with the mouse or keyboard: a covered square, or around a number
static void open_square(int i) {
    if (game.cell[i] == UNCOVERED)
        chord(i);
    else
        uncover(i);
}

//
// Layout
//

static int32_t client_width(void) {
    return 2 * (OUTER + PAD + BOARD_BEVEL) + game.cols * cell;
}

static int32_t client_height(void) {
    return 2 * OUTER + 3 * PAD + 2 * HEADER_BEVEL + HEADER_H + 2 * BOARD_BEVEL + game.rows * cell;
}

static gfx_rect_t header_rect(void) {          // Inside its edge
    int32_t x = OUTER + PAD + HEADER_BEVEL;
    return gfx_rect(x, OUTER + PAD + HEADER_BEVEL, client_width() - 2 * x, HEADER_H);
}

static gfx_rect_t board_rect(void) {           // The squares only
    int32_t y = OUTER + 2 * PAD + 2 * HEADER_BEVEL + HEADER_H + BOARD_BEVEL;
    return gfx_rect(OUTER + PAD + BOARD_BEVEL, y, game.cols * cell, game.rows * cell);
}

static gfx_rect_t face_rect(void) {
    gfx_rect_t h = header_rect();
    return gfx_rect(h.x + (h.w - FACE_SIZE) / 2, h.y + (h.h - FACE_SIZE) / 2, FACE_SIZE, FACE_SIZE);
}

static bool in_rect(const gfx_rect_t* r, int32_t x, int32_t y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

// The square at (x, y), -1: outside the field
static int square_at(int32_t x, int32_t y) {
    gfx_rect_t b = board_rect();
    if (!in_rect(&b, x, y))
        return -1;
    return idx((x - b.x) / cell, (y - b.y) / cell);
}

// Big squares unless the window would not fit on the screen
static void pick_cell_size(void) {
    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    cell = CELL_BIG;
    if (sw > 0 && sh > 0 && (client_width() + 16 > sw || client_height() + 48 > sh))
        cell = CELL_SMALL;
}

static void set_field(level_t level, int cols, int rows, int mines) {
    game.level = level;
    game.cols  = cols;
    game.rows  = rows;
    game.mines = mines;
    new_game();
}

static void set_level(level_t level) {
    set_field(level, levels[level].cols, levels[level].rows, levels[level].mines);
    pick_cell_size();
    if (main_win != 0)
        wm_window_resize(main_win, client_width(), client_height());
}

//
// Drawing
//

// A 3D edge `width` pixels thick around (x, y, w, h), drawn inside it
static void bevel(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h, int width, bool raised) {
    gfx_color_t tl = raised ? C_LIGHT : C_SHADOW;
    gfx_color_t br = raised ? C_SHADOW : C_LIGHT;
    for (int i = 0; i < width; i++) {
        gfx_draw_hline(s, x + i, y + i, w - 2 * i - 1, tl);
        gfx_draw_vline(s, x + i, y + i, h - 2 * i - 1, tl);
        gfx_draw_hline(s, x + i + 1, y + h - 1 - i, w - 2 * i - 1, br);
        gfx_draw_vline(s, x + w - 1 - i, y + i + 1, h - 2 * i - 1, br);
    }
}

// 5 x 7 glyphs for the numbers and the question mark, bit 4 the leftmost
static const uint8_t glyphs[10][7] = {
    { 0 },
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },     // 1
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },     // 2
    { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },     // 3
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },     // 4
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },     // 5
    { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E },     // 6
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },     // 7
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },     // 8
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 },     // ?
};
#define GLYPH_QUESTION  9

static void draw_glyph(gfx_surface_t* s, int32_t x, int32_t y, int g, gfx_color_t color) {
    int scale = cell >= CELL_BIG ? 2 : 1;
    int32_t ox = x + (cell - 5 * scale) / 2;
    int32_t oy = y + (cell - 7 * scale) / 2;
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++)
            if (glyphs[g][r] & (0x10 >> c))
                gfx_fill_rect(s, ox + c * scale, oy + r * scale, scale, scale, color);
    if (scale == 1)                     // Small squares: thicken the strokes
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (glyphs[g][r] & (0x10 >> c))
                    gfx_set_pixel(s, ox + c + 1, oy + r, color);
}

static void draw_mine(gfx_surface_t* s, int32_t x, int32_t y) {
    int32_t cx = x + cell / 2, cy = y + cell / 2;
    int r = cell / 4;
    gfx_fill_circle(s, cx, cy, r, C_BLACK);
    gfx_draw_hline(s, cx - r - 2, cy, 2 * r + 5, C_BLACK);
    gfx_draw_vline(s, cx, cy - r - 2, 2 * r + 5, C_BLACK);
    for (int d = r - 1; d <= r; d++) {
        gfx_set_pixel(s, cx - d, cy - d, C_BLACK);
        gfx_set_pixel(s, cx + d, cy - d, C_BLACK);
        gfx_set_pixel(s, cx - d, cy + d, C_BLACK);
        gfx_set_pixel(s, cx + d, cy + d, C_BLACK);
    }
    gfx_fill_rect(s, cx - r / 2 - 1, cy - r / 2 - 1, 2, 2, C_LIGHT);   // The shine
}

static void draw_flag(gfx_surface_t* s, int32_t x, int32_t y) {
    int32_t pole = x + cell / 2 + 1;
    int32_t top = y + cell / 5;
    int32_t foot = y + cell - cell / 5;
    // The flag: a triangle against the pole, pointing left
    for (int i = 0; i < 7; i++) {
        int w = 7 - 2 * abs(i - 3);
        gfx_draw_hline(s, pole - w, top + i, w, C_FLAG);
    }
    gfx_draw_vline(s, pole, top, foot - top, C_BLACK);
    gfx_draw_hline(s, pole - 3, foot - 2, 7, C_BLACK);
    gfx_fill_rect(s, pole - 5, foot - 1, 11, 2, C_BLACK);
}

static void draw_cross(gfx_surface_t* s, int32_t x, int32_t y) {
    int m = cell / 5;
    gfx_draw_line(s, x + m, y + m, x + cell - 1 - m, y + cell - 1 - m, C_EXPLODED);
    gfx_draw_line(s, x + m + 1, y + m, x + cell - m, y + cell - 1 - m, C_EXPLODED);
    gfx_draw_line(s, x + cell - 1 - m, y + m, x + m, y + cell - 1 - m, C_EXPLODED);
    gfx_draw_line(s, x + cell - m, y + m, x + m + 1, y + cell - 1 - m, C_EXPLODED);
}

// Is square i shown pushed in, because a button is held over it?
static bool pushed(int i) {
    if (ui.hot < 0 || ui.swallow || game.state == GAME_WON || game.state == GAME_LOST)
        return false;
    if (game.cell[i] != COVERED && game.cell[i] != QUESTION)
        return false;
    if (ui.chording) {
        int hc = ui.hot % game.cols, hr = ui.hot / game.cols;
        int c = i % game.cols, r = i / game.cols;
        return abs(c - hc) <= 1 && abs(r - hr) <= 1;
    }
    return ui.left && i == ui.hot;
}

static void draw_square(gfx_surface_t* s, int i, int32_t x, int32_t y) {
    bool open = game.cell[i] == UNCOVERED || game.wrong[i] || pushed(i);
    if (!open) {
        gfx_fill_rect(s, x, y, cell, cell, C_FACE);
        bevel(s, x, y, cell, cell, cell >= CELL_BIG ? 3 : 2, true);
        if (game.cell[i] == FLAGGED)
            draw_flag(s, x, y);
        else if (game.cell[i] == QUESTION)
            draw_glyph(s, x, y, GLYPH_QUESTION, C_BLACK);
        return;
    }

    gfx_fill_rect(s, x, y, cell, cell, i == game.exploded ? C_EXPLODED : C_FACE);
    gfx_draw_hline(s, x, y, cell, C_SHADOW);
    gfx_draw_vline(s, x, y, cell, C_SHADOW);
    if (game.wrong[i]) {
        draw_mine(s, x, y);
        draw_cross(s, x, y);
    } else if (game.cell[i] == UNCOVERED) {
        if (game.mine[i])
            draw_mine(s, x, y);
        else if (game.count[i] > 0)
            draw_glyph(s, x, y, game.count[i], number_colors[game.count[i]]);
    } else if (game.cell[i] == QUESTION) {
        draw_glyph(s, x, y, GLYPH_QUESTION, C_BLACK);   // Pushed in, mark still showing
    }
}

static void draw_board(gfx_surface_t* s) {
    gfx_rect_t b = board_rect();
    bevel(s, b.x - BOARD_BEVEL, b.y - BOARD_BEVEL, b.w + 2 * BOARD_BEVEL, b.h + 2 * BOARD_BEVEL,
          BOARD_BEVEL, false);
    for (int r = 0; r < game.rows; r++)
        for (int c = 0; c < game.cols; c++)
            draw_square(s, idx(c, r), b.x + c * cell, b.y + r * cell);

    if (ui.cursor_shown) {
        int32_t x = b.x + (ui.cursor % game.cols) * cell;
        int32_t y = b.y + (ui.cursor / game.cols) * cell;
        gfx_draw_rect(s, x + 1, y + 1, cell - 2, cell - 2, C_CURSOR);
        gfx_draw_rect(s, x + 2, y + 2, cell - 4, cell - 4, C_CURSOR);
    }
}

// Seven-segment digits. Segments a..g are bits 0..6.
static const uint8_t led_segments[11] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
    0x40,                               // '-'
};

static void draw_led_digit(gfx_surface_t* s, int32_t x, int32_t y, int d) {
    static const struct { int8_t x, y, w, h; } seg[7] = {
        {  2,  1, 9, 2 },               // a
        { 11,  2, 2, 9 },               // b
        { 11, 12, 2, 9 },               // c
        {  2, 20, 9, 2 },               // d
        {  0, 12, 2, 9 },               // e
        {  0,  2, 2, 9 },               // f
        {  2, 10, 9, 2 },               // g
    };
    for (int k = 0; k < 7; k++) {
        gfx_color_t c = (led_segments[d] & (1 << k)) ? C_LED_ON : C_LED_OFF;
        gfx_fill_rect(s, x + seg[k].x, y + seg[k].y, seg[k].w, seg[k].h, c);
    }
}

// Three digits, -99 .. 999
static void draw_led(gfx_surface_t* s, int32_t x, int32_t y, int value) {
    if (value > 999) value = 999;
    if (value < -99) value = -99;
    gfx_fill_rect(s, x, y, LED_W, LED_H, C_BLACK);
    bevel(s, x - 1, y - 1, LED_W + 2, LED_H + 2, 1, false);
    int d[3];
    if (value < 0) {
        d[0] = 10;
        d[1] = (-value) / 10;
        d[2] = (-value) % 10;
    } else {
        d[0] = value / 100;
        d[1] = value / 10 % 10;
        d[2] = value % 10;
    }
    for (int k = 0; k < 3; k++)
        draw_led_digit(s, x + 1 + k * LED_DIGIT_W, y + 1, d[k]);
}

static void draw_face(gfx_surface_t* s) {
    gfx_rect_t f = face_rect();
    bool down = ui.face_down && ui.on_face;
    gfx_fill_rect(s, f.x, f.y, f.w, f.h, C_FACE);
    gfx_draw_rect(s, f.x, f.y, f.w, f.h, C_SHADOW);
    bevel(s, f.x + 1, f.y + 1, f.w - 2, f.h - 2, 2, !down);

    int32_t cx = f.x + f.w / 2 + down, cy = f.y + f.h / 2 + down;
    gfx_fill_circle(s, cx, cy, 8, C_SMILEY);
    gfx_draw_circle(s, cx, cy, 8, C_BLACK);

    // Surprised while a square is being pressed
    bool worried = game.state != GAME_WON && game.state != GAME_LOST && ui.hot >= 0 &&
                   (ui.left || ui.chording) && !ui.swallow;
    if (game.state == GAME_LOST) {
        // Crossed-out eyes and a frown
        for (int e = -1; e <= 1; e += 2) {
            int32_t ex = cx + e * 3;
            gfx_draw_line(s, ex - 1, cy - 4, ex + 1, cy - 2, C_BLACK);
            gfx_draw_line(s, ex + 1, cy - 4, ex - 1, cy - 2, C_BLACK);
        }
        gfx_draw_hline(s, cx - 2, cy + 2, 5, C_BLACK);
        gfx_set_pixel(s, cx - 3, cy + 3, C_BLACK);
        gfx_set_pixel(s, cx + 3, cy + 3, C_BLACK);
        gfx_set_pixel(s, cx - 4, cy + 4, C_BLACK);
        gfx_set_pixel(s, cx + 4, cy + 4, C_BLACK);
        return;
    }
    if (game.state == GAME_WON) {
        // Sunglasses
        gfx_draw_hline(s, cx - 7, cy - 3, 15, C_BLACK);
        gfx_fill_rect(s, cx - 6, cy - 3, 5, 3, C_BLACK);
        gfx_fill_rect(s, cx + 2, cy - 3, 5, 3, C_BLACK);
    } else {
        gfx_fill_rect(s, cx - 4, cy - 4, 2, 2, C_BLACK);
        gfx_fill_rect(s, cx + 3, cy - 4, 2, 2, C_BLACK);
    }
    if (worried) {
        gfx_draw_circle(s, cx, cy + 3, 2, C_BLACK);
    } else {
        gfx_draw_hline(s, cx - 2, cy + 4, 5, C_BLACK);
        gfx_set_pixel(s, cx - 3, cy + 3, C_BLACK);
        gfx_set_pixel(s, cx + 3, cy + 3, C_BLACK);
        gfx_set_pixel(s, cx - 4, cy + 2, C_BLACK);
        gfx_set_pixel(s, cx + 4, cy + 2, C_BLACK);
    }
}

static void draw_header(gfx_surface_t* s) {
    gfx_rect_t h = header_rect();
    bevel(s, h.x - HEADER_BEVEL, h.y - HEADER_BEVEL, h.w + 2 * HEADER_BEVEL, h.h + 2 * HEADER_BEVEL,
          HEADER_BEVEL, false);
    int32_t ly = h.y + (h.h - LED_H) / 2;
    draw_led(s, h.x + 5, ly, game.mines - game.flags);
    draw_led(s, h.x + h.w - 5 - LED_W, ly, game.seconds);
    draw_face(s);
}

static void draw_main(void) {
    dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;

    gfx_clear(s, C_FACE);
    bevel(s, 0, 0, client_width(), client_height(), OUTER, true);
    draw_header(s);
    draw_board(s);
    wm_window_commit(main_win, NULL);
}

//
// Events
//

static void cancel_buttons(void) {
    if (ui.left || ui.right || ui.middle || ui.face_down || ui.hot >= 0)
        dirty = true;
    ui.left = ui.right = ui.middle = false;
    ui.chording = ui.swallow = ui.face_down = false;
    ui.hot = -1;
}

static void main_mouse_down(const wm_event_t* ev) {
    gfx_rect_t f = face_rect();
    int sq = square_at(ev->x, ev->y);
    bool playable = game.state == GAME_READY || game.state == GAME_PLAYING;

    if (ev->button == INPUT_BTN_LEFT) {
        ui.left = true;
        if (in_rect(&f, ev->x, ev->y) && !ui.right) {
            ui.face_down = ui.on_face = true;
        } else if (sq >= 0 && playable) {
            ui.hot = sq;
            ui.chording = ui.right;
            ui.cursor_shown = false;
        }
    } else if (ev->button == INPUT_BTN_RIGHT) {
        ui.right = true;
        if (sq >= 0 && playable) {
            if (ui.left) {
                ui.chording = true;
                ui.hot = sq;
            } else {
                toggle_flag(sq);
                ui.cursor_shown = false;
            }
        }
    } else if (ev->button == INPUT_BTN_MIDDLE) {
        ui.middle = true;
        if (sq >= 0 && playable) {
            ui.chording = true;
            ui.hot = sq;
        }
    }
    dirty = true;
}

static void main_mouse_move(const wm_event_t* ev) {
    if (ui.face_down) {
        gfx_rect_t f = face_rect();
        bool on = in_rect(&f, ev->x, ev->y);
        if (on != ui.on_face) {
            ui.on_face = on;
            dirty = true;
        }
    } else if (ui.left || ui.chording) {
        int sq = square_at(ev->x, ev->y);
        if (sq != ui.hot) {
            ui.hot = sq;
            dirty = true;
        }
    }
}

static void main_mouse_up(const wm_event_t* ev) {
    int sq = square_at(ev->x, ev->y);

    if (ev->button == INPUT_BTN_LEFT) {
        ui.left = false;
        if (ui.face_down) {
            gfx_rect_t f = face_rect();
            if (in_rect(&f, ev->x, ev->y))
                new_game();
            ui.face_down = false;
        } else if (ui.chording) {
            if (sq >= 0)
                chord(sq);
            ui.swallow = ui.right || ui.middle;
            ui.chording = false;
        } else if (!ui.swallow && sq >= 0) {
            open_square(sq);
        }
    } else if (ev->button == INPUT_BTN_RIGHT || ev->button == INPUT_BTN_MIDDLE) {
        if (ev->button == INPUT_BTN_RIGHT)
            ui.right = false;
        else
            ui.middle = false;
        if (ui.chording) {
            if (sq >= 0)
                chord(sq);
            ui.swallow = ui.left || ui.right || ui.middle;
            ui.chording = false;
        }
    }

    if (!ui.left && !ui.right && !ui.middle) {
        ui.swallow = false;
        ui.chording = false;
    }
    if (!ui.left && !ui.chording)
        ui.hot = -1;
    dirty = true;
}

static void move_cursor(int dx, int dy) {
    int c = ui.cursor % game.cols + dx, r = ui.cursor / game.cols + dy;
    if (c < 0) c = game.cols - 1;
    if (c >= game.cols) c = 0;
    if (r < 0) r = game.rows - 1;
    if (r >= game.rows) r = 0;
    if (ui.cursor_shown)                // The first press only shows the cursor
        ui.cursor = idx(c, r);
    ui.cursor_shown = true;
    dirty = true;
}

static void main_key(const wm_event_t* ev) {
    if (ev->mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT))
        return;
    switch (ev->key) {
        case KEY_LEFT:  move_cursor(-1, 0); break;
        case KEY_RIGHT: move_cursor(1, 0);  break;
        case KEY_UP:    move_cursor(0, -1); break;
        case KEY_DOWN:  move_cursor(0, 1);  break;

        case ' ':
        case KEY_ENTER:
        case '\r':
        case '\n':
            if (ui.cursor_shown)
                open_square(ui.cursor);
            else
                move_cursor(0, 0);
            break;

        case 'f':
        case 'F':
            if (ui.cursor_shown)
                toggle_flag(ui.cursor);
            else
                move_cursor(0, 0);
            break;

        case 'n':
        case 'N':
        case KEY_ESCAPE:
            new_game();
            break;

        case '1': set_level(LEVEL_BEGINNER);     break;
        case '2': set_level(LEVEL_INTERMEDIATE); break;
        case '3': set_level(LEVEL_EXPERT);       break;

        case 'm':
        case 'M':
            game.marks = !game.marks;
            wm_notify("Marks", game.marks ? "Question marks on" : "Question marks off");
            break;
    }
}

static void show_game_menu(int32_t x, int32_t y) {
    wm_menu_item_t items[] = {
        { "New game",       CMD_NEW,          0 },
        { NULL,             0,                WM_MENU_SEPARATOR },
        { "Beginner",       CMD_BEGINNER,     game.level == LEVEL_BEGINNER     ? WM_MENU_CHECKED : 0 },
        { "Intermediate",   CMD_INTERMEDIATE, game.level == LEVEL_INTERMEDIATE ? WM_MENU_CHECKED : 0 },
        { "Expert",         CMD_EXPERT,       game.level == LEVEL_EXPERT       ? WM_MENU_CHECKED : 0 },
        { NULL,             0,                WM_MENU_SEPARATOR },
        { "Marks (?)",      CMD_MARKS,        game.marks ? WM_MENU_CHECKED : 0 },
        { NULL,             0,                WM_MENU_SEPARATOR },
        { "Exit",           CMD_EXIT,         0 },
    };
    wm_menu_show(main_win, x, y, items, sizeof(items) / sizeof(items[0]));
}

static void main_event(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_MOUSE_DOWN:   main_mouse_down(ev);  break;
        case WM_EVENT_MOUSE_MOVE:   main_mouse_move(ev);  break;
        case WM_EVENT_MOUSE_UP:     main_mouse_up(ev);    break;
        case WM_EVENT_KEY:          main_key(ev);         break;

        case WM_EVENT_CONTEXT_MENU:
            // On the field a right click flags (done on the press); elsewhere it's the menu
            if (square_at(ev->x, ev->y) < 0)
                show_game_menu(ev->x, ev->y);
            break;

        case WM_EVENT_MENU_COMMAND:
            switch (ev->param) {
                case CMD_NEW:          new_game();                      break;
                case CMD_BEGINNER:     set_level(LEVEL_BEGINNER);       break;
                case CMD_INTERMEDIATE: set_level(LEVEL_INTERMEDIATE);   break;
                case CMD_EXPERT:       set_level(LEVEL_EXPERT);         break;
                case CMD_MARKS:        game.marks = !game.marks;        break;
                case CMD_EXIT:         running = false;                 break;
            }
            break;

        case WM_EVENT_MOUSE_LEAVE:
        case WM_EVENT_FOCUS_LOST:
            cancel_buttons();
            break;

        case WM_EVENT_RESIZED:
            dirty = true;
            break;

        case WM_EVENT_CLOSE:
            running = false;
            break;

        case WM_EVENT_DESTROYED:
            main_win = 0;
            running = false;
            break;
    }
}

//
// Main
//

static void usage(void) {
    fputs("usage: " APP_NAME " [beginner | intermediate | expert | COLUMNS ROWS MINES]\n", stderr);
}

// The level from the command line; false on a bad one
static bool parse_args(int argc, char** argv) {
    if (argc <= 1) {
        set_field(LEVEL_BEGINNER, levels[0].cols, levels[0].rows, levels[0].mines);
        return true;
    }
    if (argc == 2) {
        const char* a = argv[1];
        while (*a == '-')
            a++;
        for (int l = 0; l < 3; l++) {
            size_t n = strlen(a);
            if (n > 0 && strncmp(a, levels[l].name, n) == 0) {     // "b", "exp", ... will do
                set_field((level_t)l, levels[l].cols, levels[l].rows, levels[l].mines);
                return true;
            }
        }
        usage();
        return false;
    }
    if (argc == 4) {
        char* end;
        long v[3];
        for (int k = 0; k < 3; k++) {
            v[k] = strtol(argv[k + 1], &end, 10);
            if (end == argv[k + 1] || *end != 0) {
                usage();
                return false;
            }
        }
        if (v[0] < MIN_COLS || v[0] > MAX_COLS || v[1] < MIN_ROWS || v[1] > MAX_ROWS) {
            fprintf(stderr, APP_NAME ": the field is %d to %d columns by %d to %d rows\n",
                    MIN_COLS, MAX_COLS, MIN_ROWS, MAX_ROWS);
            return false;
        }
        long most = (v[0] - 1) * (v[1] - 1);
        if (v[2] < MIN_MINES || v[2] > most) {
            fprintf(stderr, APP_NAME ": %ld x %ld takes %d to %ld mines\n", v[0], v[1], MIN_MINES, most);
            return false;
        }
        level_t level = LEVEL_CUSTOM;
        for (int l = 0; l < 3; l++)
            if (v[0] == levels[l].cols && v[1] == levels[l].rows && v[2] == levels[l].mines)
                level = (level_t)l;
        set_field(level, (int)v[0], (int)v[1], (int)v[2]);
        return true;
    }
    usage();
    return false;
}

int main(int argc, char** argv) {
    game.marks = true;
    if (!parse_args(argc, argv))
        return 1;
    rng_seed((uint32_t)os_time_ms() * 2654435761u ^ (uint32_t)(os_time_ms() >> 32));
    pick_cell_size();

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title      = "Minesweeper";
    d.x          = WM_POS_DEFAULT;
    d.y          = WM_POS_DEFAULT;
    d.width      = client_width();
    d.height     = client_height();
    d.style      = WM_STYLE_CONTEXT_MENU;

    main_win = wm_window_create(&d);
    if (main_win == 0) {
        fprintf(stderr, APP_NAME ": can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    draw_main();
    wm_window_focus(main_win);

    // Events are handled as they come and the window is repainted once the
    // queue runs dry. While a game runs, the wait ends at the next second so
    // the clock ticks.
    wm_event_t ev;
    while (running) {
        int32_t timeout = -1;
        if (dirty) {
            timeout = 0;
        } else if (game.state == GAME_PLAYING && game.seconds < MAX_SECONDS) {
            uint64_t next = game.start_ms + (uint64_t)game.seconds * 1000;
            uint64_t now = os_time_ms();
            timeout = next > now ? (int32_t)(next - now) : 0;
        }
        int r = wm_wait_event(&ev, timeout);
        if (r < 0)
            break;                          // The dwm went away
        if (r == 0) {
            update_clock();
            if (dirty) draw_main();
            continue;
        }

        if (ev.type == WM_EVENT_THEME_CHANGED) {
            dirty = true;                   // Fixed classic colours, but the frame may have moved on
        } else if (ev.window == main_win) {
            main_event(&ev);
        }
    }

    wm_disconnect();
    return 0;
}
