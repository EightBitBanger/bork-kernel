/*
 * cedit: a C code editor for the desktop, grown from notepad.
 *
 *     cedit [file]
 *
 * Everything notepad does (one document per window, undo / redo, the
 * clipboard, word wrap, scroll bars, the mouse wheel, Alt+S / Alt+X), and:
 *
 *   - C syntax highlighting for .c and .h files (and new documents)
 *   - line numbers down the left
 *   - Tab / Shift+Tab indent and unindent; with a selection over several
 *     lines, all of them
 *   - auto indent: Enter keeps the line's indent, one level more after '{';
 *     typing '}' on an empty line takes a level back off
 *   - brace completion: ( [ { " ' put in their partner, typing the partner
 *     steps over it, Backspace between an empty pair takes both
 *   - the end of line the file came with is kept, or LF / CRLF on save
 *   - Build: F7 compiles with tcc, F5 compiles and runs, F8 / Shift+F8 step
 *     through the errors, Esc stops a running program (build.c)
 *   - Settings > Editor / Compiler (settings.c), kept in the registry
 *
 * Built on os/wm.h: the dwm provides the window, the context menus, the
 * clipboard and notifications; libc's file dialog (os/filedlg.h) does Open
 * and Save as. The "save changes?" question is a small window of this
 * program's own, as is the settings window.
 *
 * Needs SEC_FILE_READ / SEC_FILE_WRITE, SEC_GFX, SEC_IPC and SEC_EXEC (the
 * default clearance has them all).
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/os.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()
#include <os/filedlg.h>
#include "cedit.h"

// Main window
#define WIN_W           660
#define WIN_H           440
#define WIN_MIN_W       220
#define WIN_MIN_H       140

#define MENUBAR_H       16
#define MENU_X0         6           // The first menu title
#define MENU_PAD        8           // Each side of a menu title

#define STATUS_H        17
#define STATUS_TEXT_H   14          // Height the status text is centred in, at the
                                    // bar's bottom: the bar grew upwards, the text stayed
#define PAD_X           8
#define PAD_Y           8
#define ROW_GAP         4           // Pixels between text rows
#define GUTTER_PAD      6           // Each side of the line numbers
#define ROW_MAX         1000        // Widest row in cells (a 4096 px surface is ~680)

#define SB_W            14          // Scroll bar thickness, and its arrow buttons' length
#define SB_MIN_THUMB    12
#define SB_REPEAT_FIRST 400         // ms before a held arrow or track starts repeating
#define SB_REPEAT_MS    50

#define WHEEL_ROWS      3           // Rows a wheel notch scrolls (Ctrl: a page)
#define WHEEL_COLS      6           // Cells a notch scrolls with Shift
#define BUILD_POLL_MS   25          // How often a running build is read

#define IO_CHUNK        (64u * 1024u)   // Kept well under the kernel's SYS_RW_MAX

#define UNDO_MAX_STEPS  1000
#define UNDO_MAX_BYTES  (8u * 1024u * 1024u)    // Text the undo history may hold

// Dialogs
#define DLG_W           340
#define DLG_CONFIRM_H   90
#define DLG_MARGIN      12
#define BTN_W           80
#define BTN_H           20
#define BTN_GAP         8
#define DEFAULT_NAME    "untitled.c"     // Save as offers it for a new document

enum {
    CMD_NEW = 1,
    CMD_OPEN,
    CMD_SAVE,
    CMD_SAVE_AS,
    CMD_EXIT,
    CMD_UNDO,
    CMD_REDO,
    CMD_CUT,
    CMD_COPY,
    CMD_PASTE,
    CMD_DELETE,
    CMD_SELECT_ALL,
    CMD_WORD_WRAP,
    CMD_INDENT,
    CMD_UNINDENT,
    CMD_COMPILE,
    CMD_RUN,
    CMD_STOP,
    CMD_NEXT_ERROR,
    CMD_PREV_ERROR,
    CMD_OUTPUT,
    CMD_SET_EDITOR,
    CMD_SET_COMPILER,
};

// The menu bar
enum { MENU_FILE, MENU_EDIT, MENU_BUILD, MENU_SETTINGS, MENU_COUNT };
static const char* const menu_names[MENU_COUNT] = { "File", "Edit", "Build", "Settings" };

// Parts of a scroll bar
enum { SB_NONE, SB_LESS, SB_MORE, SB_PAGE_LESS, SB_PAGE_MORE, SB_THUMB };

// What to do once the current document is safe (saved or knowingly dropped)
typedef enum { THEN_NOTHING, THEN_NEW, THEN_OPEN, THEN_EXIT, THEN_COMPILE, THEN_RUN } then_t;

typedef enum { DLG_NONE, DLG_CONFIRM } dlg_kind_t;

typedef struct {
    char*    buf;
    uint32_t len;
    uint32_t cap;
    uint32_t cursor;
    uint32_t anchor;                // Selection is [min(anchor, cursor), max(...))
    bool     modified;
    bool     crlf;                  // The file's lines ended in CR LF (taken out on load)
    char     path[PATH_MAX];        // Absolute; "" = Untitled
} doc_t;

typedef struct {
    int32_t  width, height;         // Client size
    int32_t  cw, row_h;             // Cell width, row pitch
    int32_t  text_x, text_y;        // Top-left of the glyphs of the first shown row
    int32_t  status_y;
    int32_t  gutter;                // Width of the line numbers (0: off)
    gfx_rect_t view;                // The text area, less the scroll bars
    gfx_rect_t pane;                // The output pane (h 0: hidden)
    uint32_t cols;                  // Cells per row before it wraps
    uint32_t vis_cols;              // Whole cells that fit across
    uint32_t rows;                  // Whole rows that fit
    uint32_t total_rows;            // Rows of the laid out text
    uint32_t widest;                // Its widest row, in cells
    uint32_t lines;                 // Lines of the text ('\n' + 1)
    bool     vbar, hbar;            // Scroll bars shown
} metrics_t;

typedef struct {
    bool       vertical;
    gfx_rect_t bar, less, more;     // The whole bar and its two arrow buttons
    gfx_rect_t thumb;               // Empty when there is no room for it
    int32_t    track0, track_len;   // Where the thumb can go, along the bar
    uint32_t   pos, max, page;      // First row / cell shown, its limit, rows / cells shown
} sbar_t;

static doc_t        doc;
wm_window_t         main_win;
wm_theme_t          theme;
static bool         running = true;
static bool         main_dirty;
static bool         main_focused = true;
static uint32_t     scroll_row;     // First row shown
static uint32_t     scroll_col;     // First cell shown
static bool         word_wrap;
static uint32_t     doc_gen;        // Bumped by every change to the text
static bool         selecting;      // Left button held after clicking the text
static bool         has_pref_col;   // Column remembered across Up / Down
static uint32_t     pref_col;
static char         shown_title[WM_TITLE_MAX];

// A scroll bar part held down with the left button
static struct {
    int      part;                  // SB_*
    bool     vertical;
    int32_t  grab;                  // Thumb: where along it the pointer took hold
    int32_t  x, y;                  // The pointer now (track paging stops under it)
    uint64_t next_ms;               // When a held arrow or track repeats
} sb_hold;

static struct {
    dlg_kind_t  kind;
    wm_window_t win;
    then_t      then;
    int         focus;              // The button the keyboard is on
    int         pressed;            // Button under a held mouse button, or -1
    bool        dirty;
    char        message[64];
} dlg;

static filedlg_t*   fdlg;           // Open / Save as, while it is up
static then_t       fdlg_then;      // Save as: what to do after saving

static void dialog_open(dlg_kind_t kind, then_t then);
static void file_dialog(filedlg_mode_t mode, then_t then);
static void run_then(then_t then);
static void save_then(then_t then);
static void indent_lines(bool more);

//
// Small helpers
//

void notify_error(const char* title, const char* message) {
    wm_notify_ex(WM_NOTIFY_ERROR, title, message, WM_NOTIFY_DEFAULT_MS * 2);
}

static const char* doc_name(void) {
    if (doc.path[0] == '\0') return "Untitled";
    const char* slash = strrchr(doc.path, '/');
    return slash ? slash + 1 : doc.path;
}

bool point_in(const gfx_rect_t* r, int32_t x, int32_t y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static void default_theme(void) {
    memset(&theme, 0, sizeof(theme));
    theme.client.background    = GFX_RGB(236, 236, 236);
    theme.client.text          = GFX_RGB(20, 20, 20);
    theme.client.text_soft     = GFX_RGB(80, 80, 80);
    theme.button.fill          = GFX_RGB(222, 222, 222);
    theme.button.border        = GFX_RGB(140, 140, 140);
    theme.button.border_active = GFX_RGB(40, 100, 200);
    theme.button.text          = GFX_RGB(20, 20, 20);
    theme.button.fill_pressed  = GFX_RGB(190, 190, 190);
    theme.button.border_pressed = GFX_RGB(90, 90, 90);
    theme.edit.background      = GFX_COLOR_WHITE;
    theme.edit.border          = GFX_RGB(140, 140, 140);
    theme.edit.text            = GFX_RGB(20, 20, 20);
    theme.edit.cursor          = GFX_COLOR_BLACK;
    theme.edit.selection       = GFX_RGB(170, 200, 240);
}

static void load_theme(void) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
}

//
// Layout
//
// Rules shared by drawing, the arrow keys and mouse hit testing:
//   - the caret "at i" sits left of byte i, at (row, col)
//   - '\n' ends the row
//   - '\t' advances to the next multiple of cfg.tab_width (cut at the row end)
//   - any other byte takes one cell
//   - reaching `cols` cells wraps to the next row (`cols` is the width of the
//     window with word wrap on, else ROW_MAX)
//

// Cells byte `ch` takes when it starts at `col` (col < cols)
static uint32_t cell_width(char ch, uint32_t col, uint32_t cols) {
    if (ch != '\t')
        return 1;
    uint32_t w = cfg.tab_width - col % cfg.tab_width;
    return (col + w > cols) ? cols - col : w;
}

static void advance(char ch, uint32_t cols, uint32_t* row, uint32_t* col) {
    if (ch == '\n') {
        (*row)++;
        *col = 0;
        return;
    }
    *col += cell_width(ch, *col, cols);
    if (*col >= cols) {
        (*row)++;
        *col = 0;
    }
}

static void pos_of(uint32_t cols, uint32_t index, uint32_t* row, uint32_t* col) {
    uint32_t r = 0, c = 0;
    if (index > doc.len) index = doc.len;
    for (uint32_t i = 0; i < index; i++)
        advance(doc.buf[i], cols, &r, &c);
    *row = r;
    *col = c;
}

// Rows of the whole text and its widest row in cells, laid out `cols` wide.
// Also its count of lines. Every event asks for it, so the last three
// answers are kept until the text changes: get_metrics() asks for the line
// count (ROW_MAX wide), and with word wrap on for two widths (with and
// without the vertical bar).
static void measure(uint32_t cols, uint32_t* rows, uint32_t* widest, uint32_t* lines) {
    static struct {
        bool     valid;
        uint32_t gen, cols, rows, widest, lines;
    } cache[3];
    static int older;

    for (int k = 0; k < 3; k++) {
        if (cache[k].valid && cache[k].gen == doc_gen && cache[k].cols == cols) {
            *rows = cache[k].rows;
            *widest = cache[k].widest;
            *lines = cache[k].lines;
            return;
        }
    }

    uint32_t r = 0, c = 0, w = 0, l = 1;
    for (uint32_t i = 0; i < doc.len; i++) {
        char ch = doc.buf[i];
        if (ch == '\n') l++;
        uint32_t prev_r = r, prev_c = c;
        advance(ch, cols, &r, &c);
        uint32_t end = (r == prev_r) ? c : (ch == '\n') ? prev_c : cols;   // Where the row ended
        if (end > w) w = end;
    }

    int k = older;
    older = (older + 1) % 3;
    cache[k].valid  = true;
    cache[k].gen    = doc_gen;
    cache[k].cols   = cols;
    cache[k].rows   = r + 1;
    cache[k].widest = w;
    cache[k].lines  = l;
    *rows = r + 1;
    *widest = w;
    *lines = l;
}

static metrics_t get_metrics(void) {
    metrics_t m;
    gfx_surface_t* s = wm_window_surface(main_win);
    wm_window_get_size(main_win, &m.width, &m.height);
    m.cw       = s ? gfx_char_width(s) : 6;
    m.row_h    = (s ? gfx_line_height(s) : 8) + ROW_GAP;
    m.text_y   = MENUBAR_H + PAD_Y;
    m.status_y = m.height - STATUS_H;
    m.vbar     = false;
    m.hbar     = false;

    // The output pane sits above the status bar
    int32_t pane_h = pane_height(m.status_y - MENUBAR_H, m.row_h);
    m.pane = gfx_rect(0, m.status_y - pane_h, m.width, pane_h);
    int32_t text_b = m.status_y - pane_h;

    // Line numbers: as many digits as the last line needs, at least 3
    m.gutter = 0;
    if (cfg.line_numbers) {
        uint32_t r, w, lines;
        measure(ROW_MAX, &r, &w, &lines);
        int digits = 3;
        for (uint32_t n = lines; n >= 1000; n /= 10) digits++;
        m.gutter = digits * m.cw + 2 * GUTTER_PAD;
    }
    m.text_x = m.gutter + PAD_X;

    // A bar shows when the text doesn't fit, and takes room from the other
    // direction, so one bar can bring in the other. Bars only ever get added,
    // so this settles in three rounds at most.
    for (;;) {
        int32_t view_w = m.width - (m.vbar ? SB_W : 0);
        int32_t view_b = text_b - (m.hbar ? SB_W : 0);
        m.view = gfx_rect(0, MENUBAR_H, view_w, view_b - MENUBAR_H);

        int32_t cols = (view_w - m.gutter - 2 * PAD_X) / m.cw;
        m.vis_cols = (cols < 1) ? 1 : (cols > ROW_MAX) ? ROW_MAX : (uint32_t)cols;
        int32_t rows = (view_b - m.text_y) / m.row_h;
        m.rows = (rows < 1) ? 1 : (uint32_t)rows;

        m.cols = word_wrap ? m.vis_cols : ROW_MAX;
        measure(m.cols, &m.total_rows, &m.widest, &m.lines);

        bool vbar = m.vbar || m.total_rows > m.rows;
        bool hbar = m.hbar || m.widest > m.vis_cols;
        if (vbar == m.vbar && hbar == m.hbar)
            break;
        m.vbar = vbar;
        m.hbar = hbar;
    }
    return m;
}

// Caret index nearest (row, col). Past the end of a line snaps to the end of
// that line, past the last row to the end of the text.
static uint32_t index_at(uint32_t cols, uint32_t row, uint32_t col) {
    uint32_t r = 0, c = 0;
    for (uint32_t i = 0; i < doc.len; i++) {
        if (r > row) return i;                      // The row wrapped before `col`
        if (r == row && c >= col) return i;

        char ch = doc.buf[i];
        if (r == row) {
            if (ch == '\n') return i;               // Before the line break
            if (ch == '\t') {                       // Nearer edge of a wide cell
                uint32_t w = cell_width(ch, c, cols);
                if (col < c + w) return ((col - c) * 2 < w) ? i : i + 1;
            }
        }
        advance(ch, cols, &r, &c);
    }
    return doc.len;
}

static uint32_t index_from_point(const metrics_t* m, int32_t x, int32_t y) {
    // Rows are hit tested on their highlight band, which starts ROW_GAP/2 above the glyphs
    int32_t rel = y - (m->text_y - ROW_GAP / 2);
    int64_t row = (int64_t)scroll_row + ((rel >= 0) ? rel / m->row_h : -((-rel + m->row_h - 1) / m->row_h));
    if (row < 0)
        return 0;
    if (row > (int64_t)UINT32_MAX)
        row = UINT32_MAX;

    // Nearest gap between characters, not the cell under the pointer. Left
    // of the text is one cell left of what is shown, so a drag there scrolls.
    int64_t col;
    if (x < m->text_x)
        col = (scroll_col > 0) ? (int64_t)scroll_col - 1 : 0;
    else
        col = (int64_t)scroll_col + (x - m->text_x + m->cw / 2) / m->cw;
    if (col > (int64_t)m->cols - 1) col = (int64_t)m->cols - 1;

    return index_at(m->cols, (uint32_t)row, (uint32_t)col);
}

// No empty screen below the end of the text or right of its widest row
static void clamp_scroll(const metrics_t* m) {
    uint32_t max_row = (m->total_rows > m->rows) ? m->total_rows - m->rows : 0;
    uint32_t max_col = (m->widest > m->vis_cols) ? m->widest - m->vis_cols : 0;
    if (scroll_row > max_row) scroll_row = max_row;
    if (scroll_col > max_col) scroll_col = max_col;
}

// Keep the caret on screen. Sideways it jumps a quarter of the view, so the
// text around it shows too.
static void reveal_cursor(void) {
    metrics_t m = get_metrics();
    uint32_t row, col;
    pos_of(m.cols, doc.cursor, &row, &col);
    clamp_scroll(&m);

    if (row < scroll_row)
        scroll_row = row;
    else if (row >= scroll_row + m.rows)
        scroll_row = row - m.rows + 1;

    uint32_t margin = m.vis_cols / 4;
    if (col < scroll_col)
        scroll_col = (col > margin) ? col - margin : 0;
    else if (col > scroll_col + m.vis_cols)     // The caret may sit just past the last cell
        scroll_col = col - m.vis_cols + margin;
    clamp_scroll(&m);
}

static void changed(void) {
    reveal_cursor();
    main_dirty = true;
}

//
// Selection and editing
//

static bool has_selection(void)       { return doc.anchor != doc.cursor; }
static uint32_t selection_start(void) { return (doc.anchor < doc.cursor) ? doc.anchor : doc.cursor; }
static uint32_t selection_end(void)   { return (doc.anchor > doc.cursor) ? doc.anchor : doc.cursor; }

// Move the caret. extend: keep the anchor where it is (grow the selection)
static void move_cursor(uint32_t index, bool extend) {
    if (index > doc.len) index = doc.len;
    doc.cursor = index;
    if (!extend) doc.anchor = index;
}

static void select_range(uint32_t start, uint32_t end) {
    doc.anchor = (start > doc.len) ? doc.len : start;
    doc.cursor = (end > doc.len) ? doc.len : end;
}

// Room for `extra` more bytes. The buffer is binary: no terminator.
static bool ensure_capacity(uint32_t extra) {
    if (extra > UINT32_MAX - doc.len)
        return false;
    uint32_t need = doc.len + extra;
    if (doc.buf != NULL && need <= doc.cap)
        return true;

    // Double for typing, but a big paste gets what it needs plus a little:
    // doubling a multi-megabyte buffer for one more megabyte wastes the heap
    uint32_t cap = doc.cap ? doc.cap : 2048;
    if (cap <= 0x7FFFFFFFu) cap *= 2;
    if (cap < need) cap = (need <= UINT32_MAX - 4096u) ? need + 4096u : need;

    char* p = (char*)realloc(doc.buf, cap);
    if (p == NULL)
        return false;
    doc.buf = p;
    doc.cap = cap;
    return true;
}

//
// Undo history
//
// Every change to the text is one replace: at `pos`, `removed` gave way to
// `added`. Undo puts `removed` back, redo does the change again. Typing,
// Backspace and Delete a byte at a time grow the last step while they keep
// going in one place, so Ctrl+Z takes back a word or a run of deletes, not
// a byte. Moving the caret, clicking, a command or a save closes the step.
//

typedef enum { EDIT_OTHER, EDIT_TYPE, EDIT_BACKSPACE, EDIT_DELETE } edit_kind_t;

typedef struct {
    uint32_t    id;                 // Unique: says which state of the text this step leads to
    edit_kind_t kind;
    uint32_t    pos;
    char*       removed;
    uint32_t    removed_len;
    char*       added;
    uint32_t    added_len;
    uint32_t    anchor0, cursor0;   // The selection before
    uint32_t    anchor1, cursor1;   // and after
} step_t;

static struct {
    step_t*  steps;
    uint32_t count;                 // Steps kept: [0, pos) done, [pos, count) undone (redo)
    uint32_t pos;
    uint32_t cap;
    uint32_t bytes;                 // Text held by all the steps
    uint32_t next_id;
    uint32_t base_id;               // The state before steps[0] (0: as loaded)
    uint32_t saved_id;              // The state last loaded or saved
    bool     open;                  // The last step may still grow
} hist;

static void step_free(step_t* st) {
    hist.bytes -= st->removed_len + st->added_len;
    free(st->removed);
    free(st->added);
}

static uint32_t hist_state(void) {
    return (hist.pos > 0) ? hist.steps[hist.pos - 1].id : hist.base_id;
}

// The text matches the file again once undo or redo gets back to it
static void hist_update_modified(void) {
    doc.modified = (hist_state() != hist.saved_id);
}

static void hist_seal(void) {
    hist.open = false;
}

static void hist_mark_saved(void) {
    hist.saved_id = hist_state();
    hist.open = false;
}

static void hist_clear(void) {
    for (uint32_t i = 0; i < hist.count; i++)
        step_free(&hist.steps[i]);
    free(hist.steps);
    uint32_t next_id = hist.next_id;
    memset(&hist, 0, sizeof(hist));
    hist.next_id = next_id;         // Ids stay unique across documents
    hist.base_id = hist.saved_id = ++hist.next_id;
}

static void hist_drop_redo(void) {
    while (hist.count > hist.pos)
        step_free(&hist.steps[--hist.count]);
}

// Forget the oldest steps while there are too many or they hold too much
static void hist_trim(void) {
    uint32_t drop = 0;
    while (hist.count - drop > 1 &&
           (hist.count - drop > UNDO_MAX_STEPS || hist.bytes > UNDO_MAX_BYTES)) {
        hist.base_id = hist.steps[drop].id;
        step_free(&hist.steps[drop]);
        drop++;
    }
    if (drop == 0)
        return;
    memmove(hist.steps, hist.steps + drop, (hist.count - drop) * sizeof(step_t));
    hist.count -= drop;
    hist.pos -= drop;
}

static char* copy_bytes(const char* data, uint32_t length) {
    if (length == 0)
        return NULL;
    char* p = (char*)malloc(length);
    if (p != NULL)
        memcpy(p, data, length);
    return p;
}

// Append `data` to a step's removed / added bytes
static bool grow_bytes(char** buf, uint32_t* len, const char* data, uint32_t length, bool at_front) {
    char* p = (char*)realloc(*buf, *len + length);
    if (p == NULL)
        return false;
    if (at_front) {
        memmove(p + length, p, *len);
        memcpy(p, data, length);
    } else {
        memcpy(p + *len, data, length);
    }
    *buf = p;
    *len += length;
    hist.bytes += length;
    return true;
}

// Fold a one-byte edit into the last step, if it carries on from it
static bool hist_merge(uint32_t start, uint32_t end, const char* data, uint32_t length, edit_kind_t kind) {
    if (!hist.open || hist.pos == 0 || hist.pos != hist.count || kind == EDIT_OTHER)
        return false;
    step_t* st = &hist.steps[hist.pos - 1];
    if (st->kind != kind || st->id == hist.saved_id)
        return false;

    switch (kind) {
        case EDIT_TYPE: {
            if (start != end || length != 1 || start != st->pos + st->added_len || st->added_len == 0)
                return false;
            // A new word starts a new step
            char last = st->added[st->added_len - 1];
            bool blank = (last == ' ' || last == '\t');
            if (blank && data[0] != ' ' && data[0] != '\t')
                return false;
            return grow_bytes(&st->added, &st->added_len, data, 1, false);
        }
        case EDIT_BACKSPACE:
            if (length != 0 || end != st->pos || end - start != 1)
                return false;
            if (!grow_bytes(&st->removed, &st->removed_len, doc.buf + start, 1, true))
                return false;
            st->pos = start;
            return true;
        case EDIT_DELETE:
            if (length != 0 || start != st->pos || end - start != 1)
                return false;
            return grow_bytes(&st->removed, &st->removed_len, doc.buf + start, 1, false);
        default:
            return false;
    }
}

// Record the replace of [start, end) by data, before it is made. When there
// isn't the memory for it the history is dropped: undo would put back the
// wrong text otherwise.
static void hist_record(uint32_t start, uint32_t end, const char* data, uint32_t length, edit_kind_t kind) {
    hist_drop_redo();
    if (hist_merge(start, end, data, length, kind))
        return;

    step_t st;
    memset(&st, 0, sizeof(st));
    st.kind        = kind;
    st.pos         = start;
    st.removed_len = end - start;
    st.added_len   = length;
    st.removed     = copy_bytes(doc.buf + start, st.removed_len);
    st.added       = copy_bytes(data, length);
    st.anchor0     = doc.anchor;
    st.cursor0     = doc.cursor;

    bool ok = (st.removed_len == 0 || st.removed != NULL) && (st.added_len == 0 || st.added != NULL);
    if (ok && hist.count == hist.cap) {
        uint32_t cap = hist.cap ? hist.cap * 2 : 64;
        step_t* p = (step_t*)realloc(hist.steps, cap * sizeof(step_t));
        if (p != NULL) {
            hist.steps = p;
            hist.cap = cap;
        } else {
            ok = false;
        }
    }
    if (!ok) {
        free(st.removed);
        free(st.added);
        hist_clear();
        hist.saved_id = 0;          // Can't be sure any more: count it as changed
        return;
    }

    st.id = ++hist.next_id;
    hist.bytes += st.removed_len + st.added_len;
    hist.steps[hist.count++] = st;
    hist.pos = hist.count;
    hist.open = (kind != EDIT_OTHER);
    hist_trim();
}

// The selection after the last step, once the edit is made
static void hist_note_after(void) {
    if (hist.pos == 0)
        return;
    step_t* st = &hist.steps[hist.pos - 1];
    st->anchor1 = doc.anchor;
    st->cursor1 = doc.cursor;
}

//
// Editing
//

// Replace [start, end) with `data`, leaving the caret after it. Every change
// to the text comes through here (undo and redo too, with record false).
static bool replace_range(uint32_t start, uint32_t end, const char* data, uint32_t length,
                          edit_kind_t kind, bool record) {
    if (end > doc.len) end = doc.len;
    if (start > end) start = end;
    if (start == end && length == 0)
        return true;

    // Check the room first, so a failed edit changes nothing
    uint32_t gone = end - start;
    if (length > gone && !ensure_capacity(length - gone)) {
        notify_error(APP_NAME, "Not enough memory");
        return false;
    }
    if (record)
        hist_record(start, end, data, length, kind);

    memmove(doc.buf + start + length, doc.buf + end, doc.len - end);
    if (length > 0)
        memcpy(doc.buf + start, data, length);
    doc.len = doc.len - gone + length;
    doc_gen++;
    move_cursor(start + length, false);

    if (record)
        hist_note_after();
    hist_update_modified();
    return true;
}

static void delete_range(uint32_t start, uint32_t end) {
    replace_range(start, end, NULL, 0, EDIT_OTHER, true);
}

static bool delete_selection(void) {
    if (!has_selection())
        return false;
    delete_range(selection_start(), selection_end());
    return true;
}

// Put data in place of [start, end), as one step
static bool insert_bytes_range(uint32_t start, uint32_t end, const char* data, uint32_t length) {
    return replace_range(start, end, data, length, EDIT_OTHER, true);
}

// Insert at the caret, replacing the selection
static bool insert_bytes(const char* data, uint32_t length, edit_kind_t kind) {
    return replace_range(selection_start(), selection_end(), data, length, kind, true);
}

static bool can_undo(void) { return hist.pos > 0; }
static bool can_redo(void) { return hist.pos < hist.count; }

static void undo(void) {
    if (!can_undo())
        return;
    step_t* st = &hist.steps[hist.pos - 1];
    if (!replace_range(st->pos, st->pos + st->added_len, st->removed, st->removed_len, EDIT_OTHER, false))
        return;
    hist.pos--;
    hist.open = false;
    select_range(st->anchor0, st->cursor0);
    hist_update_modified();
}

static void redo(void) {
    if (!can_redo())
        return;
    step_t* st = &hist.steps[hist.pos];
    if (!replace_range(st->pos, st->pos + st->removed_len, st->added, st->added_len, EDIT_OTHER, false))
        return;
    hist.pos++;
    hist.open = false;
    select_range(st->anchor1, st->cursor1);
    hist_update_modified();
}

//
// Words (double click, Ctrl+Left / Right, Ctrl+Backspace / Delete)
//

enum { CLASS_WORD, CLASS_BLANK, CLASS_BREAK, CLASS_OTHER };

static int char_class(char ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_')       return CLASS_WORD;
    if (ch == ' ' || ch == '\t' || ch == '\0')       return CLASS_BLANK;
    if (ch == '\n')                                  return CLASS_BREAK;
    return CLASS_OTHER;
}

static bool char_is_word(char c) {
    return char_class(c) == CLASS_WORD;
}

static uint32_t word_left(uint32_t i) {
    while (i > 0 && char_class(doc.buf[i - 1]) == CLASS_BLANK) i--;
    if (i > 0) {
        int c = char_class(doc.buf[i - 1]);
        while (i > 0 && char_class(doc.buf[i - 1]) == c) i--;
    }
    return i;
}

static uint32_t word_right(uint32_t i) {
    if (i < doc.len) {
        int c = char_class(doc.buf[i]);
        while (i < doc.len && char_class(doc.buf[i]) == c) i++;
    }
    while (i < doc.len && char_class(doc.buf[i]) == CLASS_BLANK) i++;
    return i;
}

static void select_word_at(uint32_t index) {
    if (doc.len == 0)
        return;
    if (index >= doc.len) index = doc.len - 1;     // Just past the end: the last char

    int cls = char_class(doc.buf[index]);
    if (cls == CLASS_BREAK) {
        select_range(index, index + 1);
        return;
    }
    uint32_t start = index, end = index + 1;
    while (start > 0 && char_class(doc.buf[start - 1]) == cls) start--;
    while (end < doc.len && char_class(doc.buf[end]) == cls) end++;
    select_range(start, end);
}

//
// Clipboard
//

// Plain text goes up as TEXT so other programs see text. A selection holding
// NUL bytes goes up as BINARY instead, so nothing is lost.
static bool copy_selection(void) {
    if (!has_selection())
        return false;

    uint32_t start = selection_start();
    uint32_t length = selection_end() - start;
    if (length > WM_CLIP_MAX) {
        notify_error("Can't copy", "The selection is too large");
        return false;
    }

    const char* data = doc.buf + start;
    wm_clip_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = length;
    if (memchr(data, '\0', length) != NULL) {
        info.type = WM_CLIP_BINARY;
        strlcpy(info.format, "application/octet-stream", sizeof(info.format));
    } else {
        info.type = WM_CLIP_TEXT;
    }

    if (wm_clip_set(&info, data) < 0) {
        notify_error("Can't copy", strerror(errno));
        return false;
    }
    return true;
}

static bool clip_is_pasteable(const wm_clip_info_t* info) {
    return info->size > 0 &&
           (info->type == WM_CLIP_TEXT || info->type == WM_CLIP_BINARY || info->type == WM_CLIP_FILES);
}

static bool can_paste(void) {
    wm_clip_info_t info;
    return wm_clip_info(&info) == 0 && clip_is_pasteable(&info);
}

// TEXT and BINARY go in as they are; FILES as their paths, one per line
static void paste(void) {
    wm_clip_info_t info;
    char* data = (char*)wm_clip_get(&info);
    if (data == NULL) {
        if (errno != ENOENT)
            notify_error("Can't paste", strerror(errno));
        return;
    }
    if (clip_is_pasteable(&info))
        insert_bytes(data, info.size, EDIT_OTHER);
    free(data);
}

//
// Files
//

// The whole file, in a buffer with some room to grow. 0, or -1 with errno.
static int read_file(const char* path, char** out, uint32_t* out_len, uint32_t* out_cap) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return -1;

    char* buf = NULL;
    int err = 0;
    long size = -1;
    uint32_t cap = 0, got = 0;

    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        err = errno ? errno : EIO;
        goto done;
    }
    if ((unsigned long)size > 0x7FFFFFFFul - 4096ul) {
        err = EFBIG;
        goto done;
    }

    cap = (uint32_t)size + 2048u;
    buf = (char*)malloc(cap);
    if (buf == NULL) {
        err = ENOMEM;
        goto done;
    }

    while (got < (uint32_t)size) {
        size_t want = (uint32_t)size - got;
        if (want > IO_CHUNK) want = IO_CHUNK;
        size_t n = fread(buf + got, 1, want, f);
        got += (uint32_t)n;
        if (n < want) {
            if (ferror(f)) err = errno ? errno : EIO;
            break;                  // Else end of file: it got shorter meanwhile
        }
    }

done:
    fclose(f);
    if (err != 0) {
        free(buf);
        errno = err;
        return -1;
    }
    *out = buf;
    *out_len = got;
    *out_cap = cap;
    return 0;
}

// Written in place, so the file keeps its own permissions and clearance
static int write_file(const char* path, const char* data, uint32_t length) {
    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return -1;

    int err = 0;
    for (uint32_t done = 0; done < length && err == 0; ) {
        size_t n = length - done;
        if (n > IO_CHUNK) n = IO_CHUNK;
        if (fwrite(data + done, 1, n, f) != n)
            err = errno ? errno : EIO;
        done += (uint32_t)n;
    }
    if (fclose(f) == EOF && err == 0)
        err = errno ? errno : EIO;

    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

// CR LF line ends: when most lines end that way, the CRs before the line
// breaks come out (Save puts them back). True when they did.
static bool take_out_crlf(char* buf, uint32_t* len) {
    uint32_t crlf = 0, lf = 0;
    for (uint32_t i = 0; i < *len; i++) {
        if (buf[i] != '\n') continue;
        if (i > 0 && buf[i - 1] == '\r') crlf++;
        else                              lf++;
    }
    if (crlf == 0 || crlf < lf)
        return false;
    uint32_t o = 0;
    for (uint32_t i = 0; i < *len; i++) {
        if (buf[i] == '\r' && i + 1 < *len && buf[i + 1] == '\n')
            continue;
        buf[o++] = buf[i];
    }
    *len = o;
    return true;
}

// Save: the line ends the settings ask for
static bool save_crlf(void) {
    if (cfg.eol == EOL_CRLF) return true;
    if (cfg.eol == EOL_LF)   return false;
    return doc.crlf;
}

static void reset_view(void) {
    doc_gen++;                  // A new text
    hist_clear();
    scroll_row = 0;
    scroll_col = 0;
    selecting = false;
    sb_hold.part = SB_NONE;
    has_pref_col = false;
    main_dirty = true;
}

static void new_document(void) {
    free(doc.buf);
    memset(&doc, 0, sizeof(doc));
    reset_view();
}

// Replace the document with the file at `path`. A missing file is fine when
// missing_ok (a new document with that name, created by the first save).
// On failure the user has been told why and the current document stays.
static bool open_document(const char* path, bool missing_ok) {
    char abs[PATH_MAX];
    if (os_abspath(path, abs, sizeof(abs)) == NULL) {
        notify_error("Can't open file", strerror(errno));
        return false;
    }

    char* buf = NULL;
    uint32_t len = 0, cap = 0;
    if (read_file(abs, &buf, &len, &cap) < 0) {
        if (!(errno == ENOENT && missing_ok)) {
            char title[WM_NOTIFY_TITLE_MAX];
            const char* slash = strrchr(abs, '/');
            snprintf(title, sizeof(title), "Can't open %s", slash ? slash + 1 : abs);
            notify_error(title, strerror(errno));
            return false;
        }
    }

    bool crlf = (buf != NULL) && take_out_crlf(buf, &len);

    free(doc.buf);
    memset(&doc, 0, sizeof(doc));
    doc.buf = buf;
    doc.len = len;
    doc.cap = cap;
    doc.crlf = crlf;
    strlcpy(doc.path, abs, sizeof(doc.path));
    reset_view();
    return true;
}

static bool save_document(void) {
    // CR LF: a copy with the CRs put in (a '\n' that has one already is left)
    const char* data = doc.buf;
    uint32_t length = doc.len;
    char* converted = NULL;
    if (save_crlf()) {
        uint32_t extra = 0;
        for (uint32_t i = 0; i < doc.len; i++)
            if (doc.buf[i] == '\n' && !(i > 0 && doc.buf[i - 1] == '\r')) extra++;
        if (extra > 0) {
            converted = (char*)malloc((size_t)doc.len + extra);
            if (converted == NULL) {
                notify_error("Can't save", "Not enough memory");
                return false;
            }
            uint32_t o = 0;
            for (uint32_t i = 0; i < doc.len; i++) {
                if (doc.buf[i] == '\n' && !(i > 0 && doc.buf[i - 1] == '\r'))
                    converted[o++] = '\r';
                converted[o++] = doc.buf[i];
            }
            data = converted;
            length = o;
        }
    }
    int r = write_file(doc.path, data, length);
    free(converted);
    if (r < 0) {
        char title[WM_NOTIFY_TITLE_MAX];
        snprintf(title, sizeof(title), "Can't save %s", doc_name());
        notify_error(title, strerror(errno));
        return false;
    }
    hist_mark_saved();
    doc.modified = false;
    doc.crlf = save_crlf();
    main_dirty = true;
    wm_notify("Saved", doc_name());
    return true;
}

static void run_then(then_t then) {
    switch (then) {
        case THEN_NEW:  new_document();                          break;
        case THEN_OPEN: file_dialog(FILEDLG_OPEN, THEN_NOTHING); break;
        case THEN_EXIT: running = false;                         break;
        case THEN_COMPILE: build_start(BUILD_COMPILE);           break;
        case THEN_RUN:     build_start(BUILD_RUN);               break;
        default:                                                 break;
    }
}

// Save (asking for a name when there is none), then carry on
static void save_then(then_t then) {
    if (doc.path[0] == '\0') {
        file_dialog(FILEDLG_SAVE, then);
        return;
    }
    if (save_document())
        run_then(then);         // A failed save has shown its error: stay
}

// Carry on, but ask first when that would drop unsaved changes
static void ask_then(then_t then) {
    if (doc.modified)
        dialog_open(DLG_CONFIRM, then);
    else
        run_then(then);
}

//
// Drawing: main window
//

static void update_title(void) {
    char title[WM_TITLE_MAX];
    snprintf(title, sizeof(title), "%s%s - " APP_NAME, doc.modified ? "*" : "", doc_name());
    if (strcmp(title, shown_title) != 0 && wm_window_set_title(main_win, title) == 0)
        strlcpy(shown_title, title, sizeof(shown_title));
}

static char glyph(char ch) {
    unsigned char u = (unsigned char)ch;
    if (ch == '\t' || ch == '\0') return ' ';
    if (u < 32 || u > 126)        return '.';     // Control / non-ASCII byte
    return ch;
}

// Cells [from, to) of a row, cut to the cells shown
static void fill_highlight(gfx_surface_t* s, const metrics_t* m, uint32_t vis_row, uint32_t from, uint32_t to) {
    uint32_t shown_end = scroll_col + m->vis_cols + 1;     // + a partly shown cell
    if (from < scroll_col) from = scroll_col;
    if (to > shown_end) to = shown_end;
    if (from >= to)
        return;
    int32_t y = m->text_y + (int32_t)vis_row * m->row_h - ROW_GAP / 2;
    gfx_fill_rect(s, m->text_x + (int32_t)(from - scroll_col) * m->cw, y, (int32_t)(to - from) * m->cw, m->row_h,
                  theme.edit.selection);
}

// Colours for the syntax classes, readable on the text area's background
static gfx_color_t hl_colors[HL_COUNT];

static bool dark_background(void) {
    gfx_color_t bg = theme.edit.background;
    uint32_t lum = ((bg >> 16) & 0xFF) * 3 + ((bg >> 8) & 0xFF) * 6 + (bg & 0xFF);
    return lum < 1280;                              // Out of 2550
}

static void pick_colors(void) {
    bool dark = dark_background();
    hl_colors[HL_TEXT]    = theme.edit.text;
    hl_colors[HL_KEYWORD] = dark ? GFX_RGB(110, 160, 255) : GFX_RGB(0, 0, 190);
    hl_colors[HL_TYPE]    = dark ? GFX_RGB(80, 205, 205)  : GFX_RGB(0, 115, 140);
    hl_colors[HL_PREPROC] = dark ? GFX_RGB(225, 160, 90)  : GFX_RGB(150, 70, 0);
    hl_colors[HL_STRING]  = dark ? GFX_RGB(235, 140, 110) : GFX_RGB(165, 20, 20);
    hl_colors[HL_NUMBER]  = dark ? GFX_RGB(205, 150, 245) : GFX_RGB(120, 40, 170);
    hl_colors[HL_COMMENT] = dark ? GFX_RGB(115, 190, 115) : GFX_RGB(0, 125, 0);
}

static bool highlighting(void) {
    return cfg.highlight && hl_wanted(doc.path);
}

// The cells of a row (line[0, n)) that are shown, in runs of one colour
static void draw_row(gfx_surface_t* s, const metrics_t* m, uint32_t vis_row, const char* line,
                     const uint8_t* cls, uint32_t n) {
    if (n <= scroll_col)
        return;
    uint32_t count = n - scroll_col;
    if (count > m->vis_cols + 1) count = m->vis_cols + 1;
    int32_t y = m->text_y + (int32_t)vis_row * m->row_h;
    if (cls == NULL) {
        gfx_draw_text_n(s, m->text_x, y, line + scroll_col, count, theme.edit.text);
        return;
    }
    uint32_t end = scroll_col + count;
    for (uint32_t k = scroll_col; k < end; ) {
        uint32_t j = k + 1;
        while (j < end && cls[j] == cls[k]) j++;
        // Blanks need no drawing
        uint32_t a = k, b = j;
        while (a < b && line[a] == ' ') a++;
        if (a < b)
            gfx_draw_text_n(s, m->text_x + (int32_t)(a - scroll_col) * m->cw, y, line + a, b - a, hl_colors[cls[k]]);
        k = j;
    }
}

// The number of the line a row starts, right-aligned in the gutter
static void draw_line_number(gfx_surface_t* s, const metrics_t* m, uint32_t vis_row, uint32_t number, bool current) {
    char buf[12];
    int len = snprintf(buf, sizeof(buf), "%u", (unsigned)number);
    int32_t x = m->gutter - GUTTER_PAD - len * m->cw;
    gfx_draw_text(s, x, m->text_y + (int32_t)vis_row * m->row_h, buf,
                  current ? theme.edit.text : theme.client.text_soft);
}

static void draw_text_area(gfx_surface_t* s, const metrics_t* m) {
    static char    line[ROW_MAX + 1];
    static uint8_t line_cls[ROW_MAX + 1];

    gfx_save(s);
    gfx_set_clip(s, &m->view);

    bool colored = highlighting();
    hl_state_t hs;
    hl_init(&hs);
    if (colored)
        pick_colors();
    const uint8_t* cls_row = colored ? line_cls : NULL;

    // The gutter, and which line the caret is on
    uint32_t caret_line = 1;
    if (m->gutter > 0) {
        gfx_fill_rect(s, 0, m->view.y, m->gutter, m->view.h, theme.client.background);
        gfx_draw_vline(s, m->gutter - 1, m->view.y, m->view.h, theme.button.border);
        for (uint32_t i = 0; i < doc.cursor && i < doc.len; i++)
            if (doc.buf[i] == '\n') caret_line++;
    }

    uint32_t first = scroll_row;
    uint32_t last  = scroll_row + m->rows + 1;      // + a partly shown row
    uint32_t sel0 = selection_start(), sel1 = selection_end();

    uint32_t row = 0, col = 0, n = 0;
    int32_t  hl_from = -1;                          // Selected run on this row: [hl_from, hl_to)
    uint32_t hl_to = 0;
    uint32_t text_line = 1;                         // The line the text has got to
    uint32_t row_line = 1;                          // The line this row starts, 0: a wrapped row

    for (uint32_t i = 0; row < last; i++) {
        bool visible = row >= first;

        if (i == doc.len) {
            if (visible) {
                if (hl_from >= 0) fill_highlight(s, m, row - first, (uint32_t)hl_from, hl_to);
                draw_row(s, m, row - first, line, cls_row, n);
                if (m->gutter > 0 && row_line > 0)
                    draw_line_number(s, m, row - first, row_line, row_line == caret_line);
            }
            break;
        }

        char ch = doc.buf[i];
        uint8_t cls = colored ? (uint8_t)hl_step(&hs, doc.buf, doc.len, i) : HL_TEXT;
        if (visible) {
            uint32_t width = (ch == '\n') ? 1 : cell_width(ch, col, m->cols);
            if (i >= sel0 && i < sel1) {
                // A selected line break gets a cell, so selected empty lines show
                if (hl_from < 0) hl_from = (int32_t)col;
                hl_to = col + width;
            } else if (hl_from >= 0) {
                fill_highlight(s, m, row - first, (uint32_t)hl_from, hl_to);
                hl_from = -1;
            }
            if (ch != '\n') {
                char g = glyph(ch);
                for (uint32_t k = 0; k < width; k++) {
                    line_cls[n] = cls;
                    line[n++] = g;
                }
            }
        }

        uint32_t prev = row;
        advance(ch, m->cols, &row, &col);
        if (ch == '\n')
            text_line++;
        if (row != prev) {
            if (visible) {
                // Highlight first, so the glyphs land on top of it
                if (hl_from >= 0) fill_highlight(s, m, prev - first, (uint32_t)hl_from, hl_to);
                draw_row(s, m, prev - first, line, cls_row, n);
                if (m->gutter > 0 && row_line > 0)
                    draw_line_number(s, m, prev - first, row_line, row_line == caret_line);
            }
            n = 0;
            hl_from = -1;
            row_line = (ch == '\n') ? text_line : 0;
        }
    }

    if (main_focused) {
        uint32_t cr, cc;
        pos_of(m->cols, doc.cursor, &cr, &cc);
        if (cr >= first && cr < last && cc >= scroll_col && cc <= scroll_col + m->vis_cols + 1) {
            int32_t y = m->text_y + (int32_t)(cr - first) * m->row_h - ROW_GAP / 2;
            gfx_draw_vline(s, m->text_x + (int32_t)(cc - scroll_col) * m->cw, y + 1, m->row_h - 2,
                           theme.edit.cursor);
        }
    }
    gfx_restore(s);
}

//
// Scroll bars
//

static sbar_t get_sbar(const metrics_t* m, bool vertical) {
    sbar_t b;
    memset(&b, 0, sizeof(b));
    b.vertical = vertical;

    int32_t length;
    if (vertical) {
        b.bar  = gfx_rect(m->view.x + m->view.w, m->view.y, SB_W, m->view.h);
        b.less = gfx_rect(b.bar.x, b.bar.y, SB_W, SB_W);
        b.more = gfx_rect(b.bar.x, b.bar.y + b.bar.h - SB_W, SB_W, SB_W);
        b.track0 = b.bar.y + SB_W;
        length = b.bar.h;
        b.pos  = scroll_row;
        b.page = m->rows;
        b.max  = (m->total_rows > m->rows) ? m->total_rows - m->rows : 0;
    } else {
        b.bar  = gfx_rect(m->view.x, m->view.y + m->view.h, m->view.w, SB_W);
        b.less = gfx_rect(b.bar.x, b.bar.y, SB_W, SB_W);
        b.more = gfx_rect(b.bar.x + b.bar.w - SB_W, b.bar.y, SB_W, SB_W);
        b.track0 = b.bar.x + SB_W;
        length = b.bar.w;
        b.pos  = scroll_col;
        b.page = m->vis_cols;
        b.max  = (m->widest > m->vis_cols) ? m->widest - m->vis_cols : 0;
    }
    if (b.pos > b.max) b.pos = b.max;

    // The thumb is as long against the track as the view is against the text
    b.track_len = length - 2 * SB_W;
    if (b.track_len < SB_MIN_THUMB) {
        b.track_len = (b.track_len < 0) ? 0 : b.track_len;
        return b;                               // No room for a thumb
    }
    uint64_t total = (uint64_t)b.max + b.page;
    int32_t len = (int32_t)((uint64_t)b.track_len * b.page / total);
    if (len < SB_MIN_THUMB) len = SB_MIN_THUMB;
    if (len > b.track_len) len = b.track_len;
    int32_t off = b.max ? (int32_t)((int64_t)(b.track_len - len) * b.pos / b.max) : 0;

    if (vertical) b.thumb = gfx_rect(b.bar.x, b.track0 + off, SB_W, len);
    else          b.thumb = gfx_rect(b.track0 + off, b.bar.y, len, SB_W);
    return b;
}

static int sbar_part(const sbar_t* b, int32_t x, int32_t y) {
    if (!point_in(&b->bar, x, y))   return SB_NONE;
    if (point_in(&b->less, x, y))   return SB_LESS;
    if (point_in(&b->more, x, y))   return SB_MORE;
    if (b->thumb.w == 0)            return SB_NONE;
    if (point_in(&b->thumb, x, y))  return SB_THUMB;
    int32_t p = b->vertical ? y : x, t = b->vertical ? b->thumb.y : b->thumb.x;
    return (p < t) ? SB_PAGE_LESS : SB_PAGE_MORE;
}

// Scrolling with the bars leaves the caret where it is
static void scroll_by(bool vertical, int64_t delta) {
    uint32_t* v = vertical ? &scroll_row : &scroll_col;
    int64_t p = (int64_t)*v + delta;
    *v = (p < 0) ? 0 : (p > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)p;
    metrics_t m = get_metrics();
    clamp_scroll(&m);
    main_dirty = true;
}

// What the held part does, once
static void sbar_step(void) {
    metrics_t m = get_metrics();
    sbar_t b = get_sbar(&m, sb_hold.vertical);
    switch (sb_hold.part) {
        case SB_LESS: scroll_by(b.vertical, -1); break;
        case SB_MORE: scroll_by(b.vertical, 1);  break;

        case SB_PAGE_LESS:
        case SB_PAGE_MORE:
            // A page at a time until the thumb reaches the pointer
            if (sbar_part(&b, sb_hold.x, sb_hold.y) == sb_hold.part)
                scroll_by(b.vertical, (sb_hold.part == SB_PAGE_LESS) ? -(int64_t)b.page : (int64_t)b.page);
            break;
    }
}

// A press on a scroll bar (or the corner between them): true if it took it
static bool sbar_mouse_down(const metrics_t* m, int32_t x, int32_t y) {
    for (int i = 0; i < 2; i++) {
        bool vertical = (i == 0);
        if (!(vertical ? m->vbar : m->hbar))
            continue;
        sbar_t b = get_sbar(m, vertical);
        if (!point_in(&b.bar, x, y))
            continue;

        int part = sbar_part(&b, x, y);
        if (part == SB_NONE)
            return true;
        sb_hold.part = part;
        sb_hold.vertical = vertical;
        sb_hold.x = x;
        sb_hold.y = y;
        if (part == SB_THUMB) {
            sb_hold.grab = vertical ? y - b.thumb.y : x - b.thumb.x;
        } else {
            sbar_step();
            sb_hold.next_ms = os_time_ms() + SB_REPEAT_FIRST;
        }
        main_dirty = true;
        return true;
    }
    return false;
}

static void sbar_drag(void) {
    metrics_t m = get_metrics();
    sbar_t b = get_sbar(&m, sb_hold.vertical);
    int32_t room = b.track_len - (b.vertical ? b.thumb.h : b.thumb.w);
    if (room <= 0 || b.max == 0)
        return;

    int64_t off = (int64_t)(b.vertical ? sb_hold.y : sb_hold.x) - sb_hold.grab - b.track0;
    if (off < 0) off = 0;
    if (off > room) off = room;
    uint32_t pos = (uint32_t)((off * b.max + room / 2) / room);
    if (pos != b.pos) {
        if (b.vertical) scroll_row = pos;
        else            scroll_col = pos;
        main_dirty = true;
    }
}

static void sbar_release(void) {
    if (sb_hold.part != SB_NONE) {
        sb_hold.part = SB_NONE;
        main_dirty = true;
    }
}

// Ms until a held arrow or track repeats: 0 when due, -1 when nothing is held
static int32_t sbar_repeat_wait(void) {
    if (sb_hold.part == SB_NONE || sb_hold.part == SB_THUMB)
        return -1;
    uint64_t now = os_time_ms();
    return (now >= sb_hold.next_ms) ? 0 : (int32_t)(sb_hold.next_ms - now);
}

static void sbar_repeat(void) {
    if (sbar_repeat_wait() != 0)
        return;
    sbar_step();
    sb_hold.next_ms = os_time_ms() + SB_REPEAT_MS;
}

static void draw_sbar_box(gfx_surface_t* s, const gfx_rect_t* r, bool pressed) {
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, pressed ? theme.button.fill_pressed : theme.button.fill);
    gfx_draw_rect(s, r->x, r->y, r->w, r->h, pressed ? theme.button.border_pressed : theme.button.border);
}

// A small triangle pointing up, down, left or right (0 .. 3)
static void draw_arrow(gfx_surface_t* s, const gfx_rect_t* r, int dir, gfx_color_t color) {
    int32_t cx = r->x + r->w / 2, cy = r->y + r->h / 2;
    for (int32_t i = 0; i < 4; i++) {
        int32_t len = 2 * i + 1;
        switch (dir) {
            case 0: gfx_draw_hline(s, cx - i, cy - 2 + i, len, color); break;
            case 1: gfx_draw_hline(s, cx - i, cy + 1 - i, len, color); break;
            case 2: gfx_draw_vline(s, cx - 2 + i, cy - i, len, color); break;
            case 3: gfx_draw_vline(s, cx + 1 - i, cy - i, len, color); break;
        }
    }
}

static void draw_sbar(gfx_surface_t* s, const sbar_t* b) {
    gfx_fill_rect(s, b->bar.x, b->bar.y, b->bar.w, b->bar.h, theme.client.background);
    if (b->vertical) gfx_draw_vline(s, b->bar.x, b->bar.y, b->bar.h, theme.button.border);
    else             gfx_draw_hline(s, b->bar.x, b->bar.y, b->bar.w, theme.button.border);

    int held = (sb_hold.part != SB_NONE && sb_hold.vertical == b->vertical) ? sb_hold.part : SB_NONE;
    draw_sbar_box(s, &b->less, held == SB_LESS);
    draw_sbar_box(s, &b->more, held == SB_MORE);
    // Dimmed when it can't go any further that way
    draw_arrow(s, &b->less, b->vertical ? 0 : 2, b->pos > 0 ? theme.button.text : theme.client.text_soft);
    draw_arrow(s, &b->more, b->vertical ? 1 : 3, b->pos < b->max ? theme.button.text : theme.client.text_soft);

    if (b->thumb.w > 0)
        draw_sbar_box(s, &b->thumb, held == SB_THUMB);
}

static void draw_sbars(gfx_surface_t* s, const metrics_t* m) {
    if (m->vbar) {
        sbar_t b = get_sbar(m, true);
        draw_sbar(s, &b);
    }
    if (m->hbar) {
        sbar_t b = get_sbar(m, false);
        draw_sbar(s, &b);
    }
    if (m->vbar && m->hbar)     // The corner between them
        gfx_fill_rect(s, m->view.x + m->view.w, m->view.y + m->view.h, SB_W, SB_W, theme.button.fill);
}

static void draw_status(gfx_surface_t* s, const metrics_t* m) {
    gfx_fill_rect(s, 0, m->status_y, m->width, STATUS_H, theme.button.fill);
    gfx_draw_hline(s, 0, m->status_y, m->width, theme.button.border);

    unsigned line = 1, column = 1;
    for (uint32_t i = 0; i < doc.cursor; i++) {
        if (doc.buf[i] == '\n') { line++; column = 1; }
        else                      column++;
    }

    char left[64], right[64];
    if (has_selection())
        snprintf(left, sizeof(left), "Ln %u, Col %u   (%u selected)", line, column,
                 (unsigned)(selection_end() - selection_start()));
    else
        snprintf(left, sizeof(left), "Ln %u, Col %u", line, column);
    snprintf(right, sizeof(right), "%s   %s %u   %s   %u %s", highlighting() ? "C" : "Text",
             cfg.use_spaces ? "Spaces" : "Tabs", (unsigned)cfg.tab_width, save_crlf() ? "CRLF" : "LF",
             (unsigned)doc.len, doc.len == 1 ? "byte" : "bytes");

    int32_t y = m->status_y + (STATUS_H - STATUS_TEXT_H) + (STATUS_TEXT_H - gfx_line_height(s)) / 2 + 1;
    gfx_draw_text(s, PAD_X, y, left, theme.button.text);
    gfx_draw_text(s, m->width - PAD_X - gfx_text_width(s, right), y, right, theme.button.text);
}

// Where menu title i starts, and its width
static int32_t menu_x(gfx_surface_t* s, int i, int32_t* w) {
    int32_t x = MENU_X0;
    for (int j = 0; j < i; j++)
        x += gfx_text_width(s, menu_names[j]) + 2 * MENU_PAD;
    *w = gfx_text_width(s, menu_names[i]) + 2 * MENU_PAD;
    return x;
}

static void draw_main(void) {
    main_dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics();
    clamp_scroll(&m);

    gfx_clear(s, theme.edit.background);

    gfx_fill_rect(s, 0, 0, m.width, MENUBAR_H, theme.button.fill);
    gfx_draw_hline(s, 0, MENUBAR_H - 1, m.width, theme.button.border);
    int32_t y = (MENUBAR_H - gfx_line_height(s)) / 2;
    for (int i = 0; i < MENU_COUNT; i++) {
        int32_t w, x = menu_x(s, i, &w);
        gfx_draw_text(s, x + MENU_PAD, y, menu_names[i], theme.button.text);
    }

    draw_text_area(s, &m);
    draw_sbars(s, &m);
    if (m.pane.h > 0)
        pane_paint(s, &m.pane, m.cw, m.row_h);
    draw_status(s, &m);

    wm_window_commit(main_win, NULL);
    update_title();
}

//
// Menus and commands
//

static void show_file_menu(int32_t x) {
    wm_menu_item_t items[] = {
        { "New         Ctrl+N",       CMD_NEW,     0 },
        { "Open...     Ctrl+O",       CMD_OPEN,    0 },
        { "Save        Ctrl+S",       CMD_SAVE,    0 },
        { "Save as...  Ctrl+Shift+S", CMD_SAVE_AS, 0 },
        { NULL,                       0,           WM_MENU_SEPARATOR },
        { "Exit        Alt+X",        CMD_EXIT,    0 },
    };
    wm_menu_show(main_win, x, MENUBAR_H, items, sizeof(items) / sizeof(items[0]));
}

static void show_build_menu(int32_t x) {
    uint32_t busy = build_busy() ? 0 : WM_MENU_DISABLED;
    wm_menu_item_t items[] = {
        { "Compile          F7",       CMD_COMPILE,    0 },
        { "Run              F5",       CMD_RUN,        0 },
        { "Stop             Esc",      CMD_STOP,       busy },
        { NULL,                        0,              WM_MENU_SEPARATOR },
        { "Next error       F8",       CMD_NEXT_ERROR, 0 },
        { "Previous error   Shift+F8", CMD_PREV_ERROR, 0 },
        { NULL,                        0,              WM_MENU_SEPARATOR },
        { "Output pane      F6",       CMD_OUTPUT,     pane_visible() ? WM_MENU_CHECKED : 0 },
    };
    wm_menu_show(main_win, x, MENUBAR_H, items, sizeof(items) / sizeof(items[0]));
}

static void show_settings_menu(int32_t x) {
    wm_menu_item_t items[] = {
        { "Editor...",   CMD_SET_EDITOR,   0 },
        { "Compiler...", CMD_SET_COMPILER, 0 },
    };
    wm_menu_show(main_win, x, MENUBAR_H, items, sizeof(items) / sizeof(items[0]));
}

// Build: the compiler reads the file, so it is saved first (named first,
// for a new document)
static void build(then_t then) {
    if (doc.path[0] == '\0' || doc.modified)
        save_then(then);
    else
        run_then(then);
}

static void show_edit_menu(int32_t x, int32_t y) {
    uint32_t sel = has_selection() ? 0 : WM_MENU_DISABLED;
    wm_menu_item_t items[] = {
        { "Undo        Ctrl+Z", CMD_UNDO,       can_undo() ? 0 : WM_MENU_DISABLED },
        { "Redo        Ctrl+Y", CMD_REDO,       can_redo() ? 0 : WM_MENU_DISABLED },
        { NULL,                 0,              WM_MENU_SEPARATOR },
        { "Cut         Ctrl+X", CMD_CUT,        sel },
        { "Copy        Ctrl+C", CMD_COPY,       sel },
        { "Paste       Ctrl+V", CMD_PASTE,      can_paste() ? 0 : WM_MENU_DISABLED },
        { "Delete      Del",    CMD_DELETE,     sel },
        { NULL,                 0,              WM_MENU_SEPARATOR },
        { "Select all  Ctrl+A", CMD_SELECT_ALL, doc.len ? 0 : WM_MENU_DISABLED },
        { "Word wrap",          CMD_WORD_WRAP,  word_wrap ? WM_MENU_CHECKED : 0 },
    };
    wm_menu_show(main_win, x, y, items, sizeof(items) / sizeof(items[0]));
}

static void command(uint32_t id) {
    switch (id) {
        case CMD_NEW:        ask_then(THEN_NEW);                       break;
        case CMD_OPEN:       ask_then(THEN_OPEN);                      break;
        case CMD_SAVE:       save_then(THEN_NOTHING);                  break;
        case CMD_SAVE_AS:    file_dialog(FILEDLG_SAVE, THEN_NOTHING);  break;
        case CMD_EXIT:       ask_then(THEN_EXIT);                      break;
        case CMD_UNDO:       undo();                                   break;
        case CMD_REDO:       redo();                                   break;
        case CMD_CUT:        if (copy_selection()) delete_selection(); break;
        case CMD_COPY:       copy_selection();                         return;
        case CMD_PASTE:      paste();                                  break;
        case CMD_DELETE:     delete_selection();                       break;
        case CMD_SELECT_ALL: select_range(0, doc.len);                 break;
        case CMD_WORD_WRAP:  word_wrap = !word_wrap; scroll_col = 0;   break;
        case CMD_INDENT:     indent_lines(true);                       break;
        case CMD_UNINDENT:   indent_lines(false);                      break;
        case CMD_COMPILE:    build(THEN_COMPILE);                      return;
        case CMD_RUN:        build(THEN_RUN);                          return;
        case CMD_STOP:       build_stop();                             return;
        case CMD_NEXT_ERROR: build_next_error(1);                      return;
        case CMD_PREV_ERROR: build_next_error(-1);                     return;
        case CMD_OUTPUT:     pane_toggle();                            break;
        case CMD_SET_EDITOR:   settings_open(SETTINGS_EDITOR);         return;
        case CMD_SET_COMPILER: settings_open(SETTINGS_COMPILER);       return;
        default:                                                      return;
    }
    has_pref_col = false;
    changed();
}

//
// Input: main window
//

static void main_navigate(uint16_t key, bool shift, bool ctrl) {
    hist_seal();
    metrics_t m = get_metrics();
    uint32_t row, col;
    pos_of(m.cols, doc.cursor, &row, &col);

    bool vertical = key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN;
    if (!vertical) {
        has_pref_col = false;
    } else if (!has_pref_col) {
        pref_col = col;
        has_pref_col = true;
    }

    switch (key) {
        case KEY_LEFT:
            if (!shift && has_selection())
                move_cursor(selection_start(), false);      // Collapse to the left edge
            else if (ctrl)
                move_cursor(word_left(doc.cursor), shift);
            else if (doc.cursor > 0)
                move_cursor(doc.cursor - 1, shift);
            break;

        case KEY_RIGHT:
            if (!shift && has_selection())
                move_cursor(selection_end(), false);
            else if (ctrl)
                move_cursor(word_right(doc.cursor), shift);
            else if (doc.cursor < doc.len)
                move_cursor(doc.cursor + 1, shift);
            break;

        case KEY_UP:
            move_cursor(row == 0 ? 0 : index_at(m.cols, row - 1, pref_col), shift);
            break;

        case KEY_DOWN:
            move_cursor(index_at(m.cols, row + 1, pref_col), shift);
            break;

        case KEY_PAGE_UP:
            scroll_row = (scroll_row > m.rows) ? scroll_row - m.rows : 0;
            move_cursor(row < m.rows ? 0 : index_at(m.cols, row - m.rows, pref_col), shift);
            break;

        case KEY_PAGE_DOWN:
            scroll_row += m.rows;                           // reveal_cursor() clamps it
            move_cursor(index_at(m.cols, row + m.rows, pref_col), shift);
            break;

        case KEY_HOME:      // Ctrl+Home: start of the document
            move_cursor(ctrl ? 0 : index_at(m.cols, row, 0), shift);
            break;

        case KEY_END:       // Ctrl+End: end of the document
            move_cursor(ctrl ? doc.len : index_at(m.cols, row, m.cols), shift);
            break;
    }
    changed();
}

//
// Code editing: indent, braces
//

static uint32_t line_start(uint32_t i) {
    while (i > 0 && doc.buf[i - 1] != '\n') i--;
    return i;
}

static uint32_t line_end(uint32_t i) {
    while (i < doc.len && doc.buf[i] != '\n') i++;
    return i;
}

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

// Column of index i on its line, tabs expanded
static uint32_t column_of(uint32_t i) {
    uint32_t c = 0;
    for (uint32_t k = line_start(i); k < i; k++)
        c += (doc.buf[k] == '\t') ? cfg.tab_width - c % cfg.tab_width : 1;
    return c;
}

// One indent level put in at column col: spaces to the next tab stop, or a tab
static uint32_t indent_unit(char* out, uint32_t col) {
    if (!cfg.use_spaces) {
        out[0] = '\t';
        return 1;
    }
    uint32_t n = cfg.tab_width - col % cfg.tab_width;
    memset(out, ' ', n);
    return n;
}

// Bytes of one level of indent at the start of buf[0, n): a tab, or up to
// tab_width spaces
static uint32_t one_level(const char* buf, uint32_t n) {
    if (n > 0 && buf[0] == '\t')
        return 1;
    uint32_t k = 0;
    while (k < n && k < cfg.tab_width && buf[k] == ' ') k++;
    return k;
}

// Tab / Shift+Tab on the lines the selection touches (the caret's line
// with none): a level more or less on each. One undo step.
static void indent_lines(bool more) {
    uint32_t a = selection_start(), b = selection_end();
    bool had_selection = has_selection();
    if (b > a && line_start(b) == b)
        b--;                        // Ending at a line's start leaves that line be
    uint32_t first = line_start(a), last = line_end(b);
    uint32_t span = last - first;

    // Each line gets at most TAB_WIDTH_MAX more bytes
    uint32_t lines = 1;
    for (uint32_t i = first; i < last; i++)
        if (doc.buf[i] == '\n') lines++;
    char* out = (char*)malloc((size_t)span + (size_t)lines * TAB_WIDTH_MAX + 1);
    if (out == NULL) {
        notify_error(APP_NAME, "Not enough memory");
        return;
    }

    uint32_t o = 0, i = first;
    int64_t caret_delta = 0;                    // On the caret's line, for no selection
    while (i <= last) {
        uint32_t e = i;
        while (e < last && doc.buf[e] != '\n') e++;
        int64_t delta = 0;
        if (more) {
            if (e > i || !had_selection) {      // Empty lines in a block stay empty
                uint32_t n = indent_unit(out + o, 0);
                o += n;
                delta = n;
            }
            memcpy(out + o, doc.buf + i, e - i);
            o += e - i;
        } else {
            uint32_t cut = one_level(doc.buf + i, e - i);
            memcpy(out + o, doc.buf + i + cut, e - i - cut);
            o += e - i - cut;
            delta = -(int64_t)cut;
        }
        if (doc.cursor >= i && doc.cursor <= e)
            caret_delta = delta;
        if (e < last) out[o++] = '\n';
        i = e + 1;
    }

    if (o == span && memcmp(out, doc.buf + first, span) == 0) {
        free(out);                              // Nothing to unindent
        return;
    }
    uint32_t caret = doc.cursor;
    if (replace_range(first, last, out, o, EDIT_OTHER, true)) {
        if (had_selection) {
            select_range(first, first + o);
        } else {
            int64_t c = (int64_t)caret + caret_delta;
            if (c < (int64_t)first) c = first;
            move_cursor((uint32_t)c, false);
        }
        hist_note_after();
    }
    free(out);
}

// Enter: the line's indent again, one level more after '{'. Between '{'
// and '}' the '}' goes to a line of its own below.
static void newline(void) {
    if (!cfg.auto_indent) {
        char ch = '\n';
        insert_bytes(&ch, 1, EDIT_OTHER);
        return;
    }
    uint32_t at = selection_start(), after = selection_end();
    uint32_t ls = line_start(at);
    uint32_t ind = ls;
    while (ind < at && is_blank(doc.buf[ind])) ind++;
    uint32_t ind_len = ind - ls;

    uint32_t p = at;
    while (p > ls && is_blank(doc.buf[p - 1])) p--;
    bool opened = (p > ls && doc.buf[p - 1] == '{');
    uint32_t q = after;
    while (q < doc.len && is_blank(doc.buf[q])) q++;
    bool closes = opened && q < doc.len && doc.buf[q] == '}';

    char buf[2 * 256 + 2 * TAB_WIDTH_MAX + 4];
    if (ind_len > 256) ind_len = 256;
    uint32_t n = 0;
    buf[n++] = '\n';
    memcpy(buf + n, doc.buf + ls, ind_len);
    n += ind_len;
    uint32_t caret = n;
    if (opened) {
        n += indent_unit(buf + n, column_of(ind));
        caret = n;
    }
    if (closes) {
        buf[n++] = '\n';
        memcpy(buf + n, doc.buf + ls, ind_len);
        n += ind_len;
    }
    // Blanks before the caret on the old line, and after it, go
    if (insert_bytes_range(p, closes ? q : after, buf, n)) {
        move_cursor(p + caret, false);
        hist_note_after();
    }
}

static char closer_of(char c) {
    switch (c) {
        case '(':  return ')';
        case '[':  return ']';
        case '{':  return '}';
        case '"':  return '"';
        case '\'': return '\'';
        default:   return 0;
    }
}

// A typed character, with brace completion and '}' unindenting. False:
// nothing special, type it as it is.
static bool smart_char(char c) {
    uint32_t at = doc.cursor;
    char next = (at < doc.len) ? doc.buf[at] : '\0';
    char prev = (at > 0) ? doc.buf[at - 1] : '\0';

    if (cfg.brace_complete) {
        char close = closer_of(c);

        // A selection gets wrapped
        if (close && has_selection()) {
            uint32_t a = selection_start(), b = selection_end();
            char* buf = (char*)malloc((size_t)(b - a) + 2);
            if (buf == NULL) return false;
            buf[0] = c;
            memcpy(buf + 1, doc.buf + a, b - a);
            buf[b - a + 1] = close;
            if (replace_range(a, b, buf, b - a + 2, EDIT_OTHER, true)) {
                select_range(a + 1, b + 1);
                hist_note_after();
            }
            free(buf);
            return true;
        }

        // Typing the closer that is already there steps over it
        if (!has_selection() && (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') && next == c) {
            hist_seal();
            move_cursor(at + 1, false);
            return true;
        }

        if (close && !has_selection()) {
            bool pair;
            if (c == '"' || c == '\'')
                pair = !char_is_word(prev) && prev != '\\' && !char_is_word(next);
            else
                pair = next == '\0' || is_blank(next) || next == '\n' || next == '\r' ||
                       next == ')' || next == ']' || next == '}' || next == ';' || next == ',';
            if (pair) {
                char two[2] = { c, close };
                if (insert_bytes(two, 2, EDIT_OTHER)) {
                    move_cursor(at + 1, false);
                    hist_note_after();
                }
                return true;
            }
        }
    }

    // '}' first on its line takes a level of indent back off
    if (c == '}' && cfg.auto_indent && !has_selection()) {
        uint32_t ls = line_start(at);
        uint32_t k = ls;
        while (k < at && is_blank(doc.buf[k])) k++;
        if (k == at && at > ls) {
            uint32_t len = at - ls;
            uint32_t cut = one_level(doc.buf + ls, len);
            char buf[1024];
            if (len - cut + 1 > sizeof(buf)) return false;
            memcpy(buf, doc.buf + ls + cut, len - cut);
            buf[len - cut] = '}';
            replace_range(ls, at, buf, len - cut + 1, EDIT_OTHER, true);
            return true;
        }
    }
    return false;
}

// Backspace: an empty pair goes together; in spaces before the text, back
// to the tab stop before
static bool smart_backspace(void) {
    if (has_selection() || doc.cursor == 0)
        return false;
    uint32_t at = doc.cursor;
    char prev = doc.buf[at - 1];
    if (cfg.brace_complete && at < doc.len && closer_of(prev) != 0 && doc.buf[at] == closer_of(prev)) {
        delete_range(at - 1, at + 1);
        return true;
    }
    if (cfg.use_spaces && prev == ' ') {
        uint32_t ls = line_start(at);
        uint32_t k = ls;
        while (k < at && doc.buf[k] == ' ') k++;
        if (k == at) {                  // Only spaces before the caret
            uint32_t col = at - ls;
            uint32_t target = ((col - 1) / cfg.tab_width) * cfg.tab_width;
            delete_range(ls + target, at);
            return true;
        }
    }
    return false;
}

static void main_key(const wm_event_t* ev) {
    uint16_t key = ev->key;
    bool shift = (ev->mods & INPUT_MOD_SHIFT) != 0;
    bool ctrl  = (ev->mods & INPUT_MOD_CTRL) != 0;

    bool nav = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
               key == KEY_HOME || key == KEY_END || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN;
    if (nav) {
        main_navigate(key, shift, ctrl);
        return;
    }

    switch (key) {
        case KEY_F5: command(CMD_RUN);                                return;
        case KEY_F6: command(CMD_OUTPUT);                             return;
        case KEY_F7: command(CMD_COMPILE);                            return;
        case KEY_F8: command(shift ? CMD_PREV_ERROR : CMD_NEXT_ERROR); return;
        case KEY_ESCAPE:
            if (build_busy()) {
                build_stop();
                return;
            }
            break;
    }

    // Alt+S saves, Alt+X exits; Alt with anything else types nothing
    if ((ev->mods & INPUT_MOD_ALT) && !ctrl) {
        char letter = ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) ? (char)(key | 0x20) : 0;
        if (letter == 's')
            command(CMD_SAVE);
        else if (letter == 'x')
            command(CMD_EXIT);
        return;
    }

    if (ctrl) {
        // Ctrl+Backspace / Ctrl+Delete: a word at a time
        if (key == KEY_BACKSPACE || key == KEY_DELETE) {
            if (!delete_selection()) {
                if (key == KEY_BACKSPACE) delete_range(word_left(doc.cursor), doc.cursor);
                else                      delete_range(doc.cursor, word_right(doc.cursor));
            }
            has_pref_col = false;
            changed();
            return;
        }

        // The letter, whether the dwm sends 'c', 'C' or the control code 3
        char letter = 0;
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))
            letter = (char)(key | 0x20);
        else if (key >= 3 && key <= 26 && key != KEY_TAB)
            letter = (char)('a' + key - 1);

        switch (letter) {
            case 'a': command(CMD_SELECT_ALL);                 break;
            case 'z': command(shift ? CMD_REDO : CMD_UNDO);    break;
            case 'y': command(CMD_REDO);                       break;
            case 'c': command(CMD_COPY);                       break;
            case 'x': command(CMD_CUT);                        break;
            case 'v': command(CMD_PASTE);                      break;
            case 's': command(shift ? CMD_SAVE_AS : CMD_SAVE); break;
            case 'o': command(CMD_OPEN);                       break;
            case 'n': command(CMD_NEW);                        break;
            case 'b': command(CMD_COMPILE);                    break;
            case 'r': command(CMD_RUN);                        break;
            default:                                           break;   // Swallow the rest
        }
        return;
    }

    char ch;
    switch (key) {
        case KEY_DELETE:
            if (!delete_selection() && doc.cursor < doc.len)
                replace_range(doc.cursor, doc.cursor + 1, NULL, 0, EDIT_DELETE, true);
            break;

        case KEY_BACKSPACE:
            if (smart_backspace())
                break;
            if (!delete_selection() && doc.cursor > 0)
                replace_range(doc.cursor - 1, doc.cursor, NULL, 0, EDIT_BACKSPACE, true);
            break;

        case KEY_ESCAPE:
            move_cursor(doc.cursor, false);
            hist_seal();
            break;

        case KEY_ENTER:                     // A step of its own
            newline();
            break;

        case KEY_TAB: {
            // Over several lines, or Shift: indent / unindent them
            bool lines = has_selection() && line_start(selection_start()) != line_start(selection_end());
            if (shift || lines) {
                indent_lines(!shift);
            } else {
                char unit[TAB_WIDTH_MAX];
                uint32_t n = indent_unit(unit, column_of(selection_start()));
                insert_bytes(unit, n, EDIT_TYPE);
            }
            break;
        }

        default:
            if (key < 32 || key > 255 || key == 127)
                return;                     // A special key with no character
            ch = (char)key;
            if (!smart_char(ch))
                insert_bytes(&ch, 1, EDIT_TYPE);
            break;
    }
    has_pref_col = false;
    changed();
}

static void main_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;

    metrics_t m = get_metrics();
    if (ev->y < MENUBAR_H) {
        gfx_surface_t* s = wm_window_surface(main_win);
        for (int i = 0; s != NULL && i < MENU_COUNT; i++) {
            int32_t w, x = menu_x(s, i, &w);
            if (ev->x < x || ev->x >= x + w)
                continue;
            switch (i) {
                case MENU_FILE:     show_file_menu(x);          break;
                case MENU_EDIT:     show_edit_menu(x, MENUBAR_H); break;
                case MENU_BUILD:    show_build_menu(x);         break;
                case MENU_SETTINGS: show_settings_menu(x);      break;
            }
        }
        return;
    }
    if (m.pane.h > 0 && point_in(&m.pane, ev->x, ev->y)) {
        pane_mouse_down(&m.pane, ev->x, ev->y, m.row_h);
        return;
    }
    if (sbar_mouse_down(&m, ev->x, ev->y) || !point_in(&m.view, ev->x, ev->y))
        return;

    uint32_t index = index_from_point(&m, ev->x, ev->y);
    has_pref_col = false;
    hist_seal();
    if (ev->clicks >= 2) {
        select_word_at(index);
        selecting = false;
    } else {
        move_cursor(index, (ev->mods & INPUT_MOD_SHIFT) != 0);     // Shift+click extends
        selecting = true;
    }
    changed();
}

static void main_mouse_move(const wm_event_t* ev) {
    if (sb_hold.part != SB_NONE) {
        if (!(ev->buttons & INPUT_BTN_LEFT)) {
            sbar_release();
            return;
        }
        sb_hold.x = ev->x;
        sb_hold.y = ev->y;
        if (sb_hold.part == SB_THUMB)
            sbar_drag();
        return;
    }
    if (!selecting)
        return;
    if (!(ev->buttons & INPUT_BTN_LEFT)) {
        selecting = false;
        return;
    }
    // The pointer may be outside the window while dragging: the text scrolls
    metrics_t m = get_metrics();
    uint32_t index = index_from_point(&m, ev->x, ev->y);
    if (index != doc.cursor) {
        move_cursor(index, true);
        changed();
    }
}

// Wheel: WHEEL_ROWS rows a notch, Ctrl a page a notch, Shift sideways.
// + toward the user scrolls down (further into the text).
static void main_wheel(const wm_event_t* ev) {
    if (ev->wheel == 0)
        return;
    metrics_t pm = get_metrics();
    if (pm.pane.h > 0 && point_in(&pm.pane, ev->x, ev->y)) {
        pane_wheel(ev->wheel);
        return;
    }
    int64_t notches = ev->wheel;
    if (ev->mods & INPUT_MOD_SHIFT) {
        scroll_by(false, notches * WHEEL_COLS);
    } else if (ev->mods & INPUT_MOD_CTRL) {
        metrics_t m = get_metrics();
        scroll_by(true, notches * (int64_t)m.rows);
    } else {
        scroll_by(true, notches * WHEEL_ROWS);
    }

    // Dragging a selection: it follows the text that moved under the pointer
    if (selecting && (ev->buttons & INPUT_BTN_LEFT)) {
        metrics_t m = get_metrics();
        uint32_t index = index_from_point(&m, ev->x, ev->y);
        if (index != doc.cursor) {
            move_cursor(index, true);
            main_dirty = true;
        }
    }
}

static void main_event(const wm_event_t* ev) {
    // A dialog is up: it gets the user's attention instead
    wm_window_t up = (dlg.win != 0) ? dlg.win : filedlg_window(fdlg);
    if (up == 0)
        up = settings_window();
    if (up != 0) {
        switch (ev->type) {
            case WM_EVENT_KEY:
            case WM_EVENT_MOUSE_DOWN:
            case WM_EVENT_CONTEXT_MENU:
            case WM_EVENT_CLOSE:
                wm_window_focus(up);
                return;
        }
    }

    switch (ev->type) {
        case WM_EVENT_KEY:          main_key(ev);                  break;
        case WM_EVENT_MOUSE_DOWN:   main_mouse_down(ev);           break;
        case WM_EVENT_MOUSE_MOVE:   main_mouse_move(ev);           break;
        case WM_EVENT_MOUSE_WHEEL:  main_wheel(ev);                break;
        case WM_EVENT_MOUSE_UP:     selecting = false; sbar_release(); break;
        case WM_EVENT_CONTEXT_MENU: show_edit_menu(ev->x, ev->y);  break;
        case WM_EVENT_MENU_COMMAND: command(ev->param);            break;
        case WM_EVENT_CLOSE:        ask_then(THEN_EXIT);           break;

        case WM_EVENT_FOCUS_GAINED:
        case WM_EVENT_FOCUS_LOST:
            main_focused = (ev->type == WM_EVENT_FOCUS_GAINED);
            selecting = false;
            sbar_release();
            main_dirty = true;
            break;

        case WM_EVENT_RESIZED:
            has_pref_col = false;
            changed();
            break;

        case WM_EVENT_DESTROYED:
            main_win = 0;
            running = false;
            break;
    }
}

//
// Dialogs: "save changes?", and the file dialog (os/filedlg.h) for Open and
// Save as
//

static int dialog_buttons(const char** labels) {
    labels[0] = "Save";
    labels[1] = "Don't save";
    labels[2] = "Cancel";
    return 3;
}

static gfx_rect_t button_rect(int i, int count) {
    int32_t w, h;
    wm_window_get_size(dlg.win, &w, &h);
    int32_t x = w - DLG_MARGIN - (count - i) * BTN_W - (count - 1 - i) * BTN_GAP;
    return gfx_rect(x, h - DLG_MARGIN - BTN_H, BTN_W, BTN_H);
}

static int button_at(int32_t x, int32_t y) {
    const char* labels[3];
    int count = dialog_buttons(labels);
    for (int i = 0; i < count; i++) {
        gfx_rect_t r = button_rect(i, count);
        if (point_in(&r, x, y)) return i;
    }
    return -1;
}

static void dialog_close(void) {
    if (dlg.win != 0)
        wm_window_destroy(dlg.win);
    dlg.win = 0;
    dlg.kind = DLG_NONE;
    if (main_win != 0)
        wm_window_focus(main_win);
}

void draw_button(gfx_surface_t* s, const gfx_rect_t* r, const char* label, bool focused, bool pressed) {
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, pressed ? theme.button.fill_pressed : theme.button.fill);
    gfx_color_t border = pressed ? theme.button.border_pressed
                       : focused ? theme.button.border_active : theme.button.border;
    gfx_draw_rect(s, r->x, r->y, r->w, r->h, border);
    if (focused)
        gfx_draw_rect(s, r->x + 1, r->y + 1, r->w - 2, r->h - 2, border);
    int32_t tw = gfx_text_width(s, label);
    gfx_draw_text(s, r->x + (r->w - tw) / 2, r->y + (r->h - gfx_line_height(s)) / 2, label, theme.button.text);
}

static void draw_dialog(void) {
    dlg.dirty = false;
    gfx_surface_t* s = wm_window_surface(dlg.win);
    if (s == NULL)
        return;
    int32_t lh = gfx_line_height(s);

    gfx_clear(s, theme.client.background);
    gfx_draw_text(s, DLG_MARGIN, 14, dlg.message, theme.client.text);
    gfx_draw_text(s, DLG_MARGIN, 14 + lh + 6, "Changes are lost if you don't save them.",
                  theme.client.text_soft);

    const char* labels[3];
    int count = dialog_buttons(labels);
    for (int i = 0; i < count; i++) {
        gfx_rect_t r = button_rect(i, count);
        draw_button(s, &r, labels[i], i == dlg.focus, i == dlg.pressed);
    }

    wm_window_commit(dlg.win, NULL);
}

static void dialog_open(dlg_kind_t kind, then_t then) {
    if (dlg.win != 0)
        dialog_close();

    memset(&dlg, 0, sizeof(dlg));
    dlg.kind = kind;
    dlg.then = then;
    dlg.pressed = -1;

    // "Save changes to <name>?", a long name cut to fit the window
    char name[32];
    const char* full = doc_name();
    if (strlen(full) < sizeof(name)) {
        strlcpy(name, full, sizeof(name));
    } else {
        memcpy(name, full, sizeof(name) - 4);
        strlcpy(name + sizeof(name) - 4, "...", 4);
    }
    snprintf(dlg.message, sizeof(dlg.message), "Save changes to %s?", name);

    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title  = APP_NAME;
    d.style  = WM_STYLE_NOMINIMIZE;         // A question: answered, not put away
    d.x      = (sw > DLG_W) ? (sw - DLG_W) / 2 : WM_POS_DEFAULT;
    d.y      = (sh > DLG_CONFIRM_H) ? (sh - DLG_CONFIRM_H) / 2 - 40 : WM_POS_DEFAULT;
    d.width  = DLG_W;
    d.height = DLG_CONFIRM_H;

    dlg.win = wm_window_create(&d);
    if (dlg.win == 0) {
        // Couldn't ask: keep the document rather than drop it
        notify_error(APP_NAME, "Save your changes before closing");
        dlg.kind = DLG_NONE;
        return;
    }
    draw_dialog();
    wm_window_focus(dlg.win);
}

static void confirm_answer(int choice) {
    then_t then = dlg.then;
    dialog_close();
    if (choice == 0)
        save_then(then);
    else if (choice == 1)
        run_then(then);
    // 2: Cancel
}

static void dialog_key(const wm_event_t* ev) {
    switch (ev->key) {
        case KEY_ESCAPE: dialog_close();                  return;
        case KEY_LEFT:   dlg.focus = (dlg.focus + 2) % 3; break;
        case KEY_RIGHT:
        case KEY_TAB:    dlg.focus = (dlg.focus + 1) % 3; break;
        case KEY_ENTER:
        case ' ':        confirm_answer(dlg.focus);       return;
        case 's': case 'S':                     confirm_answer(0); return;
        case 'n': case 'N': case 'd': case 'D': confirm_answer(1); return;
    }
    dlg.dirty = true;
}

static void dialog_event(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_KEY:
            dialog_key(ev);
            break;

        case WM_EVENT_CLOSE:
            dialog_close();         // Same as Cancel
            break;

        case WM_EVENT_MOUSE_DOWN: {
            if (ev->button != INPUT_BTN_LEFT)
                break;
            int hit = button_at(ev->x, ev->y);
            if (hit >= 0)
                dlg.pressed = dlg.focus = hit;
            dlg.dirty = true;
            break;
        }

        case WM_EVENT_MOUSE_UP: {
            if (dlg.pressed < 0)
                break;
            int pressed = dlg.pressed;
            dlg.pressed = -1;
            dlg.dirty = true;
            if (button_at(ev->x, ev->y) == pressed)     // Released over the same button
                confirm_answer(pressed);
            break;
        }

        case WM_EVENT_MOUSE_MOVE:
            if (dlg.pressed >= 0 && !(ev->buttons & INPUT_BTN_LEFT)) {
                dlg.pressed = -1;
                dlg.dirty = true;
            }
            break;

        case WM_EVENT_RESIZED:
        case WM_EVENT_FOCUS_GAINED:
        case WM_EVENT_FOCUS_LOST:
            dlg.dirty = true;
            break;

        case WM_EVENT_DESTROYED:
            dlg.win = 0;
            dlg.kind = DLG_NONE;
            break;
    }
}

static void file_dialog_close(void) {
    filedlg_close(fdlg);        // Focuses the main window again
    fdlg = NULL;
}

// Open or Save as. Starts at the document: its folder, its name for a save
// (or DEFAULT_NAME in that folder or the cwd).
static void file_dialog(filedlg_mode_t mode, then_t then) {
    if (fdlg != NULL)
        file_dialog_close();

    filedlg_desc_t d;
    memset(&d, 0, sizeof(d));
    d.mode  = mode;
    d.path  = doc.path[0] != '\0' ? doc.path : NULL;
    d.name  = DEFAULT_NAME;
    d.owner = main_win;

    fdlg = filedlg_open(&d);
    if (fdlg == NULL) {
        notify_error(APP_NAME, strerror(errno));
        return;
    }
    fdlg_then = then;
}

static void file_dialog_accept(void) {
    const char* path = filedlg_path(fdlg);

    if (filedlg_mode(fdlg) == FILEDLG_OPEN) {
        if (open_document(path, false)) {           // Else: told why, the dialog stays
            file_dialog_close();
            changed();
        }
        return;
    }

    // Save as
    char old[PATH_MAX];
    strlcpy(old, doc.path, sizeof(old));
    strlcpy(doc.path, path, sizeof(doc.path));
    if (!save_document()) {
        strlcpy(doc.path, old, sizeof(doc.path));
        return;
    }

    then_t then = fdlg_then;
    file_dialog_close();
    run_then(then);
}

// False: not the file dialog's event
static bool file_dialog_event(const wm_event_t* ev) {
    switch (filedlg_event(fdlg, ev)) {
        case FILEDLG_IGNORED: return false;
        case FILEDLG_ACCEPT:  file_dialog_accept(); break;
        case FILEDLG_CANCEL:  file_dialog_close();  break;
        case FILEDLG_HANDLED:                       break;
    }
    return true;
}

//
// For the other parts (cedit.h)
//

const char* doc_path(void)       { return doc.path; }
bool        doc_is_modified(void) { return doc.modified; }
void        editor_redraw(void)   { main_dirty = true; }

void doc_goto_line(uint32_t line, uint32_t column) {
    uint32_t i = 0;
    for (uint32_t l = 1; l < line && i < doc.len; i++)
        if (doc.buf[i] == '\n') l++;
    uint32_t end = line_end(i);
    for (uint32_t c = 1; c < column && i < end; c++)
        i++;
    hist_seal();
    move_cursor(i, false);
    has_pref_col = false;

    // The line a third of the way down, if it was off screen
    metrics_t m = get_metrics();
    uint32_t row, col;
    pos_of(m.cols, doc.cursor, &row, &col);
    if (row < scroll_row || row >= scroll_row + m.rows)
        scroll_row = (row > m.rows / 3) ? row - m.rows / 3 : 0;
    changed();
}

void editor_settings_changed(void) {
    doc_gen++;                      // The tab width changes the layout
    has_pref_col = false;
    changed();
}

//
// Main
//

int main(int argc, char** argv) {
    load_theme();
    settings_load();

    if (argc > 1)
        open_document(argv[1], true);       // On failure: told why, starts Untitled

    char title[WM_TITLE_MAX];
    snprintf(title, sizeof(title), "%s - " APP_NAME, doc_name());

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title      = title;
    d.x          = WM_POS_DEFAULT;
    d.y          = WM_POS_DEFAULT;
    d.width      = WIN_W;
    d.height     = WIN_H;
    d.min_width  = WIN_MIN_W;
    d.min_height = WIN_MIN_H;
    d.style      = WM_STYLE_RESIZABLE | WM_STYLE_CONTEXT_MENU;

    main_win = wm_window_create(&d);
    if (main_win == 0) {
        fprintf(stderr, APP_NAME ": can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    strlcpy(shown_title, title, sizeof(shown_title));
    draw_main();
    wm_window_focus(main_win);

    // Events are handled as they come; the windows are repainted once the
    // queue runs dry, so a burst of keys or a drag costs one frame. A held
    // scroll bar arrow or track wakes the loop to repeat.
    // While the compiler or a program runs, the loop also wakes every
    // BUILD_POLL_MS to read what it wrote.
    wm_event_t ev;
    while (running) {
        sbar_repeat();
        build_poll();
        bool dirty = main_dirty || (dlg.win != 0 && dlg.dirty) || filedlg_dirty(fdlg) || settings_dirty();
        int32_t wait = sbar_repeat_wait();
        if (build_busy() && (wait < 0 || wait > BUILD_POLL_MS))
            wait = BUILD_POLL_MS;
        int r = wm_wait_event(&ev, dirty ? 0 : wait);
        if (r < 0)
            break;                          // The dwm went away
        if (r == 0) {
            if (main_dirty) draw_main();
            if (dlg.win != 0 && dlg.dirty) draw_dialog();
            if (settings_dirty()) settings_paint();
            filedlg_paint(fdlg);
            continue;
        }

        if (file_dialog_event(&ev))
            continue;                       // Its window 0 events still come through
        if (ev.type == WM_EVENT_THEME_CHANGED) {
            load_theme();
            main_dirty = true;
            dlg.dirty = true;
            settings_theme_changed();
        } else if (ev.window == main_win) {
            main_event(&ev);
        } else if (dlg.win != 0 && ev.window == dlg.win) {
            dialog_event(&ev);
        } else if (settings_window() != 0 && ev.window == settings_window()) {
            settings_event(&ev);
        }
    }

    build_stop();
    wm_disconnect();
    free(doc.buf);
    return 0;
}
