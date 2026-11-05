/*
 * charmap: the character map, for the desktop.
 *
 *     charmap
 *
 * All 256 characters of the system's 8-bit character set in a 32 x 8 grid:
 * the first 128 are ASCII, the rest Latin-1, the way gfx text draws bytes.
 * Pick a character, add it to "Characters to copy" and copy the lot to the
 * clipboard, ready to paste into notepad or any other program.
 *
 * Mouse: click a cell to select it; hold the button to see it magnified
 * (drag to look along the grid); double click adds it to the copy field.
 * Right click a cell for its copy menu.
 *
 * Keys in the grid: arrows, Home / End (row), Ctrl+Home / Ctrl+End,
 * Page Up / Page Down (column); typing a character jumps to it; Enter adds
 * the selected one. Tab moves between the grid and the copy field. Ctrl+C
 * copies the field (or the selected character when the field is empty),
 * Ctrl+V pastes text into the field.
 *
 * Font: the desktop's font (the one gfx text uses), the kernel's built-in
 * one, or any font file in the system fonts folder. As in gfx text, control
 * characters and characters the font lacks show the built-in glyph.
 *
 * Built on os/wm.h, the same way as notepad and hexedit. Needs SEC_GFX,
 * SEC_IPC and SEC_FILE_READ (the default clearance has them all).
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <os/os.h>
#include <os/font.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

#ifndef APP_NAME
#define APP_NAME        "charmap"
#endif
#define WIN_TITLE       "Character Map"

// The grid
#define COLS            32
#define ROWS            8
#define CELL            22          // Pixels, grid line included
#define GRID_W          (COLS * CELL + 1)
#define GRID_H          (ROWS * CELL + 1)
#define GRID_PX         16          // TrueType glyph height in a cell

// The magnifier shown while the mouse button is held on a cell
#define MAG_BOX         72
#define MAG_PX          56
#define MAG_SCALE_MAX   6           // Built-in glyphs: whole multiples only

// Window
#define PAD             8
#define GAP             8
#define FONT_BOX_W      280
#define BTN_W           64
#define BTN_GAP         6
#define FIELD_MAX       256         // Bytes in "Characters to copy"

// Fonts in the system fonts folder
#define FONT_LIST_MAX   64
#define FONTS_PER_PAGE  6           // Leaves room for the other menu items

enum {
    CMD_ADD = 1,
    CMD_COPY_CHAR,
    CMD_COPY_UCODE,
    CMD_COPY_HEX,
    CMD_COPY_FIELD,
    CMD_PASTE,
    CMD_CLEAR,
    CMD_FONT_DESKTOP,
    CMD_FONT_BUILTIN,
    CMD_FONT_MORE,
    CMD_FONT_FILE = 100,            // + index in fonts.names
};

typedef enum { FONT_DESKTOP, FONT_BUILTIN, FONT_FILE } font_kind_t;
typedef enum { FOCUS_GRID, FOCUS_FIELD } focus_t;
typedef enum { BTN_SELECT, BTN_COPY, BTN_CLEAR, BTN_COUNT } button_t;

typedef struct {
    int32_t    width, height;       // Client size
    int32_t    lh, cw;              // UI font cell
    int32_t    ctl_h;               // Height of the font box, field and buttons
    gfx_rect_t font_box;
    gfx_rect_t grid;
    int32_t    copy_y;              // Top of the "Characters to copy" row
    gfx_rect_t field;
    gfx_rect_t buttons[BTN_COUNT];
    int32_t    status_y, status_h;
} metrics_t;

static wm_window_t  main_win;
static wm_theme_t   theme;
static bool         running = true;
static bool         dirty;
static bool         focused = true;
static focus_t      focus = FOCUS_GRID;
static uint8_t      selected = 'A';
static bool         magnify;        // Left button held on the grid
static int          pressed = -1;   // Button under a held mouse button
static bool         pressed_inside;
static char         status_msg[64]; // Shown instead of the character's name until the next move

static struct {
    char     text[FIELD_MAX + 1];
    uint32_t len;
    uint32_t cursor;
    uint32_t scroll;
} field;

// The grid's font. A TrueType font is our own copy (the desktop's is
// copied too), so the faces made from it live as long as we want them.
static struct {
    font_kind_t  kind;
    font_t*      ttf;
    font_face_t* grid_face;
    font_face_t* mag_face;
    char         file[NAME_MAX + 1];    // FONT_FILE: its file name
    char         family[FONT_FAMILY_MAX];
} grid_font;

// The fonts folder, as last listed
static struct {
    char   dir[PATH_MAX];
    char   names[FONT_LIST_MAX][NAME_MAX + 1];
    size_t count;
    size_t page;
} fonts;

// The kernel's built-in glyphs, read back once: 1 = ink
static struct {
    int32_t  w, h;
    uint8_t* ink;                   // 256 * w * h
    bool     any[256];              // The glyph has some ink
} builtin;

static uint32_t* layer;             // Built-in glyphs of the grid, drawn in one go
static uint32_t* mag_pixels;        // A magnified built-in glyph

//
// Character names (Unicode's, for the Latin-1 code points)
//

static const char* const c0_names[32] = {
    "Null", "Start Of Heading", "Start Of Text", "End Of Text",
    "End Of Transmission", "Enquiry", "Acknowledge", "Bell",
    "Backspace", "Character Tabulation", "Line Feed", "Line Tabulation",
    "Form Feed", "Carriage Return", "Shift Out", "Shift In",
    "Data Link Escape", "Device Control One", "Device Control Two", "Device Control Three",
    "Device Control Four", "Negative Acknowledge", "Synchronous Idle", "End Of Transmission Block",
    "Cancel", "End Of Medium", "Substitute", "Escape",
    "Information Separator Four", "Information Separator Three",
    "Information Separator Two", "Information Separator One",
};

static const char* const c1_names[32] = {
    "Padding Character", "High Octet Preset", "Break Permitted Here", "No Break Here",
    "Index", "Next Line", "Start Of Selected Area", "End Of Selected Area",
    "Character Tabulation Set", "Character Tabulation With Justification",
    "Line Tabulation Set", "Partial Line Forward",
    "Partial Line Backward", "Reverse Line Feed", "Single Shift Two", "Single Shift Three",
    "Device Control String", "Private Use One", "Private Use Two", "Set Transmit State",
    "Cancel Character", "Message Waiting", "Start Of Guarded Area", "End Of Guarded Area",
    "Start Of String", "Single Graphic Character Introducer", "Single Character Introducer",
    "Control Sequence Introducer",
    "String Terminator", "Operating System Command", "Privacy Message", "Application Program Command",
};

// 0x20 .. 0x7F; NULL where name_of() makes the name (digits, letters)
static const char* const ascii_names[96] = {
    "Space", "Exclamation Mark", "Quotation Mark", "Number Sign",
    "Dollar Sign", "Percent Sign", "Ampersand", "Apostrophe",
    "Left Parenthesis", "Right Parenthesis", "Asterisk", "Plus Sign",
    "Comma", "Hyphen-Minus", "Full Stop", "Solidus",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "Colon", "Semicolon", "Less-Than Sign", "Equals Sign", "Greater-Than Sign", "Question Mark",
    "Commercial At",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "Left Square Bracket", "Reverse Solidus", "Right Square Bracket", "Circumflex Accent",
    "Low Line", "Grave Accent",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "Left Curly Bracket", "Vertical Line", "Right Curly Bracket", "Tilde", "Delete",
};

// 0xA0 .. 0xBF
static const char* const latin1_symbols[32] = {
    "No-Break Space", "Inverted Exclamation Mark", "Cent Sign", "Pound Sign",
    "Currency Sign", "Yen Sign", "Broken Bar", "Section Sign",
    "Diaeresis", "Copyright Sign", "Feminine Ordinal Indicator",
    "Left-Pointing Double Angle Quotation Mark",
    "Not Sign", "Soft Hyphen", "Registered Sign", "Macron",
    "Degree Sign", "Plus-Minus Sign", "Superscript Two", "Superscript Three",
    "Acute Accent", "Micro Sign", "Pilcrow Sign", "Middle Dot",
    "Cedilla", "Superscript One", "Masculine Ordinal Indicator",
    "Right-Pointing Double Angle Quotation Mark",
    "Vulgar Fraction One Quarter", "Vulgar Fraction One Half",
    "Vulgar Fraction Three Quarters", "Inverted Question Mark",
};

// 0xC0 .. 0xDF and 0xE0 .. 0xFF: after "Latin Capital / Small Letter"
static const char* const latin1_letters[32] = {
    "A With Grave", "A With Acute", "A With Circumflex", "A With Tilde",
    "A With Diaeresis", "A With Ring Above", "Ae", "C With Cedilla",
    "E With Grave", "E With Acute", "E With Circumflex", "E With Diaeresis",
    "I With Grave", "I With Acute", "I With Circumflex", "I With Diaeresis",
    "Eth", "N With Tilde", "O With Grave", "O With Acute",
    "O With Circumflex", "O With Tilde", "O With Diaeresis", NULL,
    "O With Stroke", "U With Grave", "U With Acute", "U With Circumflex",
    "U With Diaeresis", "Y With Acute", "Thorn", NULL,
};

static const char* const digit_names[10] = {
    "Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine",
};

static bool is_control(uint8_t c) {
    return c < 0x20 || (c >= 0x7F && c < 0xA0);
}

static const char* name_of(uint8_t c, char* buf, size_t size) {
    if (c < 0x20)
        return c0_names[c];
    if (c >= 0x80 && c < 0xA0)
        return c1_names[c - 0x80];
    if (c >= '0' && c <= '9') {
        snprintf(buf, size, "Digit %s", digit_names[c - '0']);
        return buf;
    }
    if (c >= 'A' && c <= 'Z') {
        snprintf(buf, size, "Latin Capital Letter %c", c);
        return buf;
    }
    if (c >= 'a' && c <= 'z') {
        snprintf(buf, size, "Latin Small Letter %c", c - 'a' + 'A');
        return buf;
    }
    if (c < 0x80)
        return ascii_names[c - 0x20];
    if (c < 0xC0)
        return latin1_symbols[c - 0xA0];
    if (c == 0xD7) return "Multiplication Sign";
    if (c == 0xDF) return "Latin Small Letter Sharp S";
    if (c == 0xF7) return "Division Sign";
    if (c == 0xFF) return "Latin Small Letter Y With Diaeresis";
    snprintf(buf, size, "Latin %s Letter %s", c < 0xE0 ? "Capital" : "Small",
             latin1_letters[(c - 0xC0) & 0x1F]);
    return buf;
}

//
// Small helpers
//

static void notify_error(const char* title, const char* message) {
    wm_notify_ex(WM_NOTIFY_ERROR, title, message, WM_NOTIFY_DEFAULT_MS * 2);
}

static bool point_in(const gfx_rect_t* r, int32_t x, int32_t y) {
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

static void set_status(const char* msg) {
    strlcpy(status_msg, msg, sizeof(status_msg));
    dirty = true;
}

//
// Built-in glyphs
//
// gfx text with a TrueType font set draws through it, so the kernel's own
// glyphs are drawn once with the font unset, read back, and kept as ink
// masks. Any size and color is then one gfx_draw_pixels_keyed() call.
//

static void load_builtin(void) {
    gfx_info_t info;
    if (gfx_get_info(&info) < 0 || info.char_width == 0 || info.line_height == 0) {
        info.char_width = 6;
        info.line_height = 8;
    }
    builtin.w = (int32_t)info.char_width;
    builtin.h = (int32_t)info.line_height;
    int32_t sw = 16 * builtin.w, sh = 16 * builtin.h;

    builtin.ink = (uint8_t*)calloc(256, (size_t)(builtin.w * builtin.h));
    uint32_t* px = (uint32_t*)malloc((size_t)(sw * sh) * sizeof(uint32_t));
    gfx_surface_t* s = gfx_surface_create(sw, sh);
    if (builtin.ink == NULL || px == NULL || s == NULL)
        goto done;

    // The desktop's font is set back as it was, so the wm keeps managing it
    int32_t px_size = 0;
    font_t* font = gfx_get_font(&px_size);
    gfx_set_font(NULL, 0);

    gfx_clear(s, GFX_COLOR_BLACK);
    for (int c = 1; c < 256; c++)
        gfx_draw_char(s, (c % 16) * builtin.w, (c / 16) * builtin.h, (unsigned char)c, GFX_COLOR_WHITE);
    int r = gfx_read_pixels(s, 0, 0, sw, sh, px, 0);

    if (font != NULL)
        gfx_set_font(font, px_size);
    if (r < 0)
        goto done;

    for (int c = 0; c < 256; c++) {
        uint8_t* ink = builtin.ink + (size_t)c * (size_t)(builtin.w * builtin.h);
        for (int32_t y = 0; y < builtin.h; y++) {
            for (int32_t x = 0; x < builtin.w; x++) {
                uint32_t p = px[((c / 16) * builtin.h + y) * sw + (c % 16) * builtin.w + x];
                if ((p & 0x00FFFFFFu) != 0) {
                    ink[y * builtin.w + x] = 1;
                    builtin.any[c] = true;
                }
            }
        }
    }

done:
    if (s != NULL)
        gfx_surface_destroy(s);
    free(px);
}

// Largest whole scale at which a built-in glyph fits a box
static int32_t builtin_scale(int32_t box_w, int32_t box_h, int32_t max) {
    int32_t k = max;
    while (k > 1 && (builtin.w * k > box_w || builtin.h * k > box_h))
        k--;
    return k;
}

// Glyph c at scale k into dst (stride in pixels), top-left at (x, y)
static void builtin_paint(uint32_t* dst, int32_t stride, int32_t x, int32_t y,
                          uint8_t c, int32_t k, gfx_color_t color) {
    if (builtin.ink == NULL)
        return;
    const uint8_t* ink = builtin.ink + (size_t)c * (size_t)(builtin.w * builtin.h);
    color |= 0xFF000000u;           // Never the key (0)
    for (int32_t gy = 0; gy < builtin.h; gy++)
        for (int32_t gx = 0; gx < builtin.w; gx++)
            if (ink[gy * builtin.w + gx])
                for (int32_t dy = 0; dy < k; dy++)
                    for (int32_t dx = 0; dx < k; dx++)
                        dst[(y + gy * k + dy) * stride + x + gx * k + dx] = color;
}

//
// The grid's font
//

static void drop_faces(void) {
    font_face_destroy(grid_font.grid_face);
    font_face_destroy(grid_font.mag_face);
    grid_font.grid_face = grid_font.mag_face = NULL;
}

// Make `font` (ours now, or NULL: built-in) the grid's font
static void use_font(font_kind_t kind, font_t* font, const char* file) {
    drop_faces();
    font_close(grid_font.ttf);
    grid_font.ttf = font;
    grid_font.kind = kind;
    grid_font.file[0] = '\0';
    grid_font.family[0] = '\0';
    if (file != NULL)
        strlcpy(grid_font.file, file, sizeof(grid_font.file));
    if (font != NULL) {
        strlcpy(grid_font.family, font_family(font), sizeof(grid_font.family));
        grid_font.grid_face = font_face_create(font, GRID_PX);
        grid_font.mag_face = font_face_create(font, MAG_PX);
        if (grid_font.grid_face == NULL || grid_font.mag_face == NULL) {
            drop_faces();           // Out of memory: built-in glyphs it is
            font_close(grid_font.ttf);
            grid_font.ttf = NULL;
        }
    }
    dirty = true;
}

// A copy of the font gfx text uses (the one the dwm handed out)
static void use_desktop_font(void) {
    size_t size = 0;
    font_t* desk = gfx_get_font(NULL);
    const void* bytes = desk ? font_bytes(desk, &size) : NULL;
    font_t* copy = bytes ? font_open(bytes, size) : NULL;
    use_font(FONT_DESKTOP, copy, NULL);
}

static bool use_font_file(size_t index) {
    if (index >= fonts.count)
        return false;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", fonts.dir, fonts.names[index]);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        notify_error("Can't open font", "Path too long");
        return false;
    }
    font_t* font = font_open_file(path);
    if (font == NULL) {
        notify_error("Can't open font", strerror(errno));
        return false;
    }
    use_font(FONT_FILE, font, fonts.names[index]);
    return true;
}

static bool is_font_file(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot != NULL && dot != name &&
           (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0 || strcasecmp(dot, ".ttc") == 0);
}

static int compare_names(const void* a, const void* b) {
    return strcasecmp((const char*)a, (const char*)b);
}

// List the fonts folder again (fonts may have come or gone)
static void scan_fonts(void) {
    fonts.count = 0;
    if (os_syspath("fonts", fonts.dir, sizeof(fonts.dir)) == NULL)
        return;
    DIR* d = opendir(fonts.dir);
    if (d == NULL)
        return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && fonts.count < FONT_LIST_MAX) {
        if (e->d_type != DT_DIR && is_font_file(e->d_name))
            strlcpy(fonts.names[fonts.count++], e->d_name, sizeof(fonts.names[0]));
    }
    closedir(d);
    qsort(fonts.names, fonts.count, sizeof(fonts.names[0]), compare_names);
    if (fonts.page * FONTS_PER_PAGE >= fonts.count)
        fonts.page = 0;
}

// "name.ttf" -> "name"
static void font_label(const char* file, char* out, size_t size) {
    strlcpy(out, file, size);
    char* dot = strrchr(out, '.');
    if (dot != NULL && dot != out)
        *dot = '\0';
}

static const char* grid_font_name(char* buf, size_t size) {
    switch (grid_font.kind) {
        case FONT_DESKTOP:
            if (grid_font.ttf == NULL)
                return "Desktop font (built-in)";
            snprintf(buf, size, "Desktop font (%s)", grid_font.family[0] ? grid_font.family : "TrueType");
            return buf;
        case FONT_FILE:
            font_label(grid_font.file, buf, size);
            return buf;
        default:
            snprintf(buf, size, "Built-in %d x %d", (int)builtin.w, (int)builtin.h);
            return buf;
    }
}

// Where character c comes from in the grid: true = the built-in glyph.
// The same rule as gfx text: controls and what the font lacks.
static bool shows_builtin(uint8_t c) {
    if (grid_font.ttf == NULL)
        return true;
    return c < 0x20 || c == 0x7F || !font_has_glyph(grid_font.ttf, c);
}

static bool has_glyph(uint8_t c) {
    if (c == ' ' || c == 0xA0)
        return true;
    return shows_builtin(c) ? builtin.any[c] : true;
}

//
// Layout
//

static void measure(gfx_surface_t* s, metrics_t* m) {
    m->lh = s ? gfx_line_height(s) : 8;
    m->cw = s ? gfx_char_width(s) : 6;
    m->ctl_h = m->lh + 8;
    if (m->ctl_h < 20) m->ctl_h = 20;

    int32_t label_w = s ? gfx_text_width(s, "Font:") : 30;
    m->font_box = gfx_rect(PAD + label_w + 6, PAD, FONT_BOX_W, m->ctl_h);
    m->grid = gfx_rect(PAD, PAD + m->ctl_h + GAP, GRID_W, GRID_H);

    m->copy_y = m->grid.y + m->grid.h + GAP;
    int32_t copy_label_w = s ? gfx_text_width(s, "Characters to copy:") : 114;
    int32_t bx = PAD + GRID_W - BTN_COUNT * BTN_W - (BTN_COUNT - 1) * BTN_GAP;
    for (int i = 0; i < BTN_COUNT; i++)
        m->buttons[i] = gfx_rect(bx + i * (BTN_W + BTN_GAP), m->copy_y, BTN_W, m->ctl_h);
    int32_t fx = PAD + copy_label_w + 6;
    m->field = gfx_rect(fx, m->copy_y, bx - BTN_GAP - fx, m->ctl_h);

    m->status_h = m->lh + 6;
    m->status_y = m->copy_y + m->ctl_h + GAP;
    m->width = PAD + GRID_W + PAD;
    m->height = m->status_y + m->status_h;
}

static metrics_t get_metrics(void) {
    metrics_t m;
    measure(wm_window_surface(main_win), &m);
    return m;
}

// The window's size for the current UI font, before there is a window
static void window_size(int32_t* w, int32_t* h) {
    metrics_t m;
    gfx_surface_t* s = gfx_surface_create(8, 8);
    measure(s, &m);
    if (s != NULL)
        gfx_surface_destroy(s);
    *w = m.width;
    *h = m.height;
}

static gfx_rect_t cell_rect(const metrics_t* m, uint8_t c) {
    return gfx_rect(m->grid.x + (c % COLS) * CELL, m->grid.y + (c / COLS) * CELL, CELL + 1, CELL + 1);
}

// The cell under (x, y), clamped to the grid when `clamp`; -1 outside
static int cell_at(const metrics_t* m, int32_t x, int32_t y, bool clamp) {
    int32_t col = (x - m->grid.x) / CELL, row = (y - m->grid.y) / CELL;
    if (x < m->grid.x) col = -1;
    if (y < m->grid.y) row = -1;
    if (clamp) {
        if (col < 0) col = 0;
        if (col >= COLS) col = COLS - 1;
        if (row < 0) row = 0;
        if (row >= ROWS) row = ROWS - 1;
    } else if (col < 0 || col >= COLS || row < 0 || row >= ROWS) {
        return -1;
    }
    return row * COLS + col;
}

//
// The copy field
//

static void field_insert(const char* text, uint32_t length) {
    for (uint32_t i = 0; i < length && field.len < FIELD_MAX; i++) {
        if (text[i] == '\0')
            continue;               // Can't be held in a C string
        memmove(field.text + field.cursor + 1, field.text + field.cursor, field.len - field.cursor + 1);
        field.text[field.cursor++] = text[i];
        field.len++;
    }
    dirty = true;
}

// The byte under the cursor
static void field_delete(void) {
    if (field.cursor < field.len) {
        memmove(field.text + field.cursor, field.text + field.cursor + 1, field.len - field.cursor);
        field.len--;
    }
}

static void field_clear(void) {
    field.len = field.cursor = field.scroll = 0;
    field.text[0] = '\0';
    dirty = true;
}

static void add_selected(void) {
    if (selected == 0) {
        set_status("Null can't be copied as text");
        return;
    }
    if (field.len >= FIELD_MAX) {
        set_status("The copy field is full");
        return;
    }
    char c = (char)selected;
    field_insert(&c, 1);
}

static void copy_text(const char* text, const char* what) {
    if (wm_clip_set_text(text) < 0) {
        notify_error("Can't copy", strerror(errno));
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "Copied %s", what);
    set_status(msg);
}

static void copy_field(void) {
    if (field.len == 0) {
        if (selected == 0) {
            set_status("Null can't be copied as text");
            return;
        }
        char one[2] = { (char)selected, '\0' };
        copy_text(one, "1 character");
        return;
    }
    char what[32];
    snprintf(what, sizeof(what), "%u character%s", (unsigned)field.len, field.len == 1 ? "" : "s");
    copy_text(field.text, what);
}

static void paste_field(void) {
    wm_clip_info_t info;
    char* data = (char*)wm_clip_get(&info);
    if (data == NULL || info.type != WM_CLIP_TEXT) {
        set_status(data == NULL ? "The clipboard is empty" : "The clipboard holds no text");
        free(data);
        return;
    }
    field_insert(data, info.size);
    free(data);
    focus = FOCUS_FIELD;
}

//
// Drawing
//

static void draw_button(gfx_surface_t* s, const gfx_rect_t* r, const char* label, bool focus_ring, bool down) {
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, down ? theme.button.fill_pressed : theme.button.fill);
    gfx_color_t border = down ? theme.button.border_pressed
                       : focus_ring ? theme.button.border_active : theme.button.border;
    gfx_draw_rect(s, r->x, r->y, r->w, r->h, border);
    int32_t tw = gfx_text_width(s, label);
    gfx_draw_text(s, r->x + (r->w - tw) / 2, r->y + (r->h - gfx_line_height(s)) / 2, label, theme.button.text);
}

static void draw_font_box(gfx_surface_t* s, const metrics_t* m) {
    int32_t ty = m->font_box.y + (m->ctl_h - m->lh) / 2;
    gfx_draw_text(s, PAD, ty, "Font:", theme.client.text);

    const gfx_rect_t* r = &m->font_box;
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, theme.edit.background);
    gfx_draw_rect(s, r->x, r->y, r->w, r->h, theme.edit.border);

    // The drop-down arrow, on a button face
    int32_t aw = r->h;
    gfx_rect_t arrow = gfx_rect(r->x + r->w - aw, r->y, aw, r->h);
    gfx_fill_rect(s, arrow.x, arrow.y, arrow.w, arrow.h, theme.button.fill);
    gfx_draw_rect(s, arrow.x, arrow.y, arrow.w, arrow.h, theme.button.border);
    int32_t cx = arrow.x + arrow.w / 2, cy = arrow.y + arrow.h / 2 - 2;
    for (int32_t i = 0; i < 4; i++)
        gfx_draw_hline(s, cx - 3 + i, cy + i, 7 - 2 * i, theme.button.text);

    char buf[FONT_FAMILY_MAX + 16];
    const char* name = grid_font_name(buf, sizeof(buf));
    gfx_rect_t inner = gfx_rect(r->x + 2, r->y + 2, r->w - aw - 4, r->h - 4);
    gfx_save(s);
    gfx_set_clip(s, &inner);
    gfx_draw_text(s, r->x + 5, ty, name, theme.edit.text);
    gfx_restore(s);
}

static void draw_grid(gfx_surface_t* s, const metrics_t* m) {
    const gfx_rect_t* g = &m->grid;
    gfx_fill_rect(s, g->x, g->y, g->w, g->h, theme.edit.background);

    // The selected cell: filled, and ringed while the grid has the keyboard
    gfx_rect_t sel = cell_rect(m, selected);
    gfx_fill_rect(s, sel.x, sel.y, sel.w, sel.h, theme.edit.selection);

    for (int32_t c = 0; c <= COLS; c++)
        gfx_draw_vline(s, g->x + c * CELL, g->y, g->h, theme.edit.border);
    for (int32_t r = 0; r <= ROWS; r++)
        gfx_draw_hline(s, g->x, g->y + r * CELL, g->w, theme.edit.border);

    if (focused && focus == FOCUS_GRID) {
        gfx_draw_rect(s, sel.x, sel.y, sel.w, sel.h, theme.button.border_active);
        gfx_draw_rect(s, sel.x + 1, sel.y + 1, sel.w - 2, sel.h - 2, theme.button.border_active);
    }

    // TrueType glyphs: a row at a time
    font_face_t* face = grid_font.grid_face;
    if (face != NULL) {
        int32_t gy = (CELL - font_face_height(face)) / 2 + 1;
        for (int32_t r = 0; r < ROWS; r++) {
            uint32_t cps[COLS];
            for (int32_t c = 0; c < COLS; c++) {
                uint8_t ch = (uint8_t)(r * COLS + c);
                cps[c] = shows_builtin(ch) ? 0 : ch;
            }
            font_draw_cells(face, s, g->x, g->y + r * CELL + gy, cps, COLS, CELL, theme.edit.text);
        }
    }

    // Built-in glyphs: all of them in one layer the size of the grid
    if (layer != NULL && builtin.ink != NULL) {
        int32_t k = builtin_scale(CELL - 4, CELL - 4, 2);
        int32_t gw = builtin.w * k, gh = builtin.h * k;
        bool any = false;
        memset(layer, 0, (size_t)(GRID_W * GRID_H) * sizeof(uint32_t));
        for (int c = 0; c < 256; c++) {
            uint8_t ch = (uint8_t)c;
            if (!shows_builtin(ch) || !builtin.any[c])
                continue;
            int32_t x = (c % COLS) * CELL + 1 + (CELL - 1 - gw) / 2;
            int32_t y = (c / COLS) * CELL + 1 + (CELL - 1 - gh) / 2;
            builtin_paint(layer, GRID_W, x, y, ch, k, theme.edit.text);
            any = true;
        }
        if (any)
            gfx_draw_pixels_keyed(s, layer, GRID_W, GRID_H, 0, g->x, g->y, 0);
    }
}

// The selected character, big, over its cell
static void draw_magnifier(gfx_surface_t* s, const metrics_t* m) {
    gfx_rect_t cell = cell_rect(m, selected);
    int32_t x = cell.x + cell.w / 2 - MAG_BOX / 2;
    int32_t y = cell.y + cell.h / 2 - MAG_BOX / 2;
    if (x < m->grid.x) x = m->grid.x;
    if (x + MAG_BOX > m->grid.x + m->grid.w) x = m->grid.x + m->grid.w - MAG_BOX;
    if (y < m->grid.y) y = m->grid.y;
    if (y + MAG_BOX > m->grid.y + m->grid.h) y = m->grid.y + m->grid.h - MAG_BOX;

    gfx_fill_rect(s, x, y, MAG_BOX, MAG_BOX, theme.edit.background);
    gfx_draw_rect(s, x, y, MAG_BOX, MAG_BOX, theme.edit.text);
    gfx_draw_rect(s, x + 1, y + 1, MAG_BOX - 2, MAG_BOX - 2, theme.edit.text);

    if (!shows_builtin(selected) && grid_font.mag_face != NULL) {
        uint32_t cp = selected;
        int32_t gy = (MAG_BOX - font_face_height(grid_font.mag_face)) / 2;
        font_draw_cells(grid_font.mag_face, s, x, y + gy, &cp, 1, MAG_BOX, theme.edit.text);
    } else if (mag_pixels != NULL && builtin.any[selected]) {
        int32_t k = builtin_scale(MAG_BOX - 12, MAG_BOX - 12, MAG_SCALE_MAX);
        int32_t gw = builtin.w * k, gh = builtin.h * k;
        memset(mag_pixels, 0, (size_t)(gw * gh) * sizeof(uint32_t));
        builtin_paint(mag_pixels, gw, 0, 0, selected, k, theme.edit.text);
        gfx_draw_pixels_keyed(s, mag_pixels, gw, gh, 0, x + (MAG_BOX - gw) / 2, y + (MAG_BOX - gh) / 2, 0);
    }
}

static void draw_copy_row(gfx_surface_t* s, const metrics_t* m) {
    int32_t ty = m->copy_y + (m->ctl_h - m->lh) / 2;
    gfx_draw_text(s, PAD, ty, "Characters to copy:", theme.client.text);

    const gfx_rect_t* f = &m->field;
    bool active = focused && focus == FOCUS_FIELD;
    gfx_fill_rect(s, f->x, f->y, f->w, f->h, theme.edit.background);
    gfx_draw_rect(s, f->x, f->y, f->w, f->h, active ? theme.button.border_active : theme.edit.border);

    uint32_t visible = (uint32_t)((f->w - 8) / m->cw);
    if (visible < 1) visible = 1;
    if (field.cursor < field.scroll) field.scroll = field.cursor;
    if (field.cursor > field.scroll + visible) field.scroll = field.cursor - visible;

    gfx_rect_t inner = gfx_rect(f->x + 2, f->y + 2, f->w - 4, f->h - 4);
    gfx_save(s);
    gfx_set_clip(s, &inner);
    gfx_draw_text_n(s, f->x + 4, ty, field.text + field.scroll, field.len - field.scroll, theme.edit.text);
    if (active)
        gfx_draw_vline(s, f->x + 4 + (int32_t)(field.cursor - field.scroll) * m->cw, ty - 1, m->lh + 2,
                       theme.edit.cursor);
    gfx_restore(s);

    static const char* const labels[BTN_COUNT] = { "Select", "Copy", "Clear" };
    for (int i = 0; i < BTN_COUNT; i++)
        draw_button(s, &m->buttons[i], labels[i], i == BTN_COPY && active,
                    i == pressed && pressed_inside);
}

static void draw_status(gfx_surface_t* s, const metrics_t* m) {
    gfx_fill_rect(s, 0, m->status_y, m->width, m->status_h, theme.button.fill);
    gfx_draw_hline(s, 0, m->status_y, m->width, theme.button.border);

    char left[96], right[48], buf[64];
    if (status_msg[0] != '\0') {
        strlcpy(left, status_msg, sizeof(left));
    } else {
        const char* name = name_of(selected, buf, sizeof(buf));
        snprintf(left, sizeof(left), "U+%04X: %s%s", (unsigned)selected, name,
                 is_control(selected) ? " (control)" : "");
    }
    const char* source = "";
    if (!has_glyph(selected))
        source = "no glyph   ";
    else if (grid_font.ttf != NULL && shows_builtin(selected) && selected != ' ')
        source = "built-in glyph   ";
    snprintf(right, sizeof(right), "%sDec %u   Hex %02X", source, (unsigned)selected, (unsigned)selected);

    int32_t y = m->status_y + (m->status_h - m->lh) / 2 + 1;
    int32_t rw = gfx_text_width(s, right);
    gfx_rect_t clip = gfx_rect(0, m->status_y, m->width - PAD - rw - 12, m->status_h);
    gfx_save(s);
    gfx_set_clip(s, &clip);
    gfx_draw_text(s, PAD, y, left, theme.button.text);
    gfx_restore(s);
    gfx_draw_text(s, m->width - PAD - rw, y, right, theme.button.text);
}

static void draw_main(void) {
    dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics();

    gfx_clear(s, theme.client.background);
    draw_font_box(s, &m);
    draw_grid(s, &m);
    draw_copy_row(s, &m);
    draw_status(s, &m);
    if (magnify)
        draw_magnifier(s, &m);

    wm_window_commit(main_win, NULL);
}

// Fit the window to the UI font (it may have changed with the theme)
static void fit_window(void) {
    int32_t w, h, cur_w, cur_h;
    window_size(&w, &h);
    wm_window_get_size(main_win, &cur_w, &cur_h);
    if (w != cur_w || h != cur_h)
        wm_window_resize(main_win, w, h);
}

//
// Menus and commands
//

static void show_font_menu(void) {
    scan_fonts();
    metrics_t m = get_metrics();

    static char labels[WM_MENU_MAX_ITEMS][WM_MENU_LABEL_MAX];
    wm_menu_item_t items[WM_MENU_MAX_ITEMS];
    size_t n = 0;

    font_t* desk = gfx_get_font(NULL);
    if (desk != NULL)
        snprintf(labels[n], WM_MENU_LABEL_MAX, "Desktop font (%s)", font_family(desk));
    else
        strlcpy(labels[n], "Desktop font (none set)", WM_MENU_LABEL_MAX);
    items[n] = (wm_menu_item_t){ labels[n], CMD_FONT_DESKTOP,
                                 (grid_font.kind == FONT_DESKTOP ? WM_MENU_CHECKED : 0) |
                                 (desk == NULL ? WM_MENU_DISABLED : 0) };
    n++;
    snprintf(labels[n], WM_MENU_LABEL_MAX, "Built-in %d x %d", (int)builtin.w, (int)builtin.h);
    items[n] = (wm_menu_item_t){ labels[n], CMD_FONT_BUILTIN,
                                 grid_font.kind == FONT_BUILTIN ? WM_MENU_CHECKED : 0 };
    n++;

    if (fonts.count > 0) {
        items[n++] = (wm_menu_item_t){ NULL, 0, WM_MENU_SEPARATOR };
        size_t first = fonts.page * FONTS_PER_PAGE;
        for (size_t i = first; i < fonts.count && i < first + FONTS_PER_PAGE; i++) {
            font_label(fonts.names[i], labels[n], WM_MENU_LABEL_MAX);
            bool on = grid_font.kind == FONT_FILE && strcmp(grid_font.file, fonts.names[i]) == 0;
            items[n] = (wm_menu_item_t){ labels[n], CMD_FONT_FILE + (uint32_t)i, on ? WM_MENU_CHECKED : 0 };
            n++;
        }
        if (fonts.count > FONTS_PER_PAGE) {
            size_t pages = (fonts.count + FONTS_PER_PAGE - 1) / FONTS_PER_PAGE;
            snprintf(labels[n], WM_MENU_LABEL_MAX, "More fonts (%u of %u)...",
                     (unsigned)(fonts.page + 1), (unsigned)pages);
            items[n] = (wm_menu_item_t){ labels[n], CMD_FONT_MORE, 0 };
            n++;
        }
    }
    wm_menu_show(main_win, m.font_box.x, m.font_box.y + m.font_box.h, items, n);
}

static void show_grid_menu(int32_t x, int32_t y) {
    char ucode[WM_MENU_LABEL_MAX], hex[WM_MENU_LABEL_MAX];
    snprintf(ucode, sizeof(ucode), "Copy code  U+%04X", (unsigned)selected);
    snprintf(hex, sizeof(hex), "Copy hex   0x%02X", (unsigned)selected);
    uint32_t text_ok = selected != 0 ? 0 : WM_MENU_DISABLED;
    wm_menu_item_t items[] = {
        { "Add to copy field  Enter", CMD_ADD,        text_ok | (field.len < FIELD_MAX ? 0 : WM_MENU_DISABLED) },
        { "Copy character",           CMD_COPY_CHAR,  text_ok },
        { NULL,                       0,              WM_MENU_SEPARATOR },
        { ucode,                      CMD_COPY_UCODE, 0 },
        { hex,                        CMD_COPY_HEX,   0 },
    };
    wm_menu_show(main_win, x, y, items, sizeof(items) / sizeof(items[0]));
}

static void show_field_menu(int32_t x, int32_t y) {
    wm_clip_info_t info;
    bool can_paste = wm_clip_info(&info) == 0 && info.type == WM_CLIP_TEXT;
    wm_menu_item_t items[] = {
        { "Copy   Ctrl+C", CMD_COPY_FIELD, 0 },
        { "Paste  Ctrl+V", CMD_PASTE,      can_paste ? 0 : WM_MENU_DISABLED },
        { NULL,            0,              WM_MENU_SEPARATOR },
        { "Clear",         CMD_CLEAR,      field.len > 0 ? 0 : WM_MENU_DISABLED },
    };
    wm_menu_show(main_win, x, y, items, sizeof(items) / sizeof(items[0]));
}

static void command(uint32_t id) {
    char buf[16];
    switch (id) {
        case CMD_ADD:          add_selected();                     break;
        case CMD_COPY_FIELD:   copy_field();                       break;
        case CMD_PASTE:        paste_field();                      break;
        case CMD_CLEAR:        field_clear();                      break;
        case CMD_COPY_CHAR:
            buf[0] = (char)selected;
            buf[1] = '\0';
            if (selected != 0)
                copy_text(buf, "1 character");
            break;
        case CMD_COPY_UCODE:
            snprintf(buf, sizeof(buf), "U+%04X", (unsigned)selected);
            copy_text(buf, buf);
            break;
        case CMD_COPY_HEX:
            snprintf(buf, sizeof(buf), "0x%02X", (unsigned)selected);
            copy_text(buf, buf);
            break;
        case CMD_FONT_DESKTOP: use_desktop_font();                 break;
        case CMD_FONT_BUILTIN: use_font(FONT_BUILTIN, NULL, NULL); break;
        case CMD_FONT_MORE:
            fonts.page++;
            show_font_menu();       // scan_fonts() wraps the page round
            break;
        default:
            if (id >= CMD_FONT_FILE && id < CMD_FONT_FILE + FONT_LIST_MAX)
                use_font_file(id - CMD_FONT_FILE);
            break;
    }
    dirty = true;
}

//
// Input
//

static void select_char(int c) {
    if (c < 0) c = 0;
    if (c > 255) c = 255;
    if ((uint8_t)c != selected) {
        selected = (uint8_t)c;
        status_msg[0] = '\0';
    }
    dirty = true;
}

static void grid_key(uint16_t key, bool ctrl) {
    int col = selected % COLS, row = selected / COLS;
    switch (key) {
        case KEY_LEFT:      select_char(selected - 1);                       return;
        case KEY_RIGHT:     select_char(selected + 1);                       return;
        case KEY_UP:        if (row > 0) select_char(selected - COLS);       return;
        case KEY_DOWN:      if (row < ROWS - 1) select_char(selected + COLS); return;
        case KEY_HOME:      select_char(ctrl ? 0 : row * COLS);              return;
        case KEY_END:       select_char(ctrl ? 255 : row * COLS + COLS - 1); return;
        case KEY_PAGE_UP:   select_char(col);                                return;
        case KEY_PAGE_DOWN: select_char((ROWS - 1) * COLS + col);            return;
        case KEY_ENTER:     add_selected();                                  return;
        default:
            if (key >= 32 && key <= 255 && key != 127)
                select_char(key);   // Typing a character finds it
            return;
    }
}

static void field_key(uint16_t key) {
    switch (key) {
        case KEY_LEFT:      if (field.cursor > 0) field.cursor--;              break;
        case KEY_RIGHT:     if (field.cursor < field.len) field.cursor++;      break;
        case KEY_HOME:      field.cursor = 0;                                  break;
        case KEY_END:       field.cursor = field.len;                          break;
        case KEY_ENTER:     copy_field();                                      break;
        case KEY_BACKSPACE:
            if (field.cursor > 0) {
                field.cursor--;
                field_delete();
            }
            break;
        case KEY_DELETE:
            field_delete();
            break;
        default:
            if (key >= 32 && key <= 255 && key != 127) {
                char c = (char)key;
                field_insert(&c, 1);
            }
            break;
    }
    dirty = true;
}

static void main_key(const wm_event_t* ev) {
    uint16_t key = ev->key;
    bool ctrl = (ev->mods & INPUT_MOD_CTRL) != 0;

    if (key == KEY_TAB) {
        focus = (focus == FOCUS_GRID) ? FOCUS_FIELD : FOCUS_GRID;
        dirty = true;
        return;
    }

    bool nav = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
               key == KEY_HOME || key == KEY_END || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN;
    if (ctrl && !nav) {
        // The letter, whether the dwm sends 'c', 'C' or the control code 3
        char letter = 0;
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))
            letter = (char)(key | 0x20);
        else if (key >= 3 && key <= 26 && key != KEY_TAB)
            letter = (char)('a' + key - 1);

        switch (letter) {
            case 'c': command(CMD_COPY_FIELD); break;
            case 'v': command(CMD_PASTE);      break;
            default:                           break;   // Swallow the rest
        }
        return;
    }

    if (focus == FOCUS_GRID)
        grid_key(key, ctrl);
    else
        field_key(key);
}

static void button_click(int b) {
    switch (b) {
        case BTN_SELECT: add_selected(); break;
        case BTN_COPY:   copy_field();   break;
        case BTN_CLEAR:  field_clear();  break;
    }
}

static void main_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    metrics_t m = get_metrics();

    if (point_in(&m.font_box, ev->x, ev->y)) {
        show_font_menu();
        return;
    }

    int c = cell_at(&m, ev->x, ev->y, false);
    if (c >= 0) {
        focus = FOCUS_GRID;
        select_char(c);
        if (ev->clicks >= 2)
            add_selected();
        else
            magnify = true;
        return;
    }

    if (point_in(&m.field, ev->x, ev->y)) {
        focus = FOCUS_FIELD;
        int32_t rel = ev->x - (m.field.x + 4) + m.cw / 2;
        uint32_t at = field.scroll + (uint32_t)(rel > 0 ? rel / m.cw : 0);
        field.cursor = at > field.len ? field.len : at;
        dirty = true;
        return;
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        if (point_in(&m.buttons[i], ev->x, ev->y)) {
            pressed = i;
            pressed_inside = true;
            dirty = true;
            return;
        }
    }
}

static void main_mouse_move(const wm_event_t* ev) {
    metrics_t m = get_metrics();
    bool held = (ev->buttons & INPUT_BTN_LEFT) != 0;

    if (magnify) {
        if (!held) {
            magnify = false;        // Let go somewhere we didn't hear about
            dirty = true;
        } else {
            select_char(cell_at(&m, ev->x, ev->y, true));
        }
    }
    if (pressed >= 0) {
        bool inside = held && point_in(&m.buttons[pressed], ev->x, ev->y);
        if (!held)
            pressed = -1;
        if (inside != pressed_inside) {
            pressed_inside = inside;
            dirty = true;
        }
    }
}

static void main_mouse_up(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    if (magnify) {
        magnify = false;
        dirty = true;
    }
    if (pressed >= 0) {
        metrics_t m = get_metrics();
        int b = pressed;
        pressed = -1;
        dirty = true;
        if (point_in(&m.buttons[b], ev->x, ev->y))
            button_click(b);
    }
}

static void main_context_menu(const wm_event_t* ev) {
    metrics_t m = get_metrics();
    int c = cell_at(&m, ev->x, ev->y, false);
    if (c >= 0) {
        focus = FOCUS_GRID;
        select_char(c);
        draw_main();                // Show what the menu is about
        show_grid_menu(ev->x, ev->y);
    } else if (point_in(&m.field, ev->x, ev->y)) {
        focus = FOCUS_FIELD;
        dirty = true;
        show_field_menu(ev->x, ev->y);
    }
}

static void main_event(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_KEY:          main_key(ev);          break;
        case WM_EVENT_MOUSE_DOWN:   main_mouse_down(ev);   break;
        case WM_EVENT_MOUSE_MOVE:   main_mouse_move(ev);   break;
        case WM_EVENT_MOUSE_UP:     main_mouse_up(ev);     break;
        case WM_EVENT_CONTEXT_MENU: main_context_menu(ev); break;
        case WM_EVENT_MENU_COMMAND: command(ev->param);    break;
        case WM_EVENT_CLOSE:        running = false;       break;

        case WM_EVENT_FOCUS_GAINED:
        case WM_EVENT_FOCUS_LOST:
            focused = (ev->type == WM_EVENT_FOCUS_GAINED);
            magnify = false;
            pressed = -1;
            dirty = true;
            break;

        case WM_EVENT_RESIZED:
            dirty = true;
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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    load_theme();
    load_builtin();
    use_desktop_font();
    if (grid_font.ttf == NULL)
        grid_font.kind = FONT_BUILTIN;      // The dwm has no TrueType font

    layer = (uint32_t*)malloc((size_t)(GRID_W * GRID_H) * sizeof(uint32_t));
    int32_t mag_side = builtin.h * MAG_SCALE_MAX;
    if (builtin.w * MAG_SCALE_MAX > mag_side) mag_side = builtin.w * MAG_SCALE_MAX;
    mag_pixels = (uint32_t*)malloc((size_t)(mag_side * mag_side) * sizeof(uint32_t));

    int32_t w, h;
    window_size(&w, &h);

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title  = WIN_TITLE;
    d.x      = WM_POS_DEFAULT;
    d.y      = WM_POS_DEFAULT;
    d.width  = w;
    d.height = h;
    d.style  = WM_STYLE_CONTEXT_MENU;      // Fixed size, like the original

    main_win = wm_window_create(&d);
    if (main_win == 0) {
        fprintf(stderr, APP_NAME ": can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    draw_main();
    wm_window_focus(main_win);

    // Events are handled as they come; the window is repainted once the
    // queue runs dry, so a drag across the grid costs one frame per batch
    wm_event_t ev;
    while (running) {
        int r = wm_wait_event(&ev, dirty ? 0 : -1);
        if (r < 0)
            break;                          // The dwm went away
        if (r == 0) {
            if (dirty) draw_main();
            continue;
        }

        if (ev.type == WM_EVENT_THEME_CHANGED) {
            // The desktop font may have changed with it: copy it again
            load_theme();
            if (grid_font.kind == FONT_DESKTOP)
                use_desktop_font();
            fit_window();
            dirty = true;
        } else if (ev.window == main_win) {
            main_event(&ev);
        }
    }

    wm_disconnect();
    drop_faces();
    font_close(grid_font.ttf);
    free(layer);
    free(mag_pixels);
    free(builtin.ink);
    return 0;
}
