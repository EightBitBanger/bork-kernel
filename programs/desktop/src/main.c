/*
 * desktop: the desktop's background, a color or a wallpaper.
 *
 *     desktop                                  the window
 *     desktop -c <#RRGGBB>                     a color background, no window
 *     desktop -w <path> [-s <style>]           a wallpaper, no window
 *
 * Installed as <home>/sys/settings/desktop, in the settings folder the
 * start menu's "Settings" opens. The window shows what the registry has
 * now (os/desktop.h, hkey_user/control/desktop) and lets the user pick:
 *
 *   Background   Color or Wallpaper (a drop-down)
 *   Color        a swatch, the "#RRGGBB" box to type in, and a palette to
 *                click. It is also what shows around a wallpaper that
 *                doesn't cover the screen (center, fit, tile edges).
 *   Wallpaper    a path box and "..." to browse for the file (os/filedlg.h);
 *                picking a file switches Background to Wallpaper
 *   Style        Center, Tile, Stretch, Fit or Fill
 *
 * Apply writes the values (os_reg_set; the kernel saves <home>/sys/hku with
 * its own clearance, so no admin is needed) and asks the desktop to read
 * them again (wm_settings_changed), even if saving the hive failed; OK does the same and
 * closes, Cancel / Escape / the close box close without writing. Enter
 * applies; Tab moves between the two boxes.
 *
 * Whether the desktop can read the wallpaper file is the desktop's to say:
 * if it can't, its notification says why and the color shows. It reads
 * PNG, JPEG, BMP, GIF, TGA, PSD, PIC and PNM (stb_image), and the system's
 * sprite resources.
 *
 * With arguments it writes them and tells the desktop, without a window:
 * "-c" sets a color background, "-w" a wallpaper (with "-s" its style;
 * otherwise the style stays as it was). Exit code: 0 done, 1 writing
 * failed, 2 bad arguments.
 *
 * Needs SEC_FILE (reading and writing the registry, browsing), SEC_GFX and
 * SEC_IPC: the desktop's launch mask has them. No admin: the settings are
 * the user's own.
 *
 * With nothing in the registry yet the window shows the desktop's default:
 * the <home>/sys/wallpaper/bluefade wallpaper, stretched (os/desktop.h).
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/desktop.h>
#include <os/filedlg.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/registry.h>
#include <os/ui.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

// Layout (client coordinates)
#define WIN_W               420
#define WIN_H               292
#define PAD                 14
#define FIELD_X             106
#define FIELD_H             22
#define ROW_SHOW            14
#define ROW_COLOR           48
#define ROW_PALETTE         78
#define ROW_WALLPAPER       142
#define ROW_STYLE           174
#define ROW_STATUS          212
#define SWATCH              22          // The current color's swatch
#define CHIP                20          // A palette color
#define CHIP_GAP            5
#define CHIPS_PER_ROW       10
#define HEX_W               90
#define BROWSE_W            30
#define BUTTON_W            72
#define BUTTON_H            24

// Menu command ids
#define CMD_SHOW            100
#define CMD_STYLE           200

typedef enum { SHOW_COLOR = 0, SHOW_WALLPAPER } show_t;
typedef enum { FOCUS_NONE = 0, FOCUS_HEX, FOCUS_PATH } focus_t;

static const struct {
    const char* value;          // In the registry
    const char* label;
} shows[] = {
    { OS_DESKTOP_SHOW_COLOR,     "Color" },
    { OS_DESKTOP_SHOW_WALLPAPER, "Wallpaper" },
};
#define SHOW_COUNT  (sizeof(shows) / sizeof(shows[0]))

static const struct {
    const char* value;
    const char* label;
} styles[] = {
    { OS_DESKTOP_STYLE_FILL,    "Fill" },
    { OS_DESKTOP_STYLE_FIT,     "Fit" },
    { OS_DESKTOP_STYLE_STRETCH, "Stretch" },
    { OS_DESKTOP_STYLE_CENTER,  "Center" },
    { OS_DESKTOP_STYLE_TILE,    "Tile" },
};
#define STYLE_COUNT (sizeof(styles) / sizeof(styles[0]))
#define STYLE_DEFAULT 2             // Stretch (OS_DESKTOP_DEFAULT_STYLE)

static const uint32_t palette[] = {
    0xFF020730, 0xFF0B1F4D, 0xFF1E4F8A, 0xFF2D7DB3, 0xFF0F5E5E,
    0xFF0E4D1E, 0xFF3A6B12, 0xFF5B3A0E, 0xFF6B1414, 0xFF4A1458,
    0xFF000000, 0xFF1A1A1A, 0xFF333340, 0xFF555566, 0xFF808090,
    0xFF2B2B1A, 0xFF3D2B1F, 0xFF103040, 0xFF301030, 0xFF203020,
};
#define PALETTE_COUNT (sizeof(palette) / sizeof(palette[0]))

static show_t     show = SHOW_COLOR;
static uint32_t   color;
static size_t     style = STYLE_DEFAULT;
static ui_edit_t  hex_box;
static ui_edit_t  path_box;
static focus_t    focus = FOCUS_NONE;
static bool       dragging;         // The left button went down in the focused box
static bool       changed;          // Something differs from what was last written
static char       status[160];
static bool       status_is_error;

static wm_window_t win;
static wm_theme_t  theme;
static int32_t     cw = 6, lh = 8;
static filedlg_t*  browse = NULL;

//
// Helpers
//

static void set_status(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(bool error, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    status_is_error = error;
}

static size_t find_style(const char* value) {
    for (size_t i = 0; i < STYLE_COUNT; i++) {
        if (strcmp(styles[i].value, value) == 0)
            return i;
    }
    return STYLE_DEFAULT;
}

static bool in_rect(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void set_hex_text(uint32_t c) {
    char text[OS_DESKTOP_COLOR_TEXT_MAX];
    os_desktop_format_color(c, text);
    ui_edit_init(&hex_box, text, OS_DESKTOP_COLOR_TEXT_MAX - 1, 0);
}

static void set_path_text(const char* path) {
    ui_edit_init(&path_box, path, PATH_MAX - 1, 0);
}

// What the registry has now; the theme's color when it has none
static void read_settings(void) {
    char text[PATH_MAX];

    color = theme.desktop.background | 0xFF000000u;
    if (os_reg_get_string(OS_DESKTOP_KEY, OS_DESKTOP_COLOR, text, sizeof(text)) == 0)
        os_desktop_parse_color(text, &color);
    set_hex_text(color);

    // Nothing set yet: what the desktop shows then (os/desktop.h)
    if (os_reg_get_string(OS_DESKTOP_KEY, OS_DESKTOP_BACKGROUND, text, sizeof(text)) < 0)
        strlcpy(text, OS_DESKTOP_DEFAULT_SHOW, sizeof(text));
    show = (strcmp(text, OS_DESKTOP_SHOW_WALLPAPER) == 0) ? SHOW_WALLPAPER : SHOW_COLOR;

    style = STYLE_DEFAULT;
    if (os_reg_get_string(OS_DESKTOP_KEY, OS_DESKTOP_STYLE, text, sizeof(text)) == 0)
        style = find_style(text);

    if (os_reg_get_string(OS_DESKTOP_KEY, OS_DESKTOP_WALLPAPER, text, sizeof(text)) < 0 &&
        os_syspath(OS_DESKTOP_DEFAULT_WALLPAPER, text, sizeof(text)) == NULL)
        text[0] = '\0';
    set_path_text(text);
    changed = false;
}

// `in` made absolute (against the cwd) into out. False if it doesn't fit.
static bool absolute_path(const char* in, char* out, size_t size) {
    if (in[0] == '/')
        return strlcpy(out, in, size) < size;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        return false;
    int n = snprintf(out, size, "%s%s%s", cwd, (cwd[strlen(cwd) - 1] == '/') ? "" : "/", in);
    return n >= 0 && (size_t)n < size;
}

static const char* write_error(int error) {
    switch (error) {
        case EPERM:  return "Not allowed to change settings";
        case EACCES: return "The registry's desktop key is read-only";
        case EIO:    return "Applied, but not saved to disk: lost on restart";
        case ENOSYS: return "This kernel can't change the registry (update it)";
        default:     return strerror(error);
    }
}

// One value. EIO (set, but the hive file wasn't saved) is remembered in
// *unsaved and not a failure: the value is live, so the rest still go in.
static bool write_value(const char* name, const char* value, bool* unsaved) {
    if (os_reg_set_string(OS_DESKTOP_KEY, name, value) == 0)
        return true;
    if (errno == EIO) {
        *unsaved = true;
        return true;
    }
    return false;
}

// Write everything to the registry and tell the desktop. 0, or -1 with
// errno. EIO means every value was set and the desktop told, but the hive
// could not be saved, so it lasts only until a restart.
static int write_settings(show_t s, uint32_t c, const char* path, const char* style_value) {
    char hex[OS_DESKTOP_COLOR_TEXT_MAX];
    os_desktop_format_color(c, hex);
    bool unsaved = false;
    bool ok = write_value(OS_DESKTOP_COLOR, hex, &unsaved) &&
              (path == NULL || write_value(OS_DESKTOP_WALLPAPER, path, &unsaved)) &&
              (style_value == NULL || write_value(OS_DESKTOP_STYLE, style_value, &unsaved)) &&
              write_value(OS_DESKTOP_BACKGROUND, shows[s].value, &unsaved);
    int saved_errno = errno;
    wm_settings_changed();          // Whatever went in shows. No desktop: nothing to tell
    if (!ok) {
        errno = saved_errno;
        return -1;
    }
    if (unsaved) {
        errno = EIO;
        return -1;
    }
    return 0;
}

//
// Checking and applying what the window shows
//

// The hex box's color into `color`. False (status says why) if it isn't one.
static bool take_hex(void) {
    uint32_t c;
    if (!os_desktop_parse_color(hex_box.text, &c)) {
        set_status(true, "\"%.20s\" is not a color: type #RRGGBB", hex_box.text);
        return false;
    }
    if (c != color)
        changed = true;
    color = c;
    return true;
}

// The path box, checked (absolute, a file) into out. "" is fine with the
// color showing. False with status set.
static bool take_path(char* out, size_t size) {
    out[0] = '\0';
    if (path_box.text[0] == '\0') {
        if (show == SHOW_WALLPAPER) {
            set_status(true, "Pick a wallpaper file, or show a color");
            return false;
        }
        return true;
    }
    if (!absolute_path(path_box.text, out, size)) {
        set_status(true, "The wallpaper's path is too long");
        return false;
    }
    struct stat st;
    if (show == SHOW_WALLPAPER) {
        if (stat(out, &st) < 0) {
            set_status(true, "%.60s: %s", out, strerror(errno));
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            set_status(true, "%.60s is a folder, not a picture", out);
            return false;
        }
    }
    return true;
}

static bool apply(void) {
    char path[PATH_MAX];
    if (!take_hex() || !take_path(path, sizeof(path)))
        return false;
    set_path_text(path);            // As written: absolute
    if (write_settings(show, color, path, styles[style].value) < 0) {
        bool shown = (errno == EIO);    // Live, only not saved
        set_status(true, "%s", write_error(errno));
        if (shown)
            changed = false;
        return shown;
    }
    changed = false;
    set_status(false, show == SHOW_COLOR ? "Applied: the desktop shows the color"
                                         : "Applied (the desktop says if it can't read the picture)");
    return true;
}

//
// Drawing
//

static gfx_rect_t field_rect(int32_t row) {
    return gfx_rect(FIELD_X, row, WIN_W - FIELD_X - PAD, FIELD_H);
}

static gfx_rect_t show_rect(void) {
    return gfx_rect(FIELD_X, ROW_SHOW, 140, FIELD_H);
}

static gfx_rect_t style_rect(void) {
    return gfx_rect(FIELD_X, ROW_STYLE, 140, FIELD_H);
}

static gfx_rect_t swatch_rect(void) {
    return gfx_rect(FIELD_X, ROW_COLOR, SWATCH, SWATCH);
}

static gfx_rect_t hex_rect(void) {
    return gfx_rect(FIELD_X + SWATCH + 8, ROW_COLOR, HEX_W, FIELD_H);
}

static gfx_rect_t chip_rect(size_t i) {
    int32_t col = (int32_t)(i % CHIPS_PER_ROW), row = (int32_t)(i / CHIPS_PER_ROW);
    return gfx_rect(FIELD_X + col * (CHIP + CHIP_GAP), ROW_PALETTE + row * (CHIP + CHIP_GAP), CHIP, CHIP);
}

static gfx_rect_t browse_rect(void) {
    gfx_rect_t f = field_rect(ROW_WALLPAPER);
    return gfx_rect(f.x + f.w - BROWSE_W, f.y, BROWSE_W, FIELD_H);
}

static gfx_rect_t path_rect(void) {
    gfx_rect_t f = field_rect(ROW_WALLPAPER);
    return gfx_rect(f.x, f.y, f.w - BROWSE_W - 6, FIELD_H);
}

static gfx_rect_t cancel_rect(void) {
    return gfx_rect(WIN_W - PAD - BUTTON_W, WIN_H - PAD - BUTTON_H, BUTTON_W, BUTTON_H);
}

static gfx_rect_t ok_rect(void) {
    gfx_rect_t r = cancel_rect();
    r.x -= BUTTON_W + 8;
    return r;
}

static gfx_rect_t apply_rect(void) {
    gfx_rect_t r = ok_rect();
    r.x -= BUTTON_W + 8;
    return r;
}

// Place a box on its rectangle (it sizes itself to the font)
static void place_box(ui_edit_t* e, gfx_rect_t r, gfx_surface_t* s) {
    ui_edit_place(e, r.x + r.w / 2, gfx_text_y(s, r.y, r.h), r.w, r.w, gfx_rect(0, 0, WIN_W, WIN_H));
}

static void draw_button(gfx_surface_t* s, gfx_rect_t b, const char* label, uint32_t color_text) {
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, theme.button.fill);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, theme.button.border);
    gfx_draw_text(s, b.x + (b.w - (int32_t)strlen(label) * cw) / 2, gfx_text_y(s, b.y, b.h), label, color_text);
}

static void draw_label(gfx_surface_t* s, int32_t row, const char* label, bool muted) {
    gfx_draw_text(s, PAD, gfx_text_y(s, row, FIELD_H), label, muted ? theme.client.text_muted : theme.client.text);
}

// A drop-down: the value and a "v" at the right
static void draw_dropdown(gfx_surface_t* s, gfx_rect_t f, const char* value) {
    gfx_fill_rect(s, f.x, f.y, f.w, f.h, theme.edit.background);
    gfx_draw_rect(s, f.x, f.y, f.w, f.h, theme.edit.border);
    gfx_draw_text(s, f.x + 6, gfx_text_y(s, f.y, f.h), value, theme.edit.text);
    gfx_draw_text(s, f.x + f.w - cw - 6, gfx_text_y(s, f.y, f.h), "v", theme.client.text_muted);
}

static void draw_box(gfx_surface_t* s, const ui_edit_t* e, bool focused) {
    ui_edit_style_t st = {
        .background = theme.edit.background,
        .border     = focused ? theme.button.border_active : theme.edit.border,
        .text       = theme.edit.text,
        .cursor     = focused ? theme.edit.cursor : theme.edit.background,   // Hidden unfocused
        .selection  = focused ? theme.edit.selection : theme.edit.background,
    };
    ui_edit_paint(e, s, &st);
}

static void draw(void) {
    gfx_surface_t* s = wm_window_surface(win);
    if (s == NULL)
        return;
    cw = gfx_char_width(s);
    lh = gfx_line_height(s);
    gfx_clear(s, theme.client.background);

    // Background: Color / Wallpaper
    draw_label(s, ROW_SHOW, "Background", false);
    draw_dropdown(s, show_rect(), shows[show].label);

    // Color: swatch, box, palette
    draw_label(s, ROW_COLOR, "Color", false);
    gfx_rect_t sw = swatch_rect();
    uint32_t shown = color;
    os_desktop_parse_color(hex_box.text, &shown);      // What is typed, once it is a color
    gfx_fill_rect(s, sw.x, sw.y, sw.w, sw.h, shown);
    gfx_draw_rect(s, sw.x, sw.y, sw.w, sw.h, theme.edit.border);
    place_box(&hex_box, hex_rect(), s);
    draw_box(s, &hex_box, focus == FOCUS_HEX);

    for (size_t i = 0; i < PALETTE_COUNT; i++) {
        gfx_rect_t c = chip_rect(i);
        gfx_fill_rect(s, c.x, c.y, c.w, c.h, palette[i]);
        bool current = (palette[i] == shown);
        gfx_draw_rect(s, c.x - 1, c.y - 1, c.w + 2, c.h + 2,
                      current ? theme.client.accent : theme.edit.border);
        if (current)
            gfx_draw_rect(s, c.x, c.y, c.w, c.h, theme.client.accent);
    }

    // Wallpaper: path box and "..."
    bool wall = (show == SHOW_WALLPAPER);
    draw_label(s, ROW_WALLPAPER, "Wallpaper", !wall);
    place_box(&path_box, path_rect(), s);
    draw_box(s, &path_box, focus == FOCUS_PATH);
    draw_button(s, browse_rect(), "...", theme.button.text);

    draw_label(s, ROW_STYLE, "Style", !wall);
    draw_dropdown(s, style_rect(), styles[style].label);

    // A hint, or the last thing that happened
    if (status[0] != '\0') {
        gfx_draw_text(s, PAD, ROW_STATUS, status, status_is_error ? theme.client.text_danger : theme.client.text_soft);
    } else {
        gfx_draw_text(s, PAD, ROW_STATUS, wall ? "The color shows around a wallpaper that"
                                               : "Pick a color, or type one as #RRGGBB.",
                      theme.client.text_muted);
        if (wall)
            gfx_draw_text(s, PAD, ROW_STATUS + lh + 3, "doesn't cover the screen (Fit, Center).",
                          theme.client.text_muted);
    }

    draw_button(s, apply_rect(), "Apply", changed ? theme.button.text : theme.client.text_muted);
    draw_button(s, ok_rect(), "OK", theme.button.text);
    draw_button(s, cancel_rect(), "Cancel", theme.button.text);
    wm_window_commit(win, NULL);
}

//
// Menus
//

static void show_show_menu(void) {
    wm_menu_item_t items[SHOW_COUNT];
    for (size_t i = 0; i < SHOW_COUNT; i++) {
        items[i].label = shows[i].label;
        items[i].id    = CMD_SHOW + (uint32_t)i;
        items[i].flags = (i == (size_t)show) ? WM_MENU_CHECKED : 0;
    }
    gfx_rect_t f = show_rect();
    wm_menu_show(win, f.x, f.y + f.h, items, SHOW_COUNT);
}

static void show_style_menu(void) {
    wm_menu_item_t items[STYLE_COUNT];
    for (size_t i = 0; i < STYLE_COUNT; i++) {
        items[i].label = styles[i].label;
        items[i].id    = CMD_STYLE + (uint32_t)i;
        items[i].flags = (i == style) ? WM_MENU_CHECKED : 0;
    }
    gfx_rect_t f = style_rect();
    wm_menu_show(win, f.x, f.y + f.h, items, STYLE_COUNT);
}

static void menu_command(uint32_t id) {
    if (id >= CMD_SHOW && id < CMD_SHOW + SHOW_COUNT) {
        show_t s = (show_t)(id - CMD_SHOW);
        changed = changed || s != show;
        show = s;
    } else if (id >= CMD_STYLE && id < CMD_STYLE + STYLE_COUNT) {
        changed = changed || (id - CMD_STYLE) != style;
        style = id - CMD_STYLE;
    }
    status[0] = '\0';
    draw();
}

//
// Browsing for the wallpaper
//

static void open_browse(void) {
    if (browse != NULL) {
        wm_window_focus(filedlg_window(browse));
        return;
    }
    // Start where the current wallpaper is, else in the home folder
    char start[PATH_MAX] = "";
    if (path_box.text[0] != '\0')
        strlcpy(start, path_box.text, sizeof(start));
    else if (os_gethome(start, sizeof(start)) == NULL)
        start[0] = '\0';
    filedlg_desc_t d = { .mode = FILEDLG_OPEN, .title = "Pick a wallpaper", .path = start, .owner = win };
    browse = filedlg_open(&d);
    if (browse == NULL)
        set_status(true, "Can't browse: %s", strerror(errno));
}

static void browse_event(const wm_event_t* ev, bool* used) {
    *used = false;
    if (browse == NULL)
        return;
    switch (filedlg_event(browse, ev)) {
        case FILEDLG_ACCEPT:
            set_path_text(filedlg_path(browse));
            show = SHOW_WALLPAPER;      // Picking one means showing it
            changed = true;
            status[0] = '\0';
            filedlg_close(browse);
            browse = NULL;
            draw();
            *used = true;
            break;
        case FILEDLG_CANCEL:
            filedlg_close(browse);
            browse = NULL;
            *used = true;
            break;
        case FILEDLG_HANDLED:
            *used = true;
            break;
        case FILEDLG_IGNORED:
            break;
    }
}

//
// Input
//

static ui_edit_t* focused_box(void) {
    return focus == FOCUS_HEX ? &hex_box : focus == FOCUS_PATH ? &path_box : NULL;
}

// A key for the window. False: close it.
static bool key(const wm_event_t* ev) {
    if (ev->key == KEY_TAB) {
        focus = (focus == FOCUS_HEX) ? FOCUS_PATH : FOCUS_HEX;
        ui_edit_select_all(focused_box());
        draw();
        return true;
    }
    ui_edit_t* box = focused_box();
    if (box == NULL) {
        if (ev->key == KEY_ESCAPE)
            return false;
        if (ev->key == KEY_ENTER)
            apply();
        draw();
        return true;
    }
    char before[UI_EDIT_MAX];
    strlcpy(before, box->text, sizeof(before));
    switch (ui_edit_key(box, ev->key, ev->mods)) {
        case UI_EDIT_ACCEPT:
            apply();
            break;
        case UI_EDIT_CANCEL:
            return false;
        case UI_EDIT_HANDLED:
            if (strcmp(before, box->text) != 0) {
                changed = true;
                status[0] = '\0';
            }
            break;
        case UI_EDIT_IGNORED:
            break;
    }
    draw();
    return true;
}

// The left button went down. False: close the window.
static bool click(const wm_event_t* ev) {
    int32_t x = ev->x, y = ev->y;
    dragging = false;

    if (ui_edit_click(&hex_box, x, y, ev->mods, ev->clicks)) {
        focus = FOCUS_HEX;
        dragging = true;
    } else if (ui_edit_click(&path_box, x, y, ev->mods, ev->clicks)) {
        focus = FOCUS_PATH;
        dragging = true;
    } else {
        focus = FOCUS_NONE;
        if (in_rect(cancel_rect(), x, y))
            return false;
        if (in_rect(ok_rect(), x, y)) {
            if (!changed || apply())
                return false;
        } else if (in_rect(apply_rect(), x, y)) {
            apply();
        } else if (in_rect(show_rect(), x, y)) {
            show_show_menu();
        } else if (in_rect(style_rect(), x, y)) {
            show_style_menu();
        } else if (in_rect(browse_rect(), x, y)) {
            open_browse();
        } else {
            for (size_t i = 0; i < PALETTE_COUNT; i++) {
                if (in_rect(chip_rect(i), x, y)) {
                    changed = changed || palette[i] != color;
                    color = palette[i];
                    set_hex_text(color);
                    status[0] = '\0';
                    break;
                }
            }
        }
    }
    draw();
    return true;
}

//
// Main
//

static void default_theme(void) {
    theme.desktop.background = 0xFF020730;
    theme.client.background = 0xFF141420; theme.client.text = 0xFFFFFFFF;
    theme.client.text_soft = 0xFFCCCCDD;  theme.client.text_muted = 0xFF8888AA;
    theme.client.text_danger = 0xFFC00404; theme.client.accent = 0xFF04C004;
    theme.button.fill = 0xFF1C1C2A;       theme.button.border = 0xFF444466;
    theme.button.border_active = 0xFF04C004; theme.button.text = 0xFF3FFF3F;
    theme.edit.background = 0xFF202020;   theme.edit.border = 0xFF404040;
    theme.edit.text = 0xFFFFFFFF;         theme.edit.cursor = 0xFFF0F0F0;
    theme.edit.selection = 0xFF2E5C2E;
}

static int usage(void) {
    wm_notify("desktop", "Usage: desktop [-c #RRGGBB] [-w <wallpaper> [-s fill|fit|stretch|center|tile]]");
    return 2;
}

// desktop -c / -w / -s: write and tell the desktop, no window
static int run_arguments(int argc, char** argv) {
    const char* want_color = NULL;
    const char* want_path = NULL;
    const char* want_style = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            want_color = argv[++i];
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            want_path = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            want_style = argv[++i];
        else
            return usage();
    }

    read_settings();
    uint32_t c = color;
    if (want_color != NULL && !os_desktop_parse_color(want_color, &c))
        return usage();
    if (want_style != NULL) {
        size_t k = find_style(want_style);
        if (strcmp(styles[k].value, want_style) != 0)
            return usage();
    }
    if (want_path == NULL && want_style != NULL)
        return usage();

    char path[PATH_MAX];
    show_t s = (want_path != NULL) ? SHOW_WALLPAPER : SHOW_COLOR;
    if (want_path != NULL && !absolute_path(want_path, path, sizeof(path)))
        return usage();
    if (write_settings(s, c, want_path != NULL ? path : NULL, want_style) < 0) {
        wm_notify_ex(WM_NOTIFY_ERROR, "Settings not changed", write_error(errno), WM_NOTIFY_DEFAULT_MS);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
    if (argc > 1)
        return run_arguments(argc, argv);

    read_settings();

    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    wm_window_desc_t desc = {
        .title  = "Desktop background",
        .x      = (sw > WIN_W) ? (sw - WIN_W) / 2 : WM_POS_DEFAULT,
        .y      = (sh > WIN_H) ? (sh - WIN_H - 20) / 2 : WM_POS_DEFAULT,
        .width  = WIN_W,
        .height = WIN_H,
        .style  = 0,
    };
    win = wm_window_create(&desc);
    if (win == 0)
        return 1;
    draw();

    bool running = true;
    wm_event_t ev;
    while (running) {
        int r = wm_wait_event(&ev, (browse != NULL && filedlg_dirty(browse)) ? 0 : -1);
        if (r < 0)
            break;
        if (r == 0) {
            if (browse != NULL)
                filedlg_paint(browse);
            continue;
        }
        bool used;
        browse_event(&ev, &used);
        if (used)
            continue;
        if (ev.window != 0 && ev.window != win)
            continue;

        // While browsing, the dialog has the input
        if (browse != NULL && (ev.type == WM_EVENT_KEY || ev.type == WM_EVENT_MOUSE_DOWN)) {
            wm_window_focus(filedlg_window(browse));
            continue;
        }

        switch (ev.type) {
            case WM_EVENT_CLOSE:
            case WM_EVENT_DESTROYED:
                running = false;
                break;
            case WM_EVENT_KEY:
                running = key(&ev);
                break;
            case WM_EVENT_THEME_CHANGED:
                if (wm_get_theme(&theme) < 0)
                    default_theme();
                draw();
                break;
            case WM_EVENT_MENU_COMMAND:
                menu_command(ev.param);
                break;
            case WM_EVENT_MOUSE_DOWN:
                if (ev.button == INPUT_BTN_LEFT)
                    running = click(&ev);
                break;
            case WM_EVENT_MOUSE_MOVE:
                if (dragging && (ev.buttons & INPUT_BTN_LEFT) && focused_box() != NULL) {
                    ui_edit_drag(focused_box(), ev.x);
                    draw();
                }
                break;
            case WM_EVENT_MOUSE_UP:
                dragging = false;
                break;
            default:
                break;
        }
    }

    if (browse != NULL)
        filedlg_close(browse);
    wm_window_destroy(win);
    wm_disconnect();
    return 0;
}
