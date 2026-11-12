/*
 * imgedit: an image viewer and editor for the desktop.
 *
 *     imgedit [file]
 *
 * Opens image files (PNG, JPEG, BMP, GIF, TGA, PSD, PIC, PNM: whatever
 * libc's os/image.h decodes), including the PNGs the dwm draws its icons
 * from, shows them zoomed over a checkerboard the way the dwm would draw
 * them, and saves them as PNG. A file that does not exist yet opens as a
 * new 32 x 32 image and is created by the first save.
 *
 * Tools, from the toolbar, a right click, or their key:
 *   Pencil (P)  Eraser (E)  Fill (F)  Line (L)  Rect (R)  Box (B, filled)
 *   Pick (I, takes the color under the pointer)  Pan (H)
 * [ and ] change the brush size of the pencil, eraser, line and rectangle.
 * The middle button pans with any tool, the arrow keys scroll. Escape while
 * drawing takes the stroke back. Ctrl+Z / Ctrl+Y undo and redo, + and -
 * zoom, 0 fits the image to the window, 1 is actual size, G shows the pixel
 * grid, C opens the color picker. Ctrl+C / Ctrl+V copy the image to the
 * clipboard and paste one from it.
 *
 * The color picker (C, Image > Color..., or the color button at the right
 * end of the toolbar) has a grid of 48 basic colors, a hue / saturation
 * field with a brightness strip beside it for any other color, and the hex
 * value, which can be typed in. A color picked there also goes into one of
 * the palette bar's custom slots.
 *
 * Transparency works the way the dwm draws its images: opaque black
 * (0xFF000000, SYS_GFX_SPRITE_KEY) is see-through and other colors are
 * alpha blended. The eraser paints the see-through color; the palette's
 * black is 0x010101 so that it stays visible.
 *
 * Built on os/wm.h like notepad and hexedit: the dwm provides the window,
 * the menus and notifications, libc's file dialog (os/filedlg.h) Open and
 * Save as; the "save changes?" question and the size and color prompts are
 * small windows of this program's own.
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
#include <os/image.h>

#ifndef APP_NAME
#define APP_NAME        "imgedit"
#endif

// Main window
#define WIN_W           640
#define WIN_H           480
#define WIN_MIN_W       420
#define WIN_MIN_H       260

#define MENUBAR_H       16
#define TOOLBAR_H       20
#define PALETTE_H       20
#define STATUS_H        14
#define PAD_X           8

#define TOOL_X          4
#define TOOL_BTN_H      16
#define TOOL_GAP        2
#define COLOR_BTN_W     40
#define SWATCH          14
#define SWATCH_GAP      2
#define FIT_MARGIN      16          // Pixels left around an image fitted to the window

// Canvas
#define CHECKER         8           // Checkerboard square behind see-through pixels
#define CHECK_LIGHT     GFX_RGB(204, 204, 204)
#define CHECK_DARK      GFX_RGB(160, 160, 160)
#define BACKDROP        GFX_RGB(112, 112, 112)
#define GRID_COLOR      0x606060u
#define SEE_THROUGH     SYS_GFX_SPRITE_KEY

// Images
#define IMG_MAX_DIM     2048        // Largest width / height opened or made
#define NEW_W           32
#define NEW_H           32
#define BRUSH_MAX       8
#define UNDO_MAX        32
#define UNDO_BYTES      (24u << 20) // Older steps are dropped past this

// Dialogs
#define DLG_W           340
#define DLG_CONFIRM_H   90
#define DLG_FIELD_H     92
#define DLG_MARGIN      12
#define FIELD_GAP       16          // Between the two fields of a size prompt
#define BTN_W           80
#define BTN_H           20
#define BTN_GAP         8
#define DEFAULT_NAME    "untitled.png" // Save as offers it for a new image

// The color picker: basic colors on the left, the hue / saturation field and
// the brightness strip on the right, the hex field under them
#define PICK_W          388
#define PICK_H          216
#define PICK_TOP        26
#define PICK_COLS       8
#define PICK_ROWS       6
#define PICK_CELL       16
#define PICK_CELL_GAP   4
#define PICK_HS_X       184
#define PICK_HS_W       160
#define PICK_HS_H       116
#define PICK_V_X        352
#define PICK_V_W        16
#define PICK_FIELD_Y    154

enum {
    CMD_NEW = 1,
    CMD_OPEN,
    CMD_SAVE,
    CMD_SAVE_AS,
    CMD_EXIT,
    CMD_UNDO,
    CMD_REDO,
    CMD_COPY,
    CMD_PASTE,
    CMD_COLOR,
    CMD_CANVAS_SIZE,
    CMD_FLIP_H,
    CMD_FLIP_V,
    CMD_ROTATE_RIGHT,
    CMD_ROTATE_LEFT,
    CMD_CLEAR,
    CMD_ZOOM_IN,
    CMD_ZOOM_OUT,
    CMD_ZOOM_ACTUAL,
    CMD_ZOOM_FIT,
    CMD_GRID,
    CMD_TOOL,                       // + the tool
};

typedef enum {
    TOOL_PENCIL,
    TOOL_ERASER,
    TOOL_FILL,
    TOOL_LINE,
    TOOL_RECT,
    TOOL_BOX,
    TOOL_PICK,
    TOOL_PAN,
    TOOL_COUNT
} tool_t;

static const char* const tool_names[TOOL_COUNT] = {
    "Pencil", "Eraser", "Fill", "Line", "Rect", "Box", "Pick", "Pan",
};
static const char tool_keys[TOOL_COUNT] = { 'p', 'e', 'f', 'l', 'r', 'b', 'i', 'h' };

// What to do once the current image is safe (saved or knowingly dropped)
typedef enum { THEN_NOTHING, THEN_NEW, THEN_OPEN, THEN_EXIT } then_t;

typedef enum { DLG_NONE, DLG_CONFIRM, DLG_NEW, DLG_RESIZE, DLG_COLOR } dlg_kind_t;

typedef enum { DRAG_NONE, DRAG_PAINT, DRAG_SHAPE, DRAG_PAN } drag_t;

typedef struct {
    int32_t   w, h;
    uint32_t* px;                   // w * h, 0xAARRGGBB, stride = w
    bool      modified;
    char      path[PATH_MAX];       // Absolute; "" = Untitled
} doc_t;

typedef struct {
    int32_t   w, h;
    uint32_t* px;
} snap_t;

typedef struct {
    int32_t    width, height;       // Client size
    int32_t    cw;                  // Cell width of the font
    gfx_rect_t canvas;              // Where the image is shown
    int32_t    toolbar_y, palette_y, status_y;
} metrics_t;

static const struct {
    const char* label;
    int32_t     x, w;
} menus[] = {
    { "File",   6, 36 },
    { "Edit",  44, 36 },
    { "Image", 82, 42 },
    { "View", 126, 36 },
};
#define MENU_COUNT  ((int)(sizeof(menus) / sizeof(menus[0])))

static const int32_t zoom_levels[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
#define ZOOM_LEVELS ((int)(sizeof(zoom_levels) / sizeof(zoom_levels[0])))

// The palette bar: fixed colors, then a few slots that remember custom ones
#define SWATCH_FIXED    18
#define SWATCH_CUSTOM   6
#define SWATCH_COUNT    (SWATCH_FIXED + SWATCH_CUSTOM)
static uint32_t swatches[SWATCH_COUNT] = {
    SEE_THROUGH,              GFX_RGB(1, 1, 1),         GFX_RGB(255, 255, 255),
    GFX_RGB(64, 64, 64),      GFX_RGB(128, 128, 128),   GFX_RGB(192, 192, 192),
    GFX_RGB(128, 0, 0),       GFX_RGB(255, 0, 0),       GFX_RGB(255, 128, 0),
    GFX_RGB(255, 255, 0),     GFX_RGB(128, 64, 0),      GFX_RGB(0, 128, 0),
    GFX_RGB(0, 255, 0),       GFX_RGB(0, 128, 128),     GFX_RGB(0, 255, 255),
    GFX_RGB(0, 0, 128),       GFX_RGB(0, 0, 255),       GFX_RGB(255, 0, 255),
    // Custom slots, until colors are picked or typed in
    GFX_RGB(255, 192, 203),   GFX_RGB(173, 216, 230),   GFX_RGB(144, 238, 144),
    GFX_RGB(245, 222, 179),   GFX_RGB(255, 255, 160),   GFX_RGB(200, 180, 240),
};
static int next_custom;

// The color picker's basic colors, a row at a time
static const uint32_t basic_colors[PICK_COLS * PICK_ROWS] = {
    SEE_THROUGH,              GFX_RGB(1, 1, 1),         GFX_RGB(64, 64, 64),      GFX_RGB(128, 128, 128),
    GFX_RGB(160, 160, 160),   GFX_RGB(192, 192, 192),   GFX_RGB(224, 224, 224),   GFX_RGB(255, 255, 255),
    GFX_RGB(128, 0, 0),       GFX_RGB(128, 64, 0),      GFX_RGB(128, 128, 0),     GFX_RGB(0, 128, 0),
    GFX_RGB(0, 128, 128),     GFX_RGB(0, 0, 128),       GFX_RGB(64, 0, 128),      GFX_RGB(128, 0, 128),
    GFX_RGB(255, 0, 0),       GFX_RGB(255, 128, 0),     GFX_RGB(255, 255, 0),     GFX_RGB(0, 255, 0),
    GFX_RGB(0, 255, 255),     GFX_RGB(0, 0, 255),       GFX_RGB(128, 0, 255),     GFX_RGB(255, 0, 255),
    GFX_RGB(255, 128, 128),   GFX_RGB(255, 192, 128),   GFX_RGB(255, 255, 128),   GFX_RGB(128, 255, 128),
    GFX_RGB(128, 255, 255),   GFX_RGB(128, 128, 255),   GFX_RGB(192, 128, 255),   GFX_RGB(255, 128, 255),
    GFX_RGB(255, 192, 192),   GFX_RGB(255, 224, 192),   GFX_RGB(255, 255, 192),   GFX_RGB(192, 255, 192),
    GFX_RGB(192, 255, 255),   GFX_RGB(192, 192, 255),   GFX_RGB(224, 192, 255),   GFX_RGB(255, 192, 255),
    GFX_RGB(64, 0, 0),        GFX_RGB(96, 48, 0),       GFX_RGB(128, 64, 32),     GFX_RGB(160, 96, 48),
    GFX_RGB(192, 128, 64),    GFX_RGB(224, 176, 128),   GFX_RGB(0, 64, 32),       GFX_RGB(32, 32, 64),
};
static uint32_t pick_field[PICK_HS_W * PICK_HS_H];   // The hue / saturation field, made once
static bool     pick_field_made;

static doc_t        doc;
static wm_window_t  main_win;
static wm_theme_t   theme;
static bool         running = true;
static bool         main_dirty;
static bool         status_dirty;   // Only the status bar changed
static char         shown_title[WM_TITLE_MAX];

static int32_t      zoom = 8;
static int32_t      pan_x, pan_y;   // The image's top-left corner, relative to the canvas
static bool         grid = true;
static tool_t       tool = TOOL_PENCIL;
static tool_t       prev_tool = TOOL_PENCIL;
static int          brush = 1;
static uint32_t     color = GFX_RGB(1, 1, 1);

static bool         hover_in;       // The pointer is over the image
static int32_t      hover_x, hover_y;

static uint32_t*    view;           // The canvas, composed before it is drawn
static size_t       view_cap;
static int32_t*     col_map;        // Canvas column -> image column, or -1
static size_t       col_cap;

static snap_t       undo_stack[UNDO_MAX];
static int          undo_count;
static snap_t       redo_stack[UNDO_MAX];
static int          redo_count;

static struct {
    drag_t   kind;
    tool_t   tool;
    uint8_t  button;                // The button that started it
    bool     has_base;              // Shape: the undo snapshot to redraw it over
    int32_t  x0, y0;                // Shape: where it started (image)
    int32_t  last_x, last_y;        // Paint: the previous point (image)
    int32_t  mx, my;                // Pan: where it started (client)
    int32_t  pan_x, pan_y;
} drag;

typedef struct {
    char     text[PATH_MAX];
    uint32_t len;
    uint32_t cursor;
    uint32_t scroll;
} field_t;

static struct {
    dlg_kind_t  kind;
    wm_window_t win;
    then_t      then;
    int         focus;              // Confirm: the button the keyboard is on
    int         pressed;            // Button under a held mouse button, or -1
    bool        dirty;
    char        message[64];
    field_t     field[2];
    int         fields;             // 0, 1 or 2 (width and height)
    int         active;             // The field with the caret
    int         pick_drag;          // Color: 0, or 1 / 2 while the field / strip is held
    int32_t     hue, sat, val;      // Color: 0 - 359, 0 - 255, 0 - 255
    uint32_t    alpha;              // Color: kept from the hex value
} dlg;

static filedlg_t*   fdlg;           // Open / Save as, while it is up
static then_t       fdlg_then;      // Save as: what to do after saving

static void dialog_open(dlg_kind_t kind, then_t then);
static void file_dialog(filedlg_mode_t mode, then_t then);
static void run_then(then_t then);
static void save_then(then_t then);
static void field_set(field_t* f, const char* text);
static void draw_field(gfx_surface_t* s, int i);

//
// Small helpers
//

static void notify_error(const char* title, const char* message) {
    wm_notify_ex(WM_NOTIFY_ERROR, title, message, WM_NOTIFY_DEFAULT_MS * 2);
}

static void out_of_memory(void) {
    notify_error(APP_NAME, "Not enough memory");
}

static const char* doc_name(void) {
    if (doc.path[0] == '\0') return "Untitled";
    const char* slash = strrchr(doc.path, '/');
    return slash ? slash + 1 : doc.path;
}

static bool point_in(const gfx_rect_t* r, int32_t x, int32_t y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static int32_t floor_div(int32_t a, int32_t b) {
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

// "#RRGGBB", "#AARRGGBB" when not opaque, or "see-through"
static void format_color(uint32_t c, char* out, size_t size) {
    if (c == SEE_THROUGH)
        strlcpy(out, "see-through", size);
    else if (GFX_ALPHA(c) == 0xFF)
        snprintf(out, size, "#%06X", (unsigned)(c & 0xFFFFFFu));
    else
        snprintf(out, size, "#%08X", (unsigned)c);
}

// RRGGBB (opaque) or AARRGGBB, with or without a '#'
static bool parse_color(const char* text, uint32_t* out) {
    if (*text == '#') text++;
    size_t n = strlen(text);
    if (n != 6 && n != 8)
        return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = text[i];
        int d;
        if (ch >= '0' && ch <= '9')      d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return false;
        v = (v << 4) | (uint32_t)d;
    }
    *out = (n == 6) ? (0xFF000000u | v) : v;
    return true;
}

// Hue 0 - 359, saturation and value 0 - 255, to 0x00RRGGBB
static uint32_t hsv_to_rgb(int32_t h, int32_t s, int32_t v) {
    int32_t region = h / 60, f = (h % 60) * 255 / 60;
    int32_t p = v * (255 - s) / 255;
    int32_t q = v * (255 - s * f / 255) / 255;
    int32_t t = v * (255 - s * (255 - f) / 255) / 255;
    int32_t r, g, b;
    switch (region) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// The hue is left as it is for a gray, which has none
static void rgb_to_hsv(uint32_t c, int32_t* h, int32_t* s, int32_t* v) {
    int32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    int32_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int32_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int32_t d = max - min;
    *v = max;
    *s = max ? d * 255 / max : 0;
    if (d == 0)
        return;
    int32_t hue;
    if (max == r)      hue = 60 * (g - b) / d;
    else if (max == g) hue = 120 + 60 * (b - r) / d;
    else               hue = 240 + 60 * (r - g) / d;
    *h = (hue + 360) % 360;
}

static bool parse_dim(const char* text, int32_t* out) {
    char* end;
    unsigned long v = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || v < 1 || v > IMG_MAX_DIM)
        return false;
    *out = (int32_t)v;
    return true;
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
// Menu bar, toolbar, the canvas, the palette bar and the status bar, top to
// bottom. The image sits at (pan_x, pan_y) in the canvas, centered on an
// axis where it fits and kept from leaving empty space where it doesn't.
//

static metrics_t get_metrics(void) {
    metrics_t m;
    gfx_surface_t* s = (main_win != 0) ? wm_window_surface(main_win) : NULL;
    m.width = m.height = 0;
    if (main_win != 0)
        wm_window_get_size(main_win, &m.width, &m.height);
    if (m.width <= 0 || m.height <= 0) {        // No window yet
        m.width  = WIN_W;
        m.height = WIN_H;
    }
    m.cw        = s ? gfx_char_width(s) : 6;
    m.toolbar_y = MENUBAR_H;
    m.status_y  = m.height - STATUS_H;
    m.palette_y = m.status_y - PALETTE_H;

    int32_t top = MENUBAR_H + TOOLBAR_H;
    int32_t h = m.palette_y - top;
    m.canvas = gfx_rect(0, top, m.width, h > 1 ? h : 1);
    return m;
}

static void clamp_axis(int32_t* pan, int32_t size, int32_t room) {
    if (size <= room) {
        *pan = (room - size) / 2;
    } else {
        if (*pan > 0) *pan = 0;
        if (*pan < room - size) *pan = room - size;
    }
}

static void clamp_pan(void) {
    metrics_t m = get_metrics();
    clamp_axis(&pan_x, doc.w * zoom, m.canvas.w);
    clamp_axis(&pan_y, doc.h * zoom, m.canvas.h);
}

static void changed(void) {
    clamp_pan();
    main_dirty = true;
}

// Keep the image point under canvas point (ax, ay) where it is
static void zoom_to(int32_t z, int32_t ax, int32_t ay) {
    if (z == zoom)
        return;
    pan_x = ax - (ax - pan_x) * z / zoom;
    pan_y = ay - (ay - pan_y) * z / zoom;
    zoom = z;
    changed();
}

static void zoom_step(int dir) {
    metrics_t m = get_metrics();
    int32_t z = zoom;
    if (dir > 0) {
        for (int i = 0; i < ZOOM_LEVELS; i++)
            if (zoom_levels[i] > zoom) { z = zoom_levels[i]; break; }
    } else {
        for (int i = ZOOM_LEVELS - 1; i >= 0; i--)
            if (zoom_levels[i] < zoom) { z = zoom_levels[i]; break; }
    }
    zoom_to(z, m.canvas.w / 2, m.canvas.h / 2);
}

// The largest zoom level that shows all of the image
static void zoom_fit(void) {
    metrics_t m = get_metrics();
    int32_t fw = (m.canvas.w - FIT_MARGIN) / doc.w;
    int32_t fh = (m.canvas.h - FIT_MARGIN) / doc.h;
    int32_t f = (fw < fh) ? fw : fh;
    zoom = 1;
    for (int i = 0; i < ZOOM_LEVELS; i++)
        if (zoom_levels[i] <= f) zoom = zoom_levels[i];
    pan_x = pan_y = 0;
    changed();
}

// Canvas (client) point to image pixel; false when outside the image
static bool to_image(const metrics_t* m, int32_t x, int32_t y, int32_t* ix, int32_t* iy) {
    *ix = floor_div(x - m->canvas.x - pan_x, zoom);
    *iy = floor_div(y - m->canvas.y - pan_y, zoom);
    return *ix >= 0 && *iy >= 0 && *ix < doc.w && *iy < doc.h;
}

// Toolbar: the tools, then the brush size (i == TOOL_COUNT); the current
// color sits at the right end
static gfx_rect_t tool_rect(const metrics_t* m, int i) {
    int32_t x = TOOL_X;
    for (int k = 0; k < i; k++)
        x += (int32_t)strlen(tool_names[k]) * m->cw + 10 + TOOL_GAP;
    if (i == TOOL_COUNT)
        x += 6;
    int32_t chars = (i == TOOL_COUNT) ? 6 : (int32_t)strlen(tool_names[i]);   // "Size 8"
    return gfx_rect(x, m->toolbar_y + (TOOLBAR_H - TOOL_BTN_H) / 2, chars * m->cw + 10, TOOL_BTN_H);
}

static gfx_rect_t color_rect(const metrics_t* m) {
    return gfx_rect(m->width - TOOL_X - COLOR_BTN_W, m->toolbar_y + (TOOLBAR_H - TOOL_BTN_H) / 2,
                    COLOR_BTN_W, TOOL_BTN_H);
}

static gfx_rect_t swatch_rect(const metrics_t* m, int i) {
    return gfx_rect(PAD_X + i * (SWATCH + SWATCH_GAP), m->palette_y + (PALETTE_H - SWATCH) / 2, SWATCH, SWATCH);
}

//
// Undo
//
// Each step is a copy of the whole image from before a change. Undoing
// swaps the image with the newest copy, so it costs no memory.
//

static void snap_free(snap_t* s) {
    free(s->px);
    s->px = NULL;
}

static size_t snap_bytes(const snap_t* s) {
    return (size_t)s->w * (size_t)s->h * sizeof(uint32_t);
}

static void drop_oldest(snap_t* stack, int* count) {
    snap_free(&stack[0]);
    memmove(&stack[0], &stack[1], (size_t)(*count - 1) * sizeof(snap_t));
    (*count)--;
}

static void history_clear(void) {
    while (undo_count > 0) snap_free(&undo_stack[--undo_count]);
    while (redo_count > 0) snap_free(&redo_stack[--redo_count]);
}

// Remember the image before a change. False when there was no memory for it
// (the change can still go ahead, it just can't be undone).
static bool undo_push(void) {
    while (redo_count > 0) snap_free(&redo_stack[--redo_count]);
    if (undo_count == UNDO_MAX)
        drop_oldest(undo_stack, &undo_count);

    snap_t s = { doc.w, doc.h, NULL };
    s.px = (uint32_t*)malloc(snap_bytes(&s));
    if (s.px == NULL && undo_count > 0) {
        history_clear();                        // Make room, then try once more
        s.px = (uint32_t*)malloc(snap_bytes(&s));
    }
    if (s.px == NULL)
        return false;
    memcpy(s.px, doc.px, snap_bytes(&s));
    undo_stack[undo_count++] = s;

    size_t total = 0;
    for (int i = 0; i < undo_count; i++) total += snap_bytes(&undo_stack[i]);
    while (undo_count > 1 && total > UNDO_BYTES) {
        total -= snap_bytes(&undo_stack[0]);
        drop_oldest(undo_stack, &undo_count);
    }
    return true;
}

// Undo or redo: the image goes on the other stack, the newest step comes back
static void history_step(snap_t* from, int* from_count, snap_t* to, int* to_count) {
    if (*from_count == 0)
        return;
    if (*to_count == UNDO_MAX)
        drop_oldest(to, to_count);
    snap_t cur = { doc.w, doc.h, doc.px };
    to[(*to_count)++] = cur;

    snap_t s = from[--(*from_count)];
    doc.w  = s.w;
    doc.h  = s.h;
    doc.px = s.px;
    doc.modified = true;
    changed();
}

//
// Drawing on the image
//

static uint32_t paint_color(void) {
    return (tool == TOOL_ERASER) ? SEE_THROUGH : color;
}

static void put_pixel(int32_t x, int32_t y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < doc.w && y < doc.h)
        doc.px[y * doc.w + x] = c;
}

// A size x size square around (x, y)
static void stamp(int32_t x, int32_t y, uint32_t c, int size) {
    int32_t x0 = x - (size - 1) / 2, y0 = y - (size - 1) / 2;
    for (int32_t dy = 0; dy < size; dy++)
        for (int32_t dx = 0; dx < size; dx++)
            put_pixel(x0 + dx, y0 + dy, c);
}

static void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c, int size) {
    int32_t dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    int32_t dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        stamp(x0, y0, c, size);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_rect_outline(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c, int size) {
    draw_line(x0, y0, x1, y0, c, size);
    draw_line(x1, y0, x1, y1, c, size);
    draw_line(x1, y1, x0, y1, c, size);
    draw_line(x0, y1, x0, y0, c, size);
}

static void fill_box(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
    if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int32_t t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= doc.w) x1 = doc.w - 1;
    if (y1 >= doc.h) y1 = doc.h - 1;
    for (int32_t y = y0; y <= y1; y++)
        for (int32_t x = x0; x <= x1; x++)
            doc.px[y * doc.w + x] = c;
}

// Flood fill of the area of one color around (x, y), a row span at a time
static void flood_fill(int32_t x, int32_t y, uint32_t c) {
    uint32_t target = doc.px[y * doc.w + x];
    if (target == c)
        return;

    size_t cap = 256, top = 0;
    int32_t* stack = (int32_t*)malloc(cap * 2 * sizeof(int32_t));
    if (stack == NULL) {
        out_of_memory();
        return;
    }
    stack[top * 2] = x;
    stack[top * 2 + 1] = y;
    top++;

    while (top > 0) {
        top--;
        int32_t sx = stack[top * 2], sy = stack[top * 2 + 1];
        uint32_t* row = doc.px + sy * doc.w;
        if (row[sx] != target)
            continue;

        int32_t l = sx, r = sx;
        while (l > 0 && row[l - 1] == target) l--;
        while (r < doc.w - 1 && row[r + 1] == target) r++;
        for (int32_t i = l; i <= r; i++)
            row[i] = c;

        // The start of every run of the target color just above and below
        for (int32_t ny = sy - 1; ny <= sy + 1; ny += 2) {
            if (ny < 0 || ny >= doc.h)
                continue;
            uint32_t* nrow = doc.px + ny * doc.w;
            for (int32_t i = l; i <= r; i++) {
                if (nrow[i] != target || (i > l && nrow[i - 1] == target))
                    continue;
                if (top == cap) {
                    int32_t* p = (int32_t*)realloc(stack, cap * 4 * sizeof(int32_t));
                    if (p == NULL) {
                        out_of_memory();        // Part of the area stays as it was
                        free(stack);
                        return;
                    }
                    stack = p;
                    cap *= 2;
                }
                stack[top * 2] = i;
                stack[top * 2 + 1] = ny;
                top++;
            }
        }
    }
    free(stack);
}

static uint32_t* alloc_pixels(int32_t w, int32_t h) {
    return (uint32_t*)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
}

static void fill_pixels(uint32_t* px, size_t count, uint32_t c) {
    for (size_t i = 0; i < count; i++)
        px[i] = c;
}

// Swap in a new pixel buffer of a new size (the old one is freed)
static void replace_pixels(uint32_t* px, int32_t w, int32_t h) {
    free(doc.px);
    doc.px = px;
    doc.w  = w;
    doc.h  = h;
    doc.modified = true;
    changed();
}

//
// Image commands
//

static void flip_horizontal(void) {
    undo_push();
    for (int32_t y = 0; y < doc.h; y++) {
        uint32_t* row = doc.px + y * doc.w;
        for (int32_t l = 0, r = doc.w - 1; l < r; l++, r--) {
            uint32_t t = row[l]; row[l] = row[r]; row[r] = t;
        }
    }
    doc.modified = true;
}

static void flip_vertical(void) {
    undo_push();
    for (int32_t t = 0, b = doc.h - 1; t < b; t++, b--) {
        uint32_t* a = doc.px + t * doc.w;
        uint32_t* c = doc.px + b * doc.w;
        for (int32_t x = 0; x < doc.w; x++) {
            uint32_t v = a[x]; a[x] = c[x]; c[x] = v;
        }
    }
    doc.modified = true;
}

static void rotate(bool clockwise) {
    int32_t nw = doc.h, nh = doc.w;
    uint32_t* px = alloc_pixels(nw, nh);
    if (px == NULL) {
        out_of_memory();
        return;
    }
    for (int32_t y = 0; y < nh; y++)
        for (int32_t x = 0; x < nw; x++) {
            // Clockwise, source (sx, sy) lands on (h - 1 - sy, sx)
            int32_t sx = clockwise ? y : doc.w - 1 - y;
            int32_t sy = clockwise ? doc.h - 1 - x : x;
            px[y * nw + x] = doc.px[sy * doc.w + sx];
        }
    undo_push();
    replace_pixels(px, nw, nh);
}

// New size, the picture kept at the top left, new area see-through
static void resize_canvas(int32_t nw, int32_t nh) {
    if (nw == doc.w && nh == doc.h)
        return;
    uint32_t* px = alloc_pixels(nw, nh);
    if (px == NULL) {
        out_of_memory();
        return;
    }
    fill_pixels(px, (size_t)nw * nh, SEE_THROUGH);
    int32_t cw = (nw < doc.w) ? nw : doc.w;
    int32_t ch = (nh < doc.h) ? nh : doc.h;
    for (int32_t y = 0; y < ch; y++)
        memcpy(px + y * nw, doc.px + y * doc.w, (size_t)cw * sizeof(uint32_t));
    undo_push();
    replace_pixels(px, nw, nh);
}

static void clear_image(void) {
    undo_push();
    fill_pixels(doc.px, (size_t)doc.w * doc.h, SEE_THROUGH);
    doc.modified = true;
}

static void copy_image(void) {
    if ((size_t)doc.w * doc.h * sizeof(uint32_t) > WM_CLIP_MAX) {
        notify_error("Can't copy", "The image is too big for the clipboard");
        return;
    }
    if (wm_clip_set_image(doc.px, doc.w, doc.h) < 0)
        notify_error("Can't copy", strerror(errno));
    else
        wm_notify("Copied", "The image is on the clipboard");
}

// The clipboard's image replaces this one (undoable)
static void paste_image(void) {
    wm_clip_info_t info;
    void* data = wm_clip_get(&info);
    if (data == NULL || info.type != WM_CLIP_IMAGE ||
        info.width < 1 || info.height < 1 || info.width > IMG_MAX_DIM || info.height > IMG_MAX_DIM ||
        info.size != info.width * info.height * sizeof(uint32_t)) {
        free(data);
        notify_error("Can't paste", "The clipboard holds no image of a size this can edit");
        return;
    }
    undo_push();
    replace_pixels((uint32_t*)data, (int32_t)info.width, (int32_t)info.height);
}

//
// Files
//

// An image file, as pixels. 0, or -1 with errno (ENOEXEC: not a picture,
// or damaged; EFBIG: larger than IMG_MAX_DIM).
static int load_image(const char* path, int32_t* out_w, int32_t* out_h, uint32_t** out_px) {
    bitmap_t b;
    if (bitmap_load(path, &b) < 0)
        return -1;
    if (b.width > IMG_MAX_DIM || b.height > IMG_MAX_DIM) {
        bitmap_free(&b);
        errno = EFBIG;
        return -1;
    }
    *out_w = b.width;
    *out_h = b.height;
    *out_px = b.pixels;
    return 0;
}

// The image as a PNG file, written in place so the file keeps its own
// permissions and clearance. 0, or -1 with errno.
static int save_image(const char* path) {
    return bitmap_save_png(path, doc.px, doc.w, doc.h, 0);
}

static void reset_view(void) {
    history_clear();
    if (drag.kind != DRAG_NONE)
        drag.kind = DRAG_NONE;
    hover_in = false;
    zoom_fit();
}

static bool new_image(int32_t w, int32_t h) {
    uint32_t* px = alloc_pixels(w, h);
    if (px == NULL) {
        out_of_memory();
        return false;
    }
    fill_pixels(px, (size_t)w * h, SEE_THROUGH);
    free(doc.px);
    doc.px = px;
    doc.w = w;
    doc.h = h;
    doc.modified = false;
    doc.path[0] = '\0';
    reset_view();
    return true;
}

// Replace the image with the file at `path`. A missing file is fine when
// missing_ok (a new image with that name, created by the first save).
// On failure the user has been told why and the current image stays.
static bool open_document(const char* path, bool missing_ok) {
    char abs[PATH_MAX];
    if (os_abspath(path, abs, sizeof(abs)) == NULL) {
        notify_error("Can't open file", strerror(errno));
        return false;
    }

    int32_t w, h;
    uint32_t* px;
    if (load_image(abs, &w, &h, &px) < 0) {
        if (errno == ENOENT && missing_ok) {
            if (!new_image(NEW_W, NEW_H))
                return false;
            strlcpy(doc.path, abs, sizeof(doc.path));
            return true;
        }
        char title[WM_NOTIFY_TITLE_MAX];
        const char* slash = strrchr(abs, '/');
        snprintf(title, sizeof(title), "Can't open %s", slash ? slash + 1 : abs);
        const char* why = (errno == ENOEXEC) ? "Not an image file, or damaged"
                        : (errno == EFBIG)   ? "Too big: up to 2048 x 2048 pixels"
                        : strerror(errno);
        notify_error(title, why);
        return false;
    }

    free(doc.px);
    doc.px = px;
    doc.w = w;
    doc.h = h;
    doc.modified = false;
    strlcpy(doc.path, abs, sizeof(doc.path));
    reset_view();
    return true;
}

static bool save_document(void) {
    if (save_image(doc.path) < 0) {
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
        case THEN_NEW:  dialog_open(DLG_NEW, THEN_NOTHING);      break;
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

// `pixel` shown over `under` the way the dwm draws its images
static uint32_t composite(uint32_t pixel, uint32_t under) {
    uint32_t a = GFX_ALPHA(pixel);
    if (a == 0xFF)
        return (pixel == SEE_THROUGH) ? under : pixel;
    if (a == 0)
        return under;
    uint32_t ia = 255 - a;
    uint32_t r = (GFX_RED(pixel)   * a + GFX_RED(under)   * ia + 127) / 255;
    uint32_t g = (GFX_GREEN(pixel) * a + GFX_GREEN(under) * ia + 127) / 255;
    uint32_t b = (GFX_BLUE(pixel)  * a + GFX_BLUE(under)  * ia + 127) / 255;
    return GFX_RGB(r, g, b);
}

static uint32_t grid_mix(uint32_t c) {
    return 0xFF000000u | (((c & 0xFEFEFEu) >> 1) + ((GRID_COLOR & 0xFEFEFEu) >> 1));
}

static bool ensure_view(size_t pixels, size_t columns) {
    if (pixels > view_cap) {
        uint32_t* p = (uint32_t*)realloc(view, pixels * sizeof(uint32_t));
        if (p == NULL) return false;
        view = p;
        view_cap = pixels;
    }
    if (columns > col_cap) {
        int32_t* p = (int32_t*)realloc(col_map, columns * sizeof(int32_t));
        if (p == NULL) return false;
        col_map = p;
        col_cap = columns;
    }
    return true;
}

// The canvas is composed here, zoomed, over the checkerboard, then drawn
// with one call
static void draw_canvas(gfx_surface_t* s, const metrics_t* m) {
    int32_t aw = m->canvas.w, ah = m->canvas.h;
    if (!ensure_view((size_t)aw * ah, (size_t)aw)) {
        gfx_fill_rect(s, m->canvas.x, m->canvas.y, aw, ah, BACKDROP);
        return;
    }

    int32_t iw = doc.w * zoom, ih = doc.h * zoom;
    for (int32_t x = 0; x < aw; x++) {
        int32_t rx = x - pan_x;
        col_map[x] = (rx >= 0 && rx < iw) ? rx / zoom : -1;
    }
    bool show_grid = grid && zoom >= 4;

    for (int32_t y = 0; y < ah; y++) {
        uint32_t* out = view + (size_t)y * aw;
        int32_t ry = y - pan_y;
        if (ry < 0 || ry >= ih) {
            fill_pixels(out, (size_t)aw, BACKDROP);
            continue;
        }
        const uint32_t* src = doc.px + (ry / zoom) * doc.w;
        bool grid_row = show_grid && ry % zoom == 0;
        uint32_t check_row = (uint32_t)(ry / CHECKER) & 1u;

        for (int32_t x = 0; x < aw; x++) {
            int32_t ix = col_map[x];
            if (ix < 0) {
                out[x] = BACKDROP;
                continue;
            }
            int32_t rx = x - pan_x;
            uint32_t under = (((uint32_t)(rx / CHECKER) & 1u) ^ check_row) ? CHECK_DARK : CHECK_LIGHT;
            uint32_t c = composite(src[ix], under);
            if (show_grid && (grid_row || rx % zoom == 0))
                c = grid_mix(c);
            out[x] = c;
        }
    }
    gfx_draw_pixels(s, view, aw, ah, 0, m->canvas.x, m->canvas.y);
}

// A color sample: the checkerboard, the color blended over it
static void draw_swatch(gfx_surface_t* s, const gfx_rect_t* r, uint32_t c, bool selected) {
    int32_t hw = r->w / 2, hh = r->h / 2;
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, CHECK_LIGHT);
    gfx_fill_rect(s, r->x + hw, r->y, r->w - hw, hh, CHECK_DARK);
    gfx_fill_rect(s, r->x, r->y + hh, hw, r->h - hh, CHECK_DARK);
    if (c != SEE_THROUGH) {
        gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
        gfx_fill_rect(s, r->x, r->y, r->w, r->h, c);
        gfx_set_blend(s, old);
    }
    gfx_color_t border = selected ? theme.button.border_active : theme.button.border;
    gfx_draw_rect(s, r->x - 1, r->y - 1, r->w + 2, r->h + 2, border);
    if (selected)
        gfx_draw_rect(s, r->x - 2, r->y - 2, r->w + 4, r->h + 4, border);
}

static void draw_tool_button(gfx_surface_t* s, const gfx_rect_t* r, const char* label, bool active) {
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, active ? theme.button.fill_pressed : theme.button.fill);
    gfx_draw_rect(s, r->x, r->y, r->w, r->h, active ? theme.button.border_active : theme.button.border);
    int32_t tw = gfx_text_width(s, label);
    gfx_draw_text(s, r->x + (r->w - tw) / 2, r->y + (r->h - gfx_line_height(s)) / 2 + 1, label,
                  theme.button.text);
}

static void draw_bars(gfx_surface_t* s, const metrics_t* m) {
    // Menu bar
    gfx_fill_rect(s, 0, 0, m->width, MENUBAR_H, theme.button.fill);
    gfx_draw_hline(s, 0, MENUBAR_H - 1, m->width, theme.button.border);
    int32_t y = (MENUBAR_H - gfx_line_height(s)) / 2;
    for (int i = 0; i < MENU_COUNT; i++)
        gfx_draw_text(s, menus[i].x + 4, y, menus[i].label, theme.button.text);

    // Toolbar
    gfx_fill_rect(s, 0, m->toolbar_y, m->width, TOOLBAR_H, theme.button.fill);
    gfx_draw_hline(s, 0, m->toolbar_y + TOOLBAR_H - 1, m->width, theme.button.border);
    for (int i = 0; i < TOOL_COUNT; i++) {
        gfx_rect_t r = tool_rect(m, i);
        draw_tool_button(s, &r, tool_names[i], (int)tool == i);
    }
    char size[8];
    snprintf(size, sizeof(size), "Size %d", brush);
    gfx_rect_t r = tool_rect(m, TOOL_COUNT);
    draw_tool_button(s, &r, size, false);
    gfx_rect_t cr = color_rect(m);
    if (cr.x > r.x + r.w + 4) {
        cr.x += 1; cr.y += 1; cr.w -= 2; cr.h -= 2;
        draw_swatch(s, &cr, color, false);
    }

    // Palette
    gfx_fill_rect(s, 0, m->palette_y, m->width, PALETTE_H, theme.client.background);
    gfx_draw_hline(s, 0, m->palette_y, m->width, theme.button.border);
    for (int i = 0; i < SWATCH_COUNT; i++) {
        gfx_rect_t sr = swatch_rect(m, i);
        if (sr.x + sr.w > m->width)
            break;
        draw_swatch(s, &sr, swatches[i], swatches[i] == color);
    }
}

static void draw_status(gfx_surface_t* s, const metrics_t* m) {
    gfx_fill_rect(s, 0, m->status_y, m->width, STATUS_H, theme.button.fill);
    gfx_draw_hline(s, 0, m->status_y, m->width, theme.button.border);

    char left[96], right[40], c[16], under[16];
    format_color(color, c, sizeof(c));
    int n = snprintf(left, sizeof(left), "%s   Color %s", tool_names[tool], c);
    if (hover_in && n > 0 && (size_t)n < sizeof(left)) {
        format_color(doc.px[hover_y * doc.w + hover_x], under, sizeof(under));
        snprintf(left + n, sizeof(left) - (size_t)n, "   Pixel %d, %d  %s", (int)hover_x, (int)hover_y, under);
    }
    snprintf(right, sizeof(right), "%d x %d   %d%%", (int)doc.w, (int)doc.h, (int)zoom * 100);

    int32_t y = m->status_y + (STATUS_H - gfx_line_height(s)) / 2 + 1;
    gfx_draw_text(s, PAD_X, y, left, theme.button.text);
    gfx_draw_text(s, m->width - PAD_X - gfx_text_width(s, right), y, right, theme.button.text);
}

static void draw_main(void) {
    main_dirty = status_dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics();

    draw_canvas(s, &m);
    draw_bars(s, &m);
    draw_status(s, &m);

    wm_window_commit(main_win, NULL);
    update_title();
}

// The pointer moved over the image: just the status bar
static void draw_status_only(void) {
    status_dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics();
    draw_status(s, &m);
    gfx_rect_t r = gfx_rect(0, m.status_y, m.width, STATUS_H);
    wm_window_commit(main_win, &r);
}

//
// Menus and commands
//

static void set_tool(tool_t t) {
    if (t != TOOL_PICK)
        prev_tool = t;
    else if (tool != TOOL_PICK)
        prev_tool = tool;
    tool = t;
    main_dirty = true;
}

static void set_brush(int size) {
    brush = (size < 1) ? 1 : (size > BRUSH_MAX) ? BRUSH_MAX : size;
    main_dirty = true;
}

// A color not in the palette goes into the next custom slot
static void use_color(uint32_t c) {
    color = c;
    for (int i = 0; i < SWATCH_COUNT; i++)
        if (swatches[i] == c)
            goto done;
    swatches[SWATCH_FIXED + next_custom] = c;
    next_custom = (next_custom + 1) % SWATCH_CUSTOM;
done:
    main_dirty = true;
}

static void show_menu(int which, int32_t x, int32_t y) {
    uint32_t no_undo = undo_count ? 0 : WM_MENU_DISABLED;
    uint32_t no_redo = redo_count ? 0 : WM_MENU_DISABLED;

    wm_menu_item_t file[] = {
        { "New...      Ctrl+N",       CMD_NEW,     0 },
        { "Open...     Ctrl+O",       CMD_OPEN,    0 },
        { "Save        Ctrl+S",       CMD_SAVE,    0 },
        { "Save as...  Ctrl+Shift+S", CMD_SAVE_AS, 0 },
        { NULL,                       0,           WM_MENU_SEPARATOR },
        { "Exit",                     CMD_EXIT,    0 },
    };
    wm_menu_item_t edit[] = {
        { "Undo         Ctrl+Z", CMD_UNDO,  no_undo },
        { "Redo         Ctrl+Y", CMD_REDO,  no_redo },
        { NULL,                  0,         WM_MENU_SEPARATOR },
        { "Copy image   Ctrl+C", CMD_COPY,  0 },
        { "Paste image  Ctrl+V", CMD_PASTE, 0 },
        { NULL,                  0,         WM_MENU_SEPARATOR },
        { "Color...     C",      CMD_COLOR, 0 },
    };
    wm_menu_item_t image[] = {
        { "Canvas size...",      CMD_CANVAS_SIZE,  0 },
        { NULL,                  0,                WM_MENU_SEPARATOR },
        { "Flip horizontally",   CMD_FLIP_H,       0 },
        { "Flip vertically",     CMD_FLIP_V,       0 },
        { "Rotate right",        CMD_ROTATE_RIGHT, 0 },
        { "Rotate left",         CMD_ROTATE_LEFT,  0 },
        { NULL,                  0,                WM_MENU_SEPARATOR },
        { "Clear",               CMD_CLEAR,        0 },
    };
    wm_menu_item_t view_items[] = {
        { "Zoom in        +",    CMD_ZOOM_IN,     zoom < 32 ? 0 : WM_MENU_DISABLED },
        { "Zoom out       -",    CMD_ZOOM_OUT,    zoom > 1 ? 0 : WM_MENU_DISABLED },
        { "Actual size    1",    CMD_ZOOM_ACTUAL, 0 },
        { "Fit to window  0",    CMD_ZOOM_FIT,    0 },
        { NULL,                  0,               WM_MENU_SEPARATOR },
        { "Grid           G",    CMD_GRID,        grid ? WM_MENU_CHECKED : 0 },
    };

    switch (which) {
        case 0: wm_menu_show(main_win, x, y, file, sizeof(file) / sizeof(file[0]));    break;
        case 1: wm_menu_show(main_win, x, y, edit, sizeof(edit) / sizeof(edit[0]));    break;
        case 2: wm_menu_show(main_win, x, y, image, sizeof(image) / sizeof(image[0])); break;
        case 3: wm_menu_show(main_win, x, y, view_items, sizeof(view_items) / sizeof(view_items[0])); break;
    }
}

// Right click: the tools
static void show_tool_menu(int32_t x, int32_t y) {
    static const char* const labels[TOOL_COUNT] = {
        "Pencil      P", "Eraser      E", "Fill        F", "Line        L",
        "Rectangle   R", "Filled box  B", "Pick color  I", "Pan         H",
    };
    wm_menu_item_t items[TOOL_COUNT + 2];
    for (int i = 0; i < TOOL_COUNT; i++) {
        items[i].label = labels[i];
        items[i].id    = CMD_TOOL + (uint32_t)i;
        items[i].flags = ((int)tool == i) ? WM_MENU_CHECKED : 0;
    }
    items[TOOL_COUNT].label     = NULL;
    items[TOOL_COUNT].id        = 0;
    items[TOOL_COUNT].flags     = WM_MENU_SEPARATOR;
    items[TOOL_COUNT + 1].label = "Color...    C";
    items[TOOL_COUNT + 1].id    = CMD_COLOR;
    items[TOOL_COUNT + 1].flags = 0;
    wm_menu_show(main_win, x, y, items, TOOL_COUNT + 2);
}

static void command(uint32_t id) {
    if (id >= CMD_TOOL && id < CMD_TOOL + TOOL_COUNT) {
        set_tool((tool_t)(id - CMD_TOOL));
        return;
    }
    switch (id) {
        case CMD_NEW:          ask_then(THEN_NEW);                          break;
        case CMD_OPEN:         ask_then(THEN_OPEN);                         break;
        case CMD_SAVE:         save_then(THEN_NOTHING);                     break;
        case CMD_SAVE_AS:      file_dialog(FILEDLG_SAVE, THEN_NOTHING);     break;
        case CMD_EXIT:         ask_then(THEN_EXIT);                         break;
        case CMD_UNDO:         history_step(undo_stack, &undo_count, redo_stack, &redo_count); break;
        case CMD_REDO:         history_step(redo_stack, &redo_count, undo_stack, &undo_count); break;
        case CMD_COPY:         copy_image();                                break;
        case CMD_PASTE:        paste_image();                               break;
        case CMD_COLOR:        dialog_open(DLG_COLOR, THEN_NOTHING);        break;
        case CMD_CANVAS_SIZE:  dialog_open(DLG_RESIZE, THEN_NOTHING);       break;
        case CMD_FLIP_H:       flip_horizontal();                           break;
        case CMD_FLIP_V:       flip_vertical();                             break;
        case CMD_ROTATE_RIGHT: rotate(true);                                break;
        case CMD_ROTATE_LEFT:  rotate(false);                               break;
        case CMD_CLEAR:        clear_image();                               break;
        case CMD_ZOOM_IN:      zoom_step(1);                                break;
        case CMD_ZOOM_OUT:     zoom_step(-1);                               break;
        case CMD_ZOOM_ACTUAL: {
            metrics_t m = get_metrics();
            zoom_to(1, m.canvas.w / 2, m.canvas.h / 2);
            break;
        }
        case CMD_ZOOM_FIT:     zoom_fit();                                  break;
        case CMD_GRID:         grid = !grid;                                break;
        default:                                                            return;
    }
    changed();
}

//
// Input: main window
//

static void shape_to(int32_t ix, int32_t iy) {
    if (drag.has_base) {
        const snap_t* base = &undo_stack[undo_count - 1];
        memcpy(doc.px, base->px, snap_bytes(base));
    }
    uint32_t c = color;
    switch (drag.tool) {
        case TOOL_LINE: draw_line(drag.x0, drag.y0, ix, iy, c, brush);         break;
        case TOOL_RECT: draw_rect_outline(drag.x0, drag.y0, ix, iy, c, brush); break;
        case TOOL_BOX:  fill_box(drag.x0, drag.y0, ix, iy, c);                 break;
        default:                                                               break;
    }
}

// Escape while drawing: put the image back as it was before the stroke
static void cancel_drag(void) {
    if ((drag.kind == DRAG_PAINT || drag.kind == DRAG_SHAPE) && drag.has_base && undo_count > 0) {
        snap_t* base = &undo_stack[undo_count - 1];
        memcpy(doc.px, base->px, snap_bytes(base));
        snap_free(base);
        undo_count--;
    }
    drag.kind = DRAG_NONE;
    main_dirty = true;
}

static void start_pan(const wm_event_t* ev) {
    drag.kind   = DRAG_PAN;
    drag.button = ev->button;
    drag.mx     = ev->x;
    drag.my     = ev->y;
    drag.pan_x  = pan_x;
    drag.pan_y  = pan_y;
}

static void canvas_down(const metrics_t* m, const wm_event_t* ev) {
    if (ev->button == INPUT_BTN_MIDDLE || tool == TOOL_PAN) {
        start_pan(ev);
        return;
    }

    int32_t ix, iy;
    bool inside = to_image(m, ev->x, ev->y, &ix, &iy);

    switch (tool) {
        case TOOL_PENCIL:
        case TOOL_ERASER:
            drag.kind     = DRAG_PAINT;
            drag.button   = ev->button;
            drag.has_base = undo_push();
            drag.last_x   = ix;
            drag.last_y   = iy;
            stamp(ix, iy, paint_color(), brush);
            doc.modified = true;
            break;

        case TOOL_LINE:
        case TOOL_RECT:
        case TOOL_BOX:
            drag.kind     = DRAG_SHAPE;
            drag.tool     = tool;
            drag.button   = ev->button;
            drag.has_base = undo_push();
            drag.x0       = ix;
            drag.y0       = iy;
            shape_to(ix, iy);
            doc.modified = true;
            break;

        case TOOL_FILL:
            if (inside && doc.px[iy * doc.w + ix] != color) {
                undo_push();
                flood_fill(ix, iy, color);
                doc.modified = true;
            }
            break;

        case TOOL_PICK:
            if (inside) {
                use_color(doc.px[iy * doc.w + ix]);
                set_tool(prev_tool);
            }
            break;

        default:
            break;
    }
    main_dirty = true;
}

static void main_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT && ev->button != INPUT_BTN_MIDDLE)
        return;
    metrics_t m = get_metrics();

    if (point_in(&m.canvas, ev->x, ev->y)) {
        canvas_down(&m, ev);
        return;
    }
    if (ev->button != INPUT_BTN_LEFT)
        return;

    if (ev->y < MENUBAR_H) {
        for (int i = 0; i < MENU_COUNT; i++)
            if (ev->x >= menus[i].x && ev->x < menus[i].x + menus[i].w)
                show_menu(i, menus[i].x, MENUBAR_H);
        return;
    }

    if (ev->y < m.canvas.y) {           // Toolbar
        for (int i = 0; i <= TOOL_COUNT; i++) {
            gfx_rect_t r = tool_rect(&m, i);
            if (!point_in(&r, ev->x, ev->y))
                continue;
            if (i == TOOL_COUNT)
                set_brush(brush % BRUSH_MAX + 1);
            else
                set_tool((tool_t)i);
            return;
        }
        gfx_rect_t cr = color_rect(&m);
        if (point_in(&cr, ev->x, ev->y))
            dialog_open(DLG_COLOR, THEN_NOTHING);
        return;
    }

    if (ev->y < m.status_y) {           // Palette
        for (int i = 0; i < SWATCH_COUNT; i++) {
            gfx_rect_t r = swatch_rect(&m, i);
            r.x -= SWATCH_GAP / 2;
            r.w += SWATCH_GAP;
            if (point_in(&r, ev->x, ev->y)) {
                color = swatches[i];
                main_dirty = true;
                return;
            }
        }
    }
}

static void main_mouse_move(const wm_event_t* ev) {
    metrics_t m = get_metrics();
    int32_t ix, iy;
    bool inside = point_in(&m.canvas, ev->x, ev->y) && to_image(&m, ev->x, ev->y, &ix, &iy);
    if (inside != hover_in || (inside && (ix != hover_x || iy != hover_y))) {
        hover_in = inside;
        if (inside) {
            hover_x = ix;
            hover_y = iy;
        }
        status_dirty = true;
    }

    if (drag.kind == DRAG_NONE)
        return;
    if (!(ev->buttons & drag.button)) {     // Let go outside the window
        drag.kind = DRAG_NONE;
        main_dirty = true;
        return;
    }

    if (drag.kind == DRAG_PAN) {
        pan_x = drag.pan_x + (ev->x - drag.mx);
        pan_y = drag.pan_y + (ev->y - drag.my);
        changed();
        return;
    }

    to_image(&m, ev->x, ev->y, &ix, &iy);
    if (drag.kind == DRAG_PAINT) {
        if (ix == drag.last_x && iy == drag.last_y)
            return;
        draw_line(drag.last_x, drag.last_y, ix, iy, paint_color(), brush);
        drag.last_x = ix;
        drag.last_y = iy;
    } else {
        shape_to(ix, iy);
    }
    main_dirty = true;
}

static void main_key(const wm_event_t* ev) {
    uint16_t key = ev->key;
    bool shift = (ev->mods & INPUT_MOD_SHIFT) != 0;
    bool ctrl  = (ev->mods & INPUT_MOD_CTRL) != 0;

    if (ctrl) {
        // The letter, whether the dwm sends 'c', 'C' or the control code 3
        char letter = 0;
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))
            letter = (char)(key | 0x20);
        else if (key >= 3 && key <= 26 && key != KEY_TAB)
            letter = (char)('a' + key - 1);

        if (drag.kind != DRAG_NONE)
            return;                             // Not in the middle of a stroke
        switch (letter) {
            case 'z': command(CMD_UNDO);                       break;
            case 'y': command(CMD_REDO);                       break;
            case 'c': command(CMD_COPY);                       break;
            case 'v': command(CMD_PASTE);                      break;
            case 's': command(shift ? CMD_SAVE_AS : CMD_SAVE); break;
            case 'o': command(CMD_OPEN);                       break;
            case 'n': command(CMD_NEW);                        break;
            default:                                           break;   // Swallow the rest
        }
        return;
    }

    if (key == KEY_ESCAPE) {
        if (drag.kind != DRAG_NONE)
            cancel_drag();
        return;
    }

    int32_t step = shift ? 256 : 32;
    switch (key) {
        case KEY_LEFT:      pan_x += step; changed(); return;
        case KEY_RIGHT:     pan_x -= step; changed(); return;
        case KEY_UP:        pan_y += step; changed(); return;
        case KEY_DOWN:      pan_y -= step; changed(); return;
        case KEY_PAGE_UP:   pan_y += get_metrics().canvas.h; changed(); return;
        case KEY_PAGE_DOWN: pan_y -= get_metrics().canvas.h; changed(); return;
    }
    if (drag.kind != DRAG_NONE)
        return;

    switch (key) {
        case '+': case '=': command(CMD_ZOOM_IN);     return;
        case '-': case '_': command(CMD_ZOOM_OUT);    return;
        case '1':           command(CMD_ZOOM_ACTUAL); return;
        case '0':           command(CMD_ZOOM_FIT);    return;
        case '[':           set_brush(brush - 1);     return;
        case ']':           set_brush(brush + 1);     return;
    }
    if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
        char letter = (char)(key | 0x20);
        if (letter == 'g') { command(CMD_GRID);  return; }
        if (letter == 'c') { command(CMD_COLOR); return; }
        for (int i = 0; i < TOOL_COUNT; i++)
            if (tool_keys[i] == letter)
                set_tool((tool_t)i);
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
        case WM_EVENT_KEY:          main_key(ev);                    break;
        case WM_EVENT_MOUSE_DOWN:   main_mouse_down(ev);             break;
        case WM_EVENT_MOUSE_MOVE:   main_mouse_move(ev);             break;
        case WM_EVENT_CONTEXT_MENU: show_tool_menu(ev->x, ev->y);    break;
        case WM_EVENT_MENU_COMMAND: command(ev->param);              break;
        case WM_EVENT_CLOSE:        ask_then(THEN_EXIT);             break;

        case WM_EVENT_MOUSE_UP:
            if (drag.kind != DRAG_NONE && ev->button == drag.button) {
                drag.kind = DRAG_NONE;
                main_dirty = true;
            }
            break;

        case WM_EVENT_MOUSE_LEAVE:
            if (hover_in) {
                hover_in = false;
                status_dirty = true;
            }
            break;

        case WM_EVENT_FOCUS_LOST:   // A stroke can't go on without the mouse
            drag.kind = DRAG_NONE;
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
// Dialogs: "save changes?", the size and color prompts, and the file dialog
// (os/filedlg.h) for Open and Save as
//

static int dialog_buttons(const char** labels) {
    switch (dlg.kind) {
        case DLG_CONFIRM:
            labels[0] = "Save";
            labels[1] = "Don't save";
            labels[2] = "Cancel";
            return 3;
        case DLG_NEW:     labels[0] = "Create"; break;
        default:          labels[0] = "OK";     break;
    }
    labels[1] = "Cancel";
    return 2;
}

static const char* dialog_prompt(void) {
    switch (dlg.kind) {
        case DLG_NEW:     return "New image, width and height (1 - 2048):";
        case DLG_RESIZE:  return "Canvas size, width and height (1 - 2048):";
        default:          return "";
    }
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

static gfx_rect_t field_rect(int i) {
    int32_t w, h;
    wm_window_get_size(dlg.win, &w, &h);
    int32_t full = w - 2 * DLG_MARGIN;
    if (dlg.kind == DLG_COLOR)
        return gfx_rect(DLG_MARGIN + 32, PICK_FIELD_Y, 100, 18);
    if (dlg.fields < 2)
        return gfx_rect(DLG_MARGIN, 28, full, 18);
    int32_t fw = (full - FIELD_GAP) / 2;
    return gfx_rect(DLG_MARGIN + i * (fw + FIELD_GAP), 28, fw, 18);
}

static gfx_rect_t preview_rect(void) {
    gfx_rect_t f = field_rect(0);
    return gfx_rect(f.x + f.w + 12, f.y + 1, 56, f.h - 2);
}

static gfx_rect_t basic_rect(int i) {
    return gfx_rect(DLG_MARGIN + (i % PICK_COLS) * (PICK_CELL + PICK_CELL_GAP),
                    PICK_TOP + (i / PICK_COLS) * (PICK_CELL + PICK_CELL_GAP), PICK_CELL, PICK_CELL);
}

// The picker's color, from its hue, saturation and value. Black at the
// bottom of the strip stays visible, as the palette's does.
static uint32_t picker_color(void) {
    uint32_t c = (dlg.alpha << 24) | hsv_to_rgb(dlg.hue, dlg.sat, dlg.val);
    return (c == SEE_THROUGH) ? GFX_RGB(1, 1, 1) : c;
}

// The hex field shows c, the field and the strip follow it
static void picker_set(uint32_t c, bool text) {
    if (text) {
        char hex[16];
        if (GFX_ALPHA(c) == 0xFF)
            snprintf(hex, sizeof(hex), "%06X", (unsigned)(c & 0xFFFFFFu));
        else
            snprintf(hex, sizeof(hex), "%08X", (unsigned)c);
        field_set(&dlg.field[0], hex);
    }
    dlg.alpha = GFX_ALPHA(c);
    rgb_to_hsv(c, &dlg.hue, &dlg.sat, &dlg.val);
    dlg.dirty = true;
}

// A point on the field or the strip (clamped, so a drag can leave them)
static void picker_drag(int32_t x, int32_t y) {
    int32_t ry = y - PICK_TOP;
    if (ry < 0) ry = 0;
    if (ry > PICK_HS_H - 1) ry = PICK_HS_H - 1;
    int32_t level = 255 - ry * 255 / (PICK_HS_H - 1);
    if (dlg.pick_drag == 1) {
        int32_t rx = x - PICK_HS_X;
        if (rx < 0) rx = 0;
        if (rx > PICK_HS_W - 1) rx = PICK_HS_W - 1;
        dlg.hue = rx * 359 / (PICK_HS_W - 1);
        dlg.sat = level;
        if (dlg.val == 0)
            dlg.val = 255;          // Else the whole field would pick black
    } else {
        dlg.val = level;
    }
    if (dlg.alpha == 0)
        dlg.alpha = 0xFF;           // A fully clear color would show nothing
    int32_t h = dlg.hue, s = dlg.sat, v = dlg.val;
    picker_set(picker_color(), true);
    dlg.hue = h; dlg.sat = s; dlg.val = v;      // Not rounded through RGB
}

// True: the press was on the picker
static bool picker_down(int32_t x, int32_t y) {
    for (int i = 0; i < PICK_COLS * PICK_ROWS; i++) {
        gfx_rect_t r = basic_rect(i);
        r.x -= PICK_CELL_GAP / 2; r.y -= PICK_CELL_GAP / 2;
        r.w += PICK_CELL_GAP;     r.h += PICK_CELL_GAP;
        if (point_in(&r, x, y)) {
            picker_set(basic_colors[i], true);
            return true;
        }
    }
    gfx_rect_t hs = gfx_rect(PICK_HS_X, PICK_TOP, PICK_HS_W, PICK_HS_H);
    gfx_rect_t vs = gfx_rect(PICK_V_X, PICK_TOP, PICK_V_W + 6, PICK_HS_H);
    if (point_in(&hs, x, y))      dlg.pick_drag = 1;
    else if (point_in(&vs, x, y)) dlg.pick_drag = 2;
    else                          return false;
    picker_drag(x, y);
    return true;
}

static void draw_picker(gfx_surface_t* s) {
    int32_t lh = gfx_line_height(s);
    uint32_t c = 0;
    bool valid = parse_color(dlg.field[0].text, &c);

    gfx_draw_text(s, DLG_MARGIN, 10, "Basic colors", theme.client.text);
    for (int i = 0; i < PICK_COLS * PICK_ROWS; i++) {
        gfx_rect_t r = basic_rect(i);
        draw_swatch(s, &r, basic_colors[i], valid && basic_colors[i] == c);
    }

    gfx_draw_text(s, PICK_HS_X, 10, "Any color", theme.client.text);
    if (!pick_field_made) {
        for (int32_t y = 0; y < PICK_HS_H; y++)
            for (int32_t x = 0; x < PICK_HS_W; x++)
                pick_field[y * PICK_HS_W + x] = 0xFF000000u |
                    hsv_to_rgb(x * 359 / (PICK_HS_W - 1), 255 - y * 255 / (PICK_HS_H - 1), 255);
        pick_field_made = true;
    }
    gfx_draw_pixels(s, pick_field, PICK_HS_W, PICK_HS_H, 0, PICK_HS_X, PICK_TOP);
    gfx_draw_rect(s, PICK_HS_X - 1, PICK_TOP - 1, PICK_HS_W + 2, PICK_HS_H + 2, theme.button.border);

    // Crosshair on the hue and saturation, in black or white, whichever shows
    int32_t mx = PICK_HS_X + dlg.hue * (PICK_HS_W - 1) / 359;
    int32_t my = PICK_TOP + (255 - dlg.sat) * (PICK_HS_H - 1) / 255;
    gfx_color_t mark = (dlg.sat < 96 && (dlg.hue < 200 || dlg.hue > 280)) ? GFX_COLOR_BLACK : GFX_COLOR_WHITE;
    gfx_rect_t hs = gfx_rect(PICK_HS_X, PICK_TOP, PICK_HS_W, PICK_HS_H);
    gfx_save(s);
    gfx_set_clip(s, &hs);
    gfx_draw_hline(s, mx - 6, my, 4, mark);
    gfx_draw_hline(s, mx + 3, my, 4, mark);
    gfx_draw_vline(s, mx, my - 6, 4, mark);
    gfx_draw_vline(s, mx, my + 3, 4, mark);
    gfx_restore(s);

    // The brightness strip for this hue and saturation, a marker beside it
    for (int32_t y = 0; y < PICK_HS_H; y++)
        gfx_draw_hline(s, PICK_V_X, PICK_TOP + y, PICK_V_W,
                       0xFF000000u | hsv_to_rgb(dlg.hue, dlg.sat, 255 - y * 255 / (PICK_HS_H - 1)));
    gfx_draw_rect(s, PICK_V_X - 1, PICK_TOP - 1, PICK_V_W + 2, PICK_HS_H + 2, theme.button.border);
    int32_t vy = PICK_TOP + (255 - dlg.val) * (PICK_HS_H - 1) / 255;
    for (int32_t i = 0; i < 4; i++)
        gfx_draw_vline(s, PICK_V_X + PICK_V_W + 2 + i, vy - 3 + i, 7 - 2 * i, theme.client.text);

    gfx_rect_t f = field_rect(0);
    gfx_draw_text(s, DLG_MARGIN, f.y + (f.h - lh) / 2, "Hex", theme.client.text);
    draw_field(s, 0);
    gfx_rect_t p = preview_rect();
    if (valid)
        draw_swatch(s, &p, c, false);
    else
        gfx_draw_rect(s, p.x - 1, p.y - 1, p.w + 2, p.h + 2, theme.button.border);
    gfx_draw_text(s, p.x + p.w + 10, f.y + (f.h - lh) / 2, valid ? "" : "RRGGBB?", theme.client.text_soft);
}

static void dialog_close(void) {
    if (dlg.win != 0)
        wm_window_destroy(dlg.win);
    dlg.win = 0;
    dlg.kind = DLG_NONE;
    if (main_win != 0)
        wm_window_focus(main_win);
}

static void field_set(field_t* f, const char* text) {
    strlcpy(f->text, text, sizeof(f->text));
    f->len = (uint32_t)strlen(f->text);
    f->cursor = f->len;
    f->scroll = 0;
}

// The fields' first text
static void dialog_prefill(dlg_kind_t kind) {
    char text[PATH_MAX];
    switch (kind) {
        case DLG_NEW:
            dlg.fields = 2;
            snprintf(text, sizeof(text), "%d", NEW_W);
            field_set(&dlg.field[0], text);
            snprintf(text, sizeof(text), "%d", NEW_H);
            field_set(&dlg.field[1], text);
            return;

        case DLG_RESIZE:
            dlg.fields = 2;
            snprintf(text, sizeof(text), "%d", (int)doc.w);
            field_set(&dlg.field[0], text);
            snprintf(text, sizeof(text), "%d", (int)doc.h);
            field_set(&dlg.field[1], text);
            return;

        case DLG_COLOR:
            dlg.fields = 1;
            dlg.hue = 0;
            picker_set(color, true);
            return;

        default:
            break;
    }
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

static void draw_field(gfx_surface_t* s, int i) {
    field_t* fld = &dlg.field[i];
    int32_t lh = gfx_line_height(s), cw = gfx_char_width(s);
    bool active = (i == dlg.active);

    gfx_rect_t f = field_rect(i);
    gfx_fill_rect(s, f.x, f.y, f.w, f.h, theme.edit.background);
    gfx_draw_rect(s, f.x, f.y, f.w, f.h,
                  (active && dlg.fields > 1) ? theme.button.border_active : theme.edit.border);

    uint32_t visible = (uint32_t)((f.w - 8) / cw);
    if (visible < 1) visible = 1;
    if (fld->cursor < fld->scroll) fld->scroll = fld->cursor;
    if (fld->cursor > fld->scroll + visible) fld->scroll = fld->cursor - visible;

    gfx_rect_t inner = gfx_rect(f.x + 2, f.y + 2, f.w - 4, f.h - 4);
    gfx_save(s);
    gfx_set_clip(s, &inner);
    int32_t ty = f.y + (f.h - lh) / 2;
    gfx_draw_text_n(s, f.x + 4, ty, fld->text + fld->scroll, fld->len - fld->scroll, theme.edit.text);
    if (active)
        gfx_draw_vline(s, f.x + 4 + (int32_t)(fld->cursor - fld->scroll) * cw, ty - 1, lh + 2,
                       theme.edit.cursor);
    gfx_restore(s);
}

static void draw_dialog(void) {
    dlg.dirty = false;
    gfx_surface_t* s = wm_window_surface(dlg.win);
    if (s == NULL)
        return;
    int32_t lh = gfx_line_height(s);

    gfx_clear(s, theme.client.background);

    if (dlg.kind == DLG_COLOR) {
        draw_picker(s);
    } else if (dlg.kind == DLG_CONFIRM) {
        gfx_draw_text(s, DLG_MARGIN, 14, dlg.message, theme.client.text);
        gfx_draw_text(s, DLG_MARGIN, 14 + lh + 6, "Changes are lost if you don't save them.",
                      theme.client.text_soft);
    } else {
        gfx_draw_text(s, DLG_MARGIN, 12, dialog_prompt(), theme.client.text);
        for (int i = 0; i < dlg.fields; i++)
            draw_field(s, i);
        if (dlg.fields == 2) {
            gfx_rect_t f = field_rect(0);
            gfx_draw_text(s, f.x + f.w + (FIELD_GAP - gfx_char_width(s)) / 2, f.y + (f.h - lh) / 2,
                          "x", theme.client.text_soft);
        }
    }

    const char* labels[3];
    int count = dialog_buttons(labels);
    int focus = (dlg.kind == DLG_CONFIRM) ? dlg.focus : 0;     // Prompts: Enter = the first
    for (int i = 0; i < count; i++) {
        gfx_rect_t r = button_rect(i, count);
        draw_button(s, &r, labels[i], i == focus, i == dlg.pressed);
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

    if (kind == DLG_CONFIRM) {
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
    } else {
        dialog_prefill(kind);
    }

    int32_t w = (kind == DLG_COLOR) ? PICK_W : DLG_W;
    int32_t h = (kind == DLG_CONFIRM) ? DLG_CONFIRM_H : (kind == DLG_COLOR) ? PICK_H : DLG_FIELD_H;
    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title  = (kind == DLG_COLOR) ? "Pick a color" : APP_NAME;
    d.style  = WM_STYLE_NOMINIMIZE;         // A question: answered, not put away
    d.x      = (sw > w) ? (sw - w) / 2 : WM_POS_DEFAULT;
    d.y      = (sh > h) ? (sh - h) / 2 - 40 : WM_POS_DEFAULT;
    d.width  = w;
    d.height = h;

    dlg.win = wm_window_create(&d);
    if (dlg.win == 0) {
        // Couldn't ask: keep the image rather than drop it
        notify_error(APP_NAME, kind == DLG_CONFIRM ? "Save your changes before closing" : strerror(errno));
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

static void size_accept(void) {
    int32_t w, h;
    if (!parse_dim(dlg.field[0].text, &w) || !parse_dim(dlg.field[1].text, &h)) {
        notify_error(APP_NAME, "Width and height go from 1 to 2048");
        return;
    }
    dlg_kind_t kind = dlg.kind;
    dialog_close();
    if (kind == DLG_NEW)
        new_image(w, h);
    else
        resize_canvas(w, h);
    changed();
}

static void color_accept(void) {
    uint32_t c;
    if (!parse_color(dlg.field[0].text, &c)) {
        notify_error(APP_NAME, "Type 6 hex digits (RRGGBB) or 8 (AARRGGBB)");
        return;
    }
    dialog_close();
    use_color(c);
}

static void field_accept(void) {
    if (dlg.field[0].len == 0)
        return;

    switch (dlg.kind) {
        case DLG_NEW:
        case DLG_RESIZE:  size_accept();    break;
        case DLG_COLOR:   color_accept();   break;
        default:                            break;
    }
}

static void dialog_activate(int button) {
    if (dlg.kind == DLG_CONFIRM)
        confirm_answer(button);
    else if (button == 0)
        field_accept();
    else
        dialog_close();
}

// What a field takes: digits for sizes, hex digits for a color
static bool field_allows(char ch) {
    switch (dlg.kind) {
        case DLG_NEW:
        case DLG_RESIZE:
            return ch >= '0' && ch <= '9';
        case DLG_COLOR:
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || ch == '#';
        default:
            return ch >= 32 && ch < 127;
    }
}

static void field_insert(const char* text, uint32_t length) {
    field_t* f = &dlg.field[dlg.active];
    uint32_t room = (f->len < 9 ? 9 - f->len : 0);     // A size or a color is short
    if (length > room) length = room;
    for (uint32_t i = 0; i < length; i++)
        if (!field_allows(text[i]))
            return;
    if (length == 0)
        return;
    memmove(f->text + f->cursor + length, f->text + f->cursor, f->len - f->cursor + 1);
    memcpy(f->text + f->cursor, text, length);
    f->len += length;
    f->cursor += length;
}

// Text or a copied file: its first line
static void field_paste(void) {
    wm_clip_info_t info;
    char* data = (char*)wm_clip_get(&info);
    if (data == NULL)
        return;
    if (info.type == WM_CLIP_TEXT || info.type == WM_CLIP_FILES) {
        uint32_t n = 0;
        while (n < info.size && data[n] != '\n' && data[n] != '\r' && data[n] != '\0') n++;
        field_insert(data, n);
    }
    free(data);
}

static void dialog_key(const wm_event_t* ev) {
    uint16_t key = ev->key;

    if (key == KEY_ESCAPE) {
        dialog_close();
        return;
    }

    if (dlg.kind == DLG_CONFIRM) {
        switch (key) {
            case KEY_LEFT:  dlg.focus = (dlg.focus + 2) % 3; break;
            case KEY_RIGHT:
            case KEY_TAB:   dlg.focus = (dlg.focus + 1) % 3; break;
            case KEY_ENTER:
            case ' ':       dialog_activate(dlg.focus);      return;
            case 's': case 'S':                     dialog_activate(0); return;
            case 'n': case 'N': case 'd': case 'D': dialog_activate(1); return;
        }
        dlg.dirty = true;
        return;
    }

    field_t* f = &dlg.field[dlg.active];
    switch (key) {
        case KEY_ENTER:
            field_accept();
            return;
        case KEY_TAB:
            if (dlg.fields > 1)
                dlg.active = (dlg.active + 1) % dlg.fields;
            break;
        case KEY_LEFT:  if (f->cursor > 0) f->cursor--;       break;
        case KEY_RIGHT: if (f->cursor < f->len) f->cursor++;  break;
        case KEY_HOME:  f->cursor = 0;                        break;
        case KEY_END:   f->cursor = f->len;                   break;
        case KEY_BACKSPACE:
            if (f->cursor > 0) {
                memmove(f->text + f->cursor - 1, f->text + f->cursor, f->len - f->cursor + 1);
                f->cursor--;
                f->len--;
            }
            break;
        case KEY_DELETE:
            if (f->cursor < f->len) {
                memmove(f->text + f->cursor, f->text + f->cursor + 1, f->len - f->cursor);
                f->len--;
            }
            break;
        default:
            if (ev->mods & INPUT_MOD_CTRL) {
                if ((key | 0x20) == 'v' || key == 22) field_paste();
            } else if (key >= 32 && key < 127) {
                char ch = (char)key;
                field_insert(&ch, 1);
            }
            break;
    }
    uint32_t c;
    if (dlg.kind == DLG_COLOR && parse_color(f->text, &c))
        picker_set(c, false);       // Typed in: the field and the strip follow
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
            if (hit >= 0) {
                dlg.pressed = hit;
                if (dlg.kind == DLG_CONFIRM) dlg.focus = hit;
            } else if (dlg.kind == DLG_COLOR && picker_down(ev->x, ev->y)) {
                break;
            } else {
                gfx_surface_t* s = wm_window_surface(dlg.win);
                for (int i = 0; s != NULL && i < dlg.fields; i++) {
                    gfx_rect_t fr = field_rect(i);
                    if (!point_in(&fr, ev->x, ev->y))
                        continue;
                    field_t* f = &dlg.field[i];
                    int32_t cw = gfx_char_width(s);
                    int32_t cell = (ev->x - fr.x - 4 + cw / 2) / cw;
                    uint32_t pos = f->scroll + (uint32_t)(cell > 0 ? cell : 0);
                    f->cursor = (pos > f->len) ? f->len : pos;
                    dlg.active = i;
                }
            }
            dlg.dirty = true;
            break;
        }

        case WM_EVENT_MOUSE_UP: {
            dlg.pick_drag = 0;
            if (dlg.pressed < 0)
                break;
            int pressed = dlg.pressed;
            dlg.pressed = -1;
            dlg.dirty = true;
            if (button_at(ev->x, ev->y) == pressed)     // Released over the same button
                dialog_activate(pressed);
            break;
        }

        case WM_EVENT_MOUSE_MOVE:
            if (dlg.pick_drag != 0) {
                if (ev->buttons & INPUT_BTN_LEFT)
                    picker_drag(ev->x, ev->y);
                else
                    dlg.pick_drag = 0;
            }
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

    // On failure: told why, starts Untitled
    if (argc < 2 || !open_document(argv[1], true)) {
        if (!new_image(NEW_W, NEW_H)) {
            wm_disconnect();
            return 1;
        }
    }

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
    zoom_fit();                             // Now against the real window size
    draw_main();
    wm_window_focus(main_win);

    // Events are handled as they come; the windows are repainted once the
    // queue runs dry, so a burst of mouse moves costs one frame
    wm_event_t ev;
    while (running) {
        bool dirty = main_dirty || status_dirty || (dlg.win != 0 && dlg.dirty) || filedlg_dirty(fdlg);
        int r = wm_wait_event(&ev, dirty ? 0 : -1);
        if (r < 0)
            break;                          // The dwm went away
        if (r == 0) {
            if (main_dirty) draw_main();
            else if (status_dirty) draw_status_only();
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
    history_clear();
    free(doc.px);
    free(view);
    free(col_map);
    return 0;
}
