/*
 * hexedit: a byte editor for the desktop.
 *
 *     hexedit [file]
 *
 * One file per window, shown as offset / hex / ASCII columns. A file that
 * does not exist yet opens empty and is created by the first save.
 *
 * Editing overwrites: hex digits change the nibble under the caret in the
 * hex column, printable keys change the byte in the ASCII column (Tab
 * switches columns). Typing at the end of the file appends. Delete and
 * Backspace remove a byte, Ctrl+B inserts a zero byte at the caret.
 *
 * The mouse wheel scrolls WHEEL_ROWS rows a notch (Ctrl: a page), leaving
 * the caret where it is (the next key brings it back into view).
 *
 * Built on os/wm.h, the same way as notepad: the dwm provides the window,
 * the menus and notifications, libc's file dialog (os/filedlg.h) Open and
 * Save as; the "save changes?" question is a small window of this
 * program's own.
 *
 * Needs SEC_FILE_READ / SEC_FILE_WRITE, SEC_GFX and SEC_IPC (the default
 * clearance has them all).
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

#ifndef APP_NAME
#define APP_NAME        "hexedit"
#endif

// Main window
#define WIN_W           660
#define WIN_H           440
#define WIN_MIN_W       220
#define WIN_MIN_H       140

#define MENUBAR_H       16
#define MENU_FILE_X     6
#define MENU_EDIT_X     44
#define MENU_ITEM_W     36

#define STATUS_H        14
#define PAD_X           8
#define PAD_Y           8
#define ROW_GAP         4           // Pixels between text rows
#define WHEEL_ROWS      3           // Rows a wheel notch scrolls (Ctrl: a page)

// Columns, in cells: "00000000  00 01 02 03 04 05 06 07  08 ... 0F  ................"
#define OFFSET_CELLS    8
#define HEX_X           (OFFSET_CELLS + 2)
#define BPR_MAX         16          // Bytes per row when the window is wide enough
#define BPR_MIN         4

#define IO_CHUNK        (64u * 1024u)   // Kept well under the kernel's SYS_RW_MAX

// Dialogs
#define DLG_W           340
#define DLG_CONFIRM_H   90
#define DLG_MARGIN      12
#define BTN_W           80
#define BTN_H           20
#define BTN_GAP         8
#define DEFAULT_NAME    "untitled.bin"   // Save as offers it for a new document

enum {
    CMD_NEW = 1,
    CMD_OPEN,
    CMD_SAVE,
    CMD_SAVE_AS,
    CMD_EXIT,
    CMD_INSERT,
    CMD_DELETE,
    CMD_SWITCH_PANE,
};

// What to do once the current document is safe (saved or knowingly dropped)
typedef enum { THEN_NOTHING, THEN_NEW, THEN_OPEN, THEN_EXIT } then_t;

typedef enum { DLG_NONE, DLG_CONFIRM } dlg_kind_t;

typedef enum { PANE_HEX, PANE_ASCII } pane_t;

typedef struct {
    uint8_t* buf;
    uint32_t len;
    uint32_t cap;
    uint32_t cursor;                // Byte under the caret; == len: the append cell
    uint32_t nibble;                // Hex column: 0 = high digit, 1 = low digit
    bool     modified;
    char     path[PATH_MAX];        // Absolute; "" = Untitled
} doc_t;

typedef struct {
    int32_t  width, height;         // Client size
    int32_t  cw, row_h;             // Cell width, row pitch
    int32_t  header_y;              // Glyph top of the column header row
    int32_t  text_x, text_y;        // Top-left of the glyphs of the first shown row
    int32_t  status_y;
    uint32_t bpr;                   // Bytes per row
    uint32_t ascii_x;               // First cell of the ASCII column
    uint32_t rows;                  // Whole rows that fit
} metrics_t;

static doc_t        doc;
static wm_window_t  main_win;
static wm_theme_t   theme;
static bool         running = true;
static bool         main_dirty;
static bool         main_focused = true;
static uint32_t     scroll_row;     // First row shown
static pane_t       pane = PANE_HEX;
static char         shown_title[WM_TITLE_MAX];

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

//
// Small helpers
//

static void notify_error(const char* title, const char* message) {
    wm_notify_ex(WM_NOTIFY_ERROR, title, message, WM_NOTIFY_DEFAULT_MS * 2);
}

static const char* doc_name(void) {
    if (doc.path[0] == '\0') return "Untitled";
    const char* slash = strrchr(doc.path, '/');
    return slash ? slash + 1 : doc.path;
}

static bool point_in(const gfx_rect_t* r, int32_t x, int32_t y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static int hex_value(uint16_t key) {
    if (key >= '0' && key <= '9') return key - '0';
    if (key >= 'a' && key <= 'f') return key - 'a' + 10;
    if (key >= 'A' && key <= 'F') return key - 'A' + 10;
    return -1;
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
// Row r shows bytes [r * bpr, (r + 1) * bpr). The row holding `len` also
// shows the append cell, so an empty file still has somewhere to type.
// The hex column has an extra blank cell after every 8 bytes.
//

static uint32_t hex_cell(uint32_t col) {
    return HEX_X + col * 3 + col / 8;
}

static uint32_t ascii_start(uint32_t bpr) {
    return hex_cell(bpr - 1) + 2 + 2;
}

static metrics_t get_metrics(void) {
    metrics_t m;
    gfx_surface_t* s = wm_window_surface(main_win);
    wm_window_get_size(main_win, &m.width, &m.height);
    m.cw       = s ? gfx_char_width(s) : 6;
    m.row_h    = (s ? gfx_line_height(s) : 8) + ROW_GAP;
    m.text_x   = PAD_X;
    m.header_y = MENUBAR_H + PAD_Y;
    m.text_y   = m.header_y + m.row_h + ROW_GAP;
    m.status_y = m.height - STATUS_H;

    // As many bytes per row as fit, halving down from BPR_MAX
    int32_t cols = (m.width - 2 * PAD_X) / m.cw;
    m.bpr = BPR_MAX;
    while (m.bpr > BPR_MIN && (int32_t)(ascii_start(m.bpr) + m.bpr) > cols)
        m.bpr /= 2;
    m.ascii_x = ascii_start(m.bpr);

    int32_t rows = (m.status_y - m.text_y) / m.row_h;
    m.rows = (rows < 1) ? 1 : (uint32_t)rows;
    return m;
}

static uint32_t total_rows(const metrics_t* m) {
    return doc.len / m->bpr + 1;
}

// Keep the caret on screen, and no empty screen below the end of the data
static void reveal_cursor(void) {
    metrics_t m = get_metrics();
    uint32_t total = total_rows(&m);
    uint32_t max_scroll = (total > m.rows) ? total - m.rows : 0;
    if (scroll_row > max_scroll) scroll_row = max_scroll;

    uint32_t row = doc.cursor / m.bpr;
    if (row < scroll_row)
        scroll_row = row;
    else if (row >= scroll_row + m.rows)
        scroll_row = row - m.rows + 1;
}

static void changed(void) {
    reveal_cursor();
    main_dirty = true;
}

//
// Editing
//

static void move_cursor(uint32_t index) {
    doc.cursor = (index > doc.len) ? doc.len : index;
    doc.nibble = 0;
}

// Room for `extra` more bytes
static bool ensure_capacity(uint32_t extra) {
    if (extra > UINT32_MAX - doc.len)
        return false;
    uint32_t need = doc.len + extra;
    if (doc.buf != NULL && need <= doc.cap)
        return true;

    uint32_t cap = doc.cap ? doc.cap : 2048;
    if (cap <= 0x7FFFFFFFu) cap *= 2;
    if (cap < need) cap = (need <= UINT32_MAX - 4096u) ? need + 4096u : need;

    uint8_t* p = (uint8_t*)realloc(doc.buf, cap);
    if (p == NULL)
        return false;
    doc.buf = p;
    doc.cap = cap;
    return true;
}

static bool insert_byte(uint32_t at, uint8_t value) {
    if (!ensure_capacity(1)) {
        notify_error(APP_NAME, "Not enough memory");
        return false;
    }
    memmove(doc.buf + at + 1, doc.buf + at, doc.len - at);
    doc.buf[at] = value;
    doc.len++;
    doc.modified = true;
    return true;
}

static void delete_byte(uint32_t at) {
    if (at >= doc.len)
        return;
    memmove(doc.buf + at, doc.buf + at + 1, doc.len - at - 1);
    doc.len--;
    doc.modified = true;
    move_cursor(at);
}

// The byte under the caret, made first when the caret is on the append cell
static uint8_t* byte_at_cursor(void) {
    if (doc.cursor == doc.len && !insert_byte(doc.len, 0))
        return NULL;
    return doc.buf + doc.cursor;
}

static void type_hex_digit(int value) {
    uint8_t* b = byte_at_cursor();
    if (b == NULL)
        return;
    if (doc.nibble == 0) {
        *b = (uint8_t)((*b & 0x0F) | (value << 4));
        doc.nibble = 1;
    } else {
        *b = (uint8_t)((*b & 0xF0) | value);
        move_cursor(doc.cursor + 1);
    }
    doc.modified = true;
}

static void type_char(uint8_t ch) {
    uint8_t* b = byte_at_cursor();
    if (b == NULL)
        return;
    *b = ch;
    doc.modified = true;
    move_cursor(doc.cursor + 1);
}

//
// Files
//

// The whole file, in a buffer with some room to grow. 0, or -1 with errno.
static int read_file(const char* path, uint8_t** out, uint32_t* out_len, uint32_t* out_cap) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return -1;

    uint8_t* buf = NULL;
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
    buf = (uint8_t*)malloc(cap);
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
static int write_file(const char* path, const uint8_t* data, uint32_t length) {
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

static void reset_view(void) {
    scroll_row = 0;
    pane = PANE_HEX;
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

    uint8_t* buf = NULL;
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

    free(doc.buf);
    memset(&doc, 0, sizeof(doc));
    doc.buf = buf;
    doc.len = len;
    doc.cap = cap;
    strlcpy(doc.path, abs, sizeof(doc.path));
    reset_view();
    return true;
}

static bool save_document(void) {
    if (write_file(doc.path, doc.buf, doc.len) < 0) {
        char title[WM_NOTIFY_TITLE_MAX];
        snprintf(title, sizeof(title), "Can't save %s", doc_name());
        notify_error(title, strerror(errno));
        return false;
    }
    doc.modified = false;
    main_dirty = true;
    wm_notify("Saved", doc_name());
    return true;
}

static void run_then(then_t then) {
    switch (then) {
        case THEN_NEW:  new_document();                          break;
        case THEN_OPEN: file_dialog(FILEDLG_OPEN, THEN_NOTHING); break;
        case THEN_EXIT: running = false;                         break;
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

static char glyph(uint8_t b) {
    return (b < 32 || b > 126) ? '.' : (char)b;
}

static int32_t cell_x(const metrics_t* m, uint32_t cell) {
    return m->text_x + (int32_t)cell * m->cw;
}

static void draw_header(gfx_surface_t* s, const metrics_t* m) {
    char line[96];
    uint32_t n = 0;
    memset(line, ' ', sizeof(line));
    memcpy(line, "Offset", 6);
    for (uint32_t c = 0; c < m->bpr; c++) {
        static const char digits[] = "0123456789ABCDEF";
        n = hex_cell(c);
        line[n]     = '0';
        line[n + 1] = digits[c & 0x0F];
    }
    memcpy(line + m->ascii_x, "ASCII", 5);
    n = m->ascii_x + 5;
    gfx_draw_text_n(s, m->text_x, m->header_y, line, n, theme.client.text_soft);

    int32_t y = m->text_y - ROW_GAP / 2 - ROW_GAP / 2 - 1;
    gfx_draw_hline(s, m->text_x, y, cell_x(m, m->ascii_x + m->bpr) - m->text_x, theme.edit.border);
}

static void draw_data(gfx_surface_t* s, const metrics_t* m) {
    static const char digits[] = "0123456789ABCDEF";
    char line[96];

    gfx_rect_t area = gfx_rect(0, m->text_y - ROW_GAP / 2, m->width, m->status_y - (m->text_y - ROW_GAP / 2));
    gfx_save(s);
    gfx_set_clip(s, &area);

    uint32_t total = total_rows(m);
    for (uint32_t vis = 0; vis <= m->rows; vis++) {     // + a partly shown row
        uint32_t row = scroll_row + vis;
        if (row >= total)
            break;
        int32_t y = m->text_y + (int32_t)vis * m->row_h;
        uint32_t first = row * m->bpr;

        // The caret's byte, highlighted in both columns
        if (doc.cursor >= first && doc.cursor < first + m->bpr) {
            uint32_t c = doc.cursor - first;
            int32_t hy = y - ROW_GAP / 2;
            gfx_fill_rect(s, cell_x(m, hex_cell(c)), hy, 2 * m->cw, m->row_h, theme.edit.selection);
            gfx_fill_rect(s, cell_x(m, m->ascii_x + c), hy, m->cw, m->row_h, theme.edit.selection);

            if (main_focused) {
                // Underline the digit or character that typing changes
                uint32_t at = (pane == PANE_HEX) ? hex_cell(c) + doc.nibble : m->ascii_x + c;
                int32_t uy = y + gfx_line_height(s) + 1;
                gfx_fill_rect(s, cell_x(m, at), uy, m->cw, 2, theme.edit.cursor);
            }
        }

        memset(line, ' ', sizeof(line));
        uint32_t off = first;
        for (int k = 7; k >= 0; k--) {
            line[k] = digits[off & 0x0F];
            off >>= 4;
        }
        uint32_t n = OFFSET_CELLS;
        for (uint32_t c = 0; c < m->bpr && first + c < doc.len; c++) {
            uint8_t b = doc.buf[first + c];
            uint32_t h = hex_cell(c);
            line[h]     = digits[b >> 4];
            line[h + 1] = digits[b & 0x0F];
            line[m->ascii_x + c] = glyph(b);
            n = m->ascii_x + c + 1;
        }
        gfx_draw_text_n(s, m->text_x, y, line, OFFSET_CELLS, theme.client.text_soft);
        if (n > OFFSET_CELLS)
            gfx_draw_text_n(s, cell_x(m, HEX_X), y, line + HEX_X, n - HEX_X, theme.edit.text);
    }
    gfx_restore(s);
}

static void draw_status(gfx_surface_t* s, const metrics_t* m) {
    gfx_fill_rect(s, 0, m->status_y, m->width, STATUS_H, theme.button.fill);
    gfx_draw_hline(s, 0, m->status_y, m->width, theme.button.border);

    char left[80], right[32];
    const char* where = (pane == PANE_HEX) ? "Hex" : "ASCII";
    if (doc.cursor < doc.len) {
        uint8_t b = doc.buf[doc.cursor];
        snprintf(left, sizeof(left), "%s   Offset %08X (%u)   Value %02X (%u)",
                 where, (unsigned)doc.cursor, (unsigned)doc.cursor, b, b);
    } else {
        snprintf(left, sizeof(left), "%s   Offset %08X (%u)   End of file",
                 where, (unsigned)doc.cursor, (unsigned)doc.cursor);
    }
    snprintf(right, sizeof(right), "%u %s", (unsigned)doc.len, doc.len == 1 ? "byte" : "bytes");

    int32_t y = m->status_y + (STATUS_H - gfx_line_height(s)) / 2 + 1;
    gfx_draw_text(s, PAD_X, y, left, theme.button.text);
    gfx_draw_text(s, m->width - PAD_X - gfx_text_width(s, right), y, right, theme.button.text);
}

static void draw_main(void) {
    main_dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics();

    gfx_clear(s, theme.edit.background);

    gfx_fill_rect(s, 0, 0, m.width, MENUBAR_H, theme.button.fill);
    gfx_draw_hline(s, 0, MENUBAR_H - 1, m.width, theme.button.border);
    int32_t y = (MENUBAR_H - gfx_line_height(s)) / 2;
    gfx_draw_text(s, MENU_FILE_X + 4, y, "File", theme.button.text);
    gfx_draw_text(s, MENU_EDIT_X + 4, y, "Edit", theme.button.text);

    draw_header(s, &m);
    draw_data(s, &m);
    draw_status(s, &m);

    wm_window_commit(main_win, NULL);
    update_title();
}

//
// Menus and commands
//

static void show_file_menu(void) {
    wm_menu_item_t items[] = {
        { "New         Ctrl+N",       CMD_NEW,     0 },
        { "Open...     Ctrl+O",       CMD_OPEN,    0 },
        { "Save        Ctrl+S",       CMD_SAVE,    0 },
        { "Save as...  Ctrl+Shift+S", CMD_SAVE_AS, 0 },
        { NULL,                       0,           WM_MENU_SEPARATOR },
        { "Exit",                     CMD_EXIT,    0 },
    };
    wm_menu_show(main_win, MENU_FILE_X, MENUBAR_H, items, sizeof(items) / sizeof(items[0]));
}

static void show_edit_menu(int32_t x, int32_t y) {
    wm_menu_item_t items[] = {
        { "Insert byte   Ctrl+B", CMD_INSERT,      0 },
        { "Delete byte   Del",    CMD_DELETE,      doc.cursor < doc.len ? 0 : WM_MENU_DISABLED },
        { NULL,                   0,               WM_MENU_SEPARATOR },
        { pane == PANE_HEX ? "Edit as ASCII Tab" : "Edit as hex   Tab",
                                  CMD_SWITCH_PANE, 0 },
    };
    wm_menu_show(main_win, x, y, items, sizeof(items) / sizeof(items[0]));
}

static void command(uint32_t id) {
    switch (id) {
        case CMD_NEW:         ask_then(THEN_NEW);                       break;
        case CMD_OPEN:        ask_then(THEN_OPEN);                      break;
        case CMD_SAVE:        save_then(THEN_NOTHING);                  break;
        case CMD_SAVE_AS:     file_dialog(FILEDLG_SAVE, THEN_NOTHING);  break;
        case CMD_EXIT:        ask_then(THEN_EXIT);                      break;
        case CMD_INSERT:      if (insert_byte(doc.cursor, 0)) doc.nibble = 0; break;
        case CMD_DELETE:      delete_byte(doc.cursor);                  break;
        case CMD_SWITCH_PANE:
            pane = (pane == PANE_HEX) ? PANE_ASCII : PANE_HEX;
            doc.nibble = 0;
            break;
        default:                                                        return;
    }
    changed();
}

//
// Input: main window
//

static void main_navigate(uint16_t key, bool ctrl) {
    metrics_t m = get_metrics();
    uint32_t bpr = m.bpr;
    uint32_t col = doc.cursor % bpr;
    uint32_t page = m.rows * bpr;

    switch (key) {
        case KEY_LEFT:      // Hex column: a digit at a time
            if (pane == PANE_HEX && doc.nibble == 1)
                doc.nibble = 0;
            else if (doc.cursor > 0) {
                move_cursor(doc.cursor - 1);
                if (pane == PANE_HEX) doc.nibble = 1;
            }
            break;

        case KEY_RIGHT:
            if (pane == PANE_HEX && doc.nibble == 0 && doc.cursor < doc.len)
                doc.nibble = 1;
            else if (doc.cursor < doc.len)
                move_cursor(doc.cursor + 1);
            break;

        case KEY_UP:
            if (doc.cursor >= bpr) move_cursor(doc.cursor - bpr);
            break;

        case KEY_DOWN:      // Last row: the end of the data
            move_cursor((doc.len - doc.cursor > bpr) ? doc.cursor + bpr : doc.len);
            break;

        case KEY_PAGE_UP:
            scroll_row = (scroll_row > m.rows) ? scroll_row - m.rows : 0;
            move_cursor(doc.cursor > page ? doc.cursor - page : col);
            break;

        case KEY_PAGE_DOWN:
            scroll_row += m.rows;                           // reveal_cursor() clamps it
            move_cursor((doc.len - doc.cursor > page) ? doc.cursor + page : doc.len);
            break;

        case KEY_HOME:      // Ctrl+Home: start of the file
            move_cursor(ctrl ? 0 : doc.cursor - col);
            break;

        case KEY_END:       // Ctrl+End: end of the file
            if (ctrl)
                move_cursor(doc.len);
            else
                move_cursor(doc.cursor - col + bpr - 1);    // Clamped to len
            break;
    }
    changed();
}

static void main_key(const wm_event_t* ev) {
    uint16_t key = ev->key;
    bool shift = (ev->mods & INPUT_MOD_SHIFT) != 0;
    bool ctrl  = (ev->mods & INPUT_MOD_CTRL) != 0;

    bool nav = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
               key == KEY_HOME || key == KEY_END || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN;
    if (nav) {
        main_navigate(key, ctrl);
        return;
    }

    if (ctrl) {
        // The letter, whether the dwm sends 'c', 'C' or the control code 3
        char letter = 0;
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))
            letter = (char)(key | 0x20);
        else if (key >= 3 && key <= 26 && key != KEY_TAB)
            letter = (char)('a' + key - 1);

        switch (letter) {
            case 'b': command(CMD_INSERT);                     break;
            case 's': command(shift ? CMD_SAVE_AS : CMD_SAVE); break;
            case 'o': command(CMD_OPEN);                       break;
            case 'n': command(CMD_NEW);                        break;
            default:                                           break;   // Swallow the rest
        }
        return;
    }

    switch (key) {
        case KEY_TAB:
            command(CMD_SWITCH_PANE);
            return;

        case KEY_DELETE:
            delete_byte(doc.cursor);
            break;

        case KEY_BACKSPACE:
            if (doc.cursor > 0)
                delete_byte(doc.cursor - 1);
            break;

        case KEY_ESCAPE:
        case KEY_ENTER:
            return;

        default:
            if (pane == PANE_HEX) {
                int v = hex_value(key);
                if (v < 0)
                    return;
                type_hex_digit(v);
            } else {
                if (key < 32 || key > 126)
                    return;                 // A special key with no character
                type_char((uint8_t)key);
            }
            break;
    }
    changed();
}

static void main_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;

    metrics_t m = get_metrics();
    if (ev->y < MENUBAR_H) {
        if (ev->x >= MENU_FILE_X && ev->x < MENU_FILE_X + MENU_ITEM_W)
            show_file_menu();
        else if (ev->x >= MENU_EDIT_X && ev->x < MENU_EDIT_X + MENU_ITEM_W)
            show_edit_menu(MENU_EDIT_X, MENUBAR_H);
        return;
    }
    int32_t rel = ev->y - (m.text_y - ROW_GAP / 2);
    if (rel < 0 || ev->y >= m.status_y || ev->x < m.text_x)
        return;

    uint32_t row = scroll_row + (uint32_t)(rel / m.row_h);
    uint32_t cell = (uint32_t)((ev->x - m.text_x) / m.cw);

    // Which byte, and in which column, the click landed on
    for (uint32_t c = 0; c < m.bpr; c++) {
        uint32_t h = hex_cell(c);
        bool in_hex = cell >= h && cell < h + 2;
        bool in_ascii = cell == m.ascii_x + c;
        if (!in_hex && !in_ascii)
            continue;
        uint32_t index = row * m.bpr + c;
        if (index > doc.len)
            return;
        move_cursor(index);
        pane = in_hex ? PANE_HEX : PANE_ASCII;
        if (in_hex && index < doc.len)
            doc.nibble = cell - h;
        changed();
        return;
    }
}

// Wheel: scrolls the rows without moving the caret, down to the last row
static void main_wheel(const wm_event_t* ev) {
    if (ev->wheel == 0)
        return;
    metrics_t m = get_metrics();
    uint32_t total = total_rows(&m);
    int64_t max_scroll = (total > m.rows) ? (int64_t)(total - m.rows) : 0;
    int64_t step = (ev->mods & INPUT_MOD_CTRL) ? (int64_t)m.rows : WHEEL_ROWS;
    int64_t row = (int64_t)scroll_row + ev->wheel * step;
    if (row > max_scroll) row = max_scroll;
    if (row < 0) row = 0;
    if ((uint32_t)row != scroll_row) {
        scroll_row = (uint32_t)row;
        main_dirty = true;
    }
}

static void main_event(const wm_event_t* ev) {
    // A dialog is up: it gets the user's attention instead
    wm_window_t up = (dlg.win != 0) ? dlg.win : filedlg_window(fdlg);
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
        case WM_EVENT_MOUSE_WHEEL:  main_wheel(ev);                break;
        case WM_EVENT_CONTEXT_MENU: show_edit_menu(ev->x, ev->y);  break;
        case WM_EVENT_MENU_COMMAND: command(ev->param);            break;
        case WM_EVENT_CLOSE:        ask_then(THEN_EXIT);           break;

        case WM_EVENT_FOCUS_GAINED:
        case WM_EVENT_FOCUS_LOST:
            main_focused = (ev->type == WM_EVENT_FOCUS_GAINED);
            main_dirty = true;
            break;

        case WM_EVENT_RESIZED:
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

static void draw_button(gfx_surface_t* s, const gfx_rect_t* r, const char* label, bool focused, bool pressed) {
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

// Main
//

int main(int argc, char** argv) {
    load_theme();

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
        fprintf(stderr, "hexedit: can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    strlcpy(shown_title, title, sizeof(shown_title));
    draw_main();
    wm_window_focus(main_win);

    // Events are handled as they come; the windows are repainted once the
    // queue runs dry, so a burst of keys costs one frame
    wm_event_t ev;
    while (running) {
        bool dirty = main_dirty || (dlg.win != 0 && dlg.dirty) || filedlg_dirty(fdlg);
        int r = wm_wait_event(&ev, dirty ? 0 : -1);
        if (r < 0)
            break;                          // The dwm went away
        if (r == 0) {
            if (main_dirty) draw_main();
            if (dlg.win != 0 && dlg.dirty) draw_dialog();
            filedlg_paint(fdlg);
            continue;
        }

        if (file_dialog_event(&ev))
            continue;                       // Its window 0 events still come through
        if (ev.type == WM_EVENT_THEME_CHANGED) {
            load_theme();
            main_dirty = true;
            dlg.dirty = true;
        } else if (ev.window == main_win) {
            main_event(&ev);
        } else if (dlg.win != 0 && ev.window == dlg.win) {
            dialog_event(&ev);
        }
    }

    wm_disconnect();
    free(doc.buf);
    return 0;
}
