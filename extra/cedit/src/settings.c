/*
 * Settings: the editor's and the compiler's, kept per user in the registry
 * under REG_KEY (one value each), and the window that changes them.
 *
 * The window has two pages, Editor and Compiler, picked with the tabs at
 * its top (or Ctrl+Tab). Nothing changes until OK; Cancel, Escape or
 * closing the window drops the edits. Defaults puts the page shown back to
 * how cedit comes.
 *
 * Keys: Tab / Shift+Tab move between the controls, Space ticks a box or
 * picks the next choice, Left / Right pick a choice, Enter is OK.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/input.h>
#include <os/registry.h>
#include <os/ui.h>
#include "cedit.h"

#define REG_KEY         "hkey_user/software/cedit"

#define SW_W            420
#define SW_MARGIN       12
#define TAB_H           22
#define ROW_H           26
#define LABEL_W         120
#define CHECK_SIZE      12
#define SBTN_W          76
#define SBTN_H          20
#define SBTN_GAP        8

settings_t cfg;

//
// Defaults and the registry
//

static void defaults_editor(settings_t* c) {
    c->tab_width      = 4;
    c->use_spaces     = true;
    c->auto_indent    = true;
    c->brace_complete = true;
    c->highlight      = true;
    c->line_numbers   = true;
    c->eol            = EOL_KEEP;
}

static void defaults_compiler(settings_t* c) {
    c->cc_path[0]    = '\0';
    c->cc_options[0] = '\0';
    c->cc_libs[0]    = '\0';
    c->cc_output[0]  = '\0';
    c->cc_wall       = true;
}

static void get_u32(const char* name, uint32_t* out, uint32_t lo, uint32_t hi) {
    uint32_t v;
    if (os_reg_get(REG_KEY, name, &v, sizeof(v)) == (ssize_t)sizeof(v) && v >= lo && v <= hi)
        *out = v;
}

static void get_bool(const char* name, bool* out) {
    uint32_t v = *out ? 1 : 0;
    get_u32(name, &v, 0, 1);
    *out = (v != 0);
}

static void get_text(const char* name, char* out, size_t size) {
    char buf[CFG_TEXT_MAX];
    if (os_reg_get_string(REG_KEY, name, buf, sizeof(buf)) == 0)
        strlcpy(out, buf, size);
}

void settings_load(void) {
    memset(&cfg, 0, sizeof(cfg));
    defaults_editor(&cfg);
    defaults_compiler(&cfg);

    get_u32("tab_width", &cfg.tab_width, TAB_WIDTH_MIN, TAB_WIDTH_MAX);
    get_bool("use_spaces", &cfg.use_spaces);
    get_bool("auto_indent", &cfg.auto_indent);
    get_bool("brace_complete", &cfg.brace_complete);
    get_bool("highlight", &cfg.highlight);
    get_bool("line_numbers", &cfg.line_numbers);
    get_u32("eol", &cfg.eol, EOL_KEEP, EOL_CRLF);
    get_text("cc_path", cfg.cc_path, sizeof(cfg.cc_path));
    get_text("cc_options", cfg.cc_options, sizeof(cfg.cc_options));
    get_text("cc_libs", cfg.cc_libs, sizeof(cfg.cc_libs));
    get_text("cc_output", cfg.cc_output, sizeof(cfg.cc_output));
    get_bool("cc_wall", &cfg.cc_wall);
}

static int put_u32(const char* name, uint32_t v) {
    return os_reg_set(REG_KEY, name, &v, sizeof(v));
}

bool settings_save(void) {
    int bad = 0;
    bad |= put_u32("tab_width", cfg.tab_width);
    bad |= put_u32("use_spaces", cfg.use_spaces);
    bad |= put_u32("auto_indent", cfg.auto_indent);
    bad |= put_u32("brace_complete", cfg.brace_complete);
    bad |= put_u32("highlight", cfg.highlight);
    bad |= put_u32("line_numbers", cfg.line_numbers);
    bad |= put_u32("eol", cfg.eol);
    bad |= os_reg_set_string(REG_KEY, "cc_path", cfg.cc_path);
    bad |= os_reg_set_string(REG_KEY, "cc_options", cfg.cc_options);
    bad |= os_reg_set_string(REG_KEY, "cc_libs", cfg.cc_libs);
    bad |= os_reg_set_string(REG_KEY, "cc_output", cfg.cc_output);
    bad |= put_u32("cc_wall", cfg.cc_wall);
    return bad == 0;
}

//
// The window
//

typedef enum { F_CHECK, F_NUMBER, F_TEXT, F_CHOICE } field_kind_t;

typedef struct {
    field_kind_t kind;
    const char*  label;
    const char*  hint;              // Grey text under a text field, or NULL
    // Where the value lives in the working copy
    bool*        flag;              // F_CHECK
    uint32_t*    number;            // F_NUMBER, F_CHOICE
    char*        text;              // F_TEXT
    const char* const* choices;     // F_CHOICE
    uint32_t     choice_count;
    ui_edit_t    edit;              // F_NUMBER, F_TEXT
    gfx_rect_t   rect;              // The control (set while painting)
} field_t;

static const char* const eol_names[] = { "As loaded", "LF (Unix)", "CRLF (DOS)" };

#define FIELD_MAX 8
#define BTN_COUNT 3                 // Defaults, OK, Cancel
#define FOCUS_BUTTONS FIELD_MAX     // focus >= this: a button

static struct {
    wm_window_t     win;
    settings_page_t page;
    settings_t      work;           // What the window shows; cfg on OK
    field_t         fields[FIELD_MAX];
    int             count;
    int             focus;          // A field, or FOCUS_BUTTONS + button
    int             pressed;        // Button held down, or -1
    bool            dragging;       // Selecting in the focused edit box
    bool            dirty;
    gfx_rect_t      tabs[2];
} sw;

static void add_check(const char* label, bool* flag) {
    field_t* f = &sw.fields[sw.count++];
    memset(f, 0, sizeof(*f));
    f->kind = F_CHECK;
    f->label = label;
    f->flag = flag;
}

static void add_number(const char* label, uint32_t* number) {
    field_t* f = &sw.fields[sw.count++];
    memset(f, 0, sizeof(*f));
    f->kind = F_NUMBER;
    f->label = label;
    f->number = number;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)*number);
    ui_edit_init(&f->edit, buf, 2, 0);
}

static void add_text(const char* label, char* text, const char* hint) {
    field_t* f = &sw.fields[sw.count++];
    memset(f, 0, sizeof(*f));
    f->kind = F_TEXT;
    f->label = label;
    f->hint = hint;
    f->text = text;
    ui_edit_init(&f->edit, text, CFG_TEXT_MAX - 1, 0);
}

static void add_choice(const char* label, uint32_t* number, const char* const* choices, uint32_t count) {
    field_t* f = &sw.fields[sw.count++];
    memset(f, 0, sizeof(*f));
    f->kind = F_CHOICE;
    f->label = label;
    f->number = number;
    f->choices = choices;
    f->choice_count = count;
}

// The fields of the page shown, on the working copy
static void build_page(void) {
    sw.count = 0;
    if (sw.page == SETTINGS_EDITOR) {
        add_number("Tab width", &sw.work.tab_width);
        add_check("Indent with spaces instead of tabs", &sw.work.use_spaces);
        add_check("Auto indent (one more level after {)", &sw.work.auto_indent);
        add_check("Complete braces, brackets and quotes", &sw.work.brace_complete);
        add_check("C syntax highlighting (.c and .h files)", &sw.work.highlight);
        add_check("Line numbers", &sw.work.line_numbers);
        add_choice("End of line", &sw.work.eol, eol_names, 3);
    } else {
        add_text("Compiler", sw.work.cc_path, "Empty: tcc, from the system's bin folder");
        add_text("Options", sw.work.cc_options, "Before the file, e.g. -O2 -DDEBUG -Iinclude");
        add_text("Libraries", sw.work.cc_libs, "After the file, e.g. -lm");
        add_text("Output name", sw.work.cc_output, "Empty: the source's name without .c");
        add_check("Warn about questionable code (-Wall)", &sw.work.cc_wall);
    }
    sw.focus = 0;
    sw.pressed = -1;
    sw.dragging = false;
}

// The edit boxes' text back into the working copy. False (and the focus
// on it) when a number is out of range.
static bool collect(void) {
    for (int i = 0; i < sw.count; i++) {
        field_t* f = &sw.fields[i];
        if (f->kind == F_TEXT) {
            strlcpy(f->text, f->edit.text, CFG_TEXT_MAX);
        } else if (f->kind == F_NUMBER) {
            char* end;
            long v = strtol(f->edit.text, &end, 10);
            if (f->edit.text[0] == '\0' || *end != '\0' || v < TAB_WIDTH_MIN || v > TAB_WIDTH_MAX) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s must be %d to %d", f->label, TAB_WIDTH_MIN, TAB_WIDTH_MAX);
                notify_error("Settings", msg);
                sw.focus = i;
                ui_edit_select_all(&f->edit);
                sw.dirty = true;
                return false;
            }
            *f->number = (uint32_t)v;
        }
    }
    return true;
}

static int32_t window_height(void) {
    // The taller page decides, so switching pages doesn't resize the window
    int32_t editor   = 7 * ROW_H;
    int32_t compiler = 4 * (ROW_H + 12) + ROW_H;
    int32_t body = editor > compiler ? editor : compiler;
    return TAB_H + SW_MARGIN + body + SW_MARGIN + SBTN_H + SW_MARGIN;
}

static gfx_rect_t button_rect_at(int i) {
    int32_t w, h;
    wm_window_get_size(sw.win, &w, &h);
    if (i == 0)                     // Defaults, on its own at the left
        return gfx_rect(SW_MARGIN, h - SW_MARGIN - SBTN_H, SBTN_W, SBTN_H);
    int32_t x = w - SW_MARGIN - (BTN_COUNT - i) * SBTN_W - (BTN_COUNT - 1 - i) * SBTN_GAP;
    return gfx_rect(x, h - SW_MARGIN - SBTN_H, SBTN_W, SBTN_H);
}

static const char* const button_labels[BTN_COUNT] = { "Defaults", "OK", "Cancel" };

static int button_hit(int32_t x, int32_t y) {
    for (int i = 0; i < BTN_COUNT; i++) {
        gfx_rect_t r = button_rect_at(i);
        if (point_in(&r, x, y))
            return i;
    }
    return -1;
}

static ui_edit_style_t edit_style(bool focused) {
    ui_edit_style_t st;
    st.background = theme.edit.background;
    st.border     = focused ? theme.button.border_active : theme.edit.border;
    st.text       = theme.edit.text;
    st.cursor     = focused ? theme.edit.cursor : theme.edit.background;
    st.selection  = focused ? theme.edit.selection : theme.edit.background;
    return st;
}

// The radio button `k` of a choice field
static gfx_rect_t choice_rect(gfx_surface_t* s, const field_t* f, uint32_t k) {
    int32_t x = f->rect.x;
    for (uint32_t j = 0; j < k; j++)
        x += CHECK_SIZE + 6 + gfx_text_width(s, f->choices[j]) + 14;
    return gfx_rect(x, f->rect.y, CHECK_SIZE + 6 + gfx_text_width(s, f->choices[k]), f->rect.h);
}

static void layout(gfx_surface_t* s, int32_t w) {
    int32_t y = TAB_H + SW_MARGIN;
    int32_t cx = SW_MARGIN + LABEL_W;
    int32_t cw = w - cx - SW_MARGIN;
    gfx_rect_t bounds = gfx_rect(0, 0, w, window_height());

    for (int i = 0; i < sw.count; i++) {
        field_t* f = &sw.fields[i];
        switch (f->kind) {
            case F_CHECK:
                f->rect = gfx_rect(SW_MARGIN, y + 2, w - 2 * SW_MARGIN, ROW_H - 4);
                y += ROW_H;
                break;
            case F_NUMBER:
                f->rect = gfx_rect(cx, y + 3, 40, ROW_H - 6);
                ui_edit_place(&f->edit, f->rect.x + f->rect.w / 2, gfx_text_y(s, f->rect.y, f->rect.h),
                              f->rect.w, f->rect.w, bounds);
                y += ROW_H;
                break;
            case F_TEXT:
                f->rect = gfx_rect(cx, y + 3, cw, ROW_H - 6);
                ui_edit_place(&f->edit, f->rect.x + f->rect.w / 2, gfx_text_y(s, f->rect.y, f->rect.h),
                              f->rect.w, f->rect.w, bounds);
                y += ROW_H + (f->hint ? 12 : 0);
                break;
            case F_CHOICE:
                f->rect = gfx_rect(cx, y + 2, cw, ROW_H - 4);
                y += ROW_H;
                break;
        }
    }
}

static void draw_box(gfx_surface_t* s, int32_t x, int32_t y, bool on, bool round) {
    gfx_fill_rect(s, x, y, CHECK_SIZE, CHECK_SIZE, theme.edit.background);
    gfx_draw_rect(s, x, y, CHECK_SIZE, CHECK_SIZE, theme.edit.border);
    if (!on)
        return;
    if (round) {
        gfx_fill_rect(s, x + 3, y + 3, CHECK_SIZE - 6, CHECK_SIZE - 6, theme.edit.text);
    } else {
        // A tick
        for (int32_t k = 0; k < 3; k++) {
            gfx_draw_line(s, x + 2, y + 5 + k, x + 4, y + 7 + k, theme.edit.text);
            gfx_draw_line(s, x + 4, y + 7 + k, x + 9, y + 2 + k, theme.edit.text);
        }
    }
}

static void focus_ring(gfx_surface_t* s, const gfx_rect_t* r) {
    gfx_draw_rect(s, r->x - 2, r->y - 1, r->w + 4, r->h + 2, theme.button.border_active);
}

void settings_paint(void) {
    if (sw.win == 0)
        return;
    sw.dirty = false;
    gfx_surface_t* s = wm_window_surface(sw.win);
    if (s == NULL)
        return;
    int32_t w, h;
    wm_window_get_size(sw.win, &w, &h);
    layout(s, w);

    gfx_clear(s, theme.client.background);

    // Tabs
    const char* names[2] = { "Editor", "Compiler" };
    int32_t x = SW_MARGIN;
    gfx_draw_hline(s, 0, TAB_H - 1, w, theme.button.border);
    for (int t = 0; t < 2; t++) {
        int32_t tw = gfx_text_width(s, names[t]) + 20;
        sw.tabs[t] = gfx_rect(x, 3, tw, TAB_H - 3);
        bool on = ((int)sw.page == t);
        gfx_fill_rect(s, x, 3, tw, TAB_H - (on ? 3 : 4), on ? theme.client.background : theme.button.fill);
        gfx_draw_rect(s, x, 3, tw, TAB_H - 3 + (on ? 1 : 0), theme.button.border);
        if (on)
            gfx_draw_hline(s, x + 1, TAB_H - 1, tw - 2, theme.client.background);
        gfx_draw_text(s, x + 10, gfx_text_y(s, 3, TAB_H - 3), names[t], theme.button.text);
        x += tw + 2;
    }

    for (int i = 0; i < sw.count; i++) {
        field_t* f = &sw.fields[i];
        bool focused = (sw.focus == i);
        int32_t ty = gfx_text_y(s, f->rect.y, f->rect.h);

        switch (f->kind) {
            case F_CHECK: {
                int32_t by = f->rect.y + (f->rect.h - CHECK_SIZE) / 2;
                draw_box(s, f->rect.x, by, *f->flag, false);
                gfx_draw_text(s, f->rect.x + CHECK_SIZE + 8, ty, f->label, theme.client.text);
                if (focused) {
                    gfx_rect_t r = gfx_rect(f->rect.x + CHECK_SIZE + 6, f->rect.y,
                                            gfx_text_width(s, f->label) + 4, f->rect.h);
                    focus_ring(s, &r);
                }
                break;
            }
            case F_NUMBER:
            case F_TEXT: {
                gfx_draw_text(s, SW_MARGIN, ty, f->label, theme.client.text);
                ui_edit_style_t st = edit_style(focused);
                ui_edit_paint(&f->edit, s, &st);
                if (f->hint)
                    gfx_draw_text(s, f->rect.x, f->rect.y + f->rect.h + 3, f->hint, theme.client.text_soft);
                break;
            }
            case F_CHOICE: {
                gfx_draw_text(s, SW_MARGIN, ty, f->label, theme.client.text);
                for (uint32_t k = 0; k < f->choice_count; k++) {
                    gfx_rect_t r = choice_rect(s, f, k);
                    draw_box(s, r.x, r.y + (r.h - CHECK_SIZE) / 2, *f->number == k, true);
                    gfx_draw_text(s, r.x + CHECK_SIZE + 6, ty, f->choices[k], theme.client.text);
                    if (focused && *f->number == k)
                        focus_ring(s, &r);
                }
                break;
            }
        }
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        gfx_rect_t r = button_rect_at(i);
        draw_button(s, &r, button_labels[i], sw.focus == FOCUS_BUTTONS + i || (i == 1 && sw.focus < FOCUS_BUTTONS),
                    sw.pressed == i);
    }
    wm_window_commit(sw.win, NULL);
}

static void close_window(void) {
    if (sw.win != 0)
        wm_window_destroy(sw.win);
    sw.win = 0;
    if (main_win != 0)
        wm_window_focus(main_win);
}

static void switch_page(settings_page_t page) {
    if (page == sw.page)
        return;
    if (!collect())
        return;                     // Fix the number first
    sw.page = page;
    build_page();
    sw.dirty = true;
}

static void accept(void) {
    if (!collect())
        return;
    cfg = sw.work;
    if (!settings_save())
        notify_error("Settings", "Couldn't save the settings: they last until cedit closes");
    close_window();
    editor_settings_changed();
}

static void press_button(int i) {
    switch (i) {
        case 0:                     // Defaults: this page only
            if (sw.page == SETTINGS_EDITOR) defaults_editor(&sw.work);
            else                            defaults_compiler(&sw.work);
            build_page();
            sw.focus = FOCUS_BUTTONS;
            sw.dirty = true;
            break;
        case 1: accept();       break;
        case 2: close_window(); break;
    }
}

void settings_open(settings_page_t page) {
    if (sw.win != 0) {
        switch_page(page);
        wm_window_focus(sw.win);
        return;
    }

    memset(&sw, 0, sizeof(sw));
    sw.work = cfg;
    sw.page = page;
    build_page();

    int32_t sw_, sh_;
    wm_screen_size(&sw_, &sh_);
    int32_t h = window_height();

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title  = "cedit settings";
    d.style  = WM_STYLE_NOMINIMIZE;
    d.x      = (sw_ > SW_W) ? (sw_ - SW_W) / 2 : WM_POS_DEFAULT;
    d.y      = (sh_ > h) ? (sh_ - h) / 2 - 40 : WM_POS_DEFAULT;
    d.width  = SW_W;
    d.height = h;

    sw.win = wm_window_create(&d);
    if (sw.win == 0) {
        notify_error("Settings", strerror(errno));
        return;
    }
    settings_paint();
    wm_window_focus(sw.win);
}

wm_window_t settings_window(void) { return sw.win; }
bool settings_dirty(void)         { return sw.win != 0 && sw.dirty; }
void settings_theme_changed(void) { sw.dirty = true; }

static void move_focus(int dir) {
    int total = sw.count + BTN_COUNT;
    int pos = (sw.focus >= FOCUS_BUTTONS) ? sw.count + (sw.focus - FOCUS_BUTTONS) : sw.focus;
    pos = (pos + dir + total) % total;
    sw.focus = (pos >= sw.count) ? FOCUS_BUTTONS + (pos - sw.count) : pos;
    if (sw.focus < sw.count && (sw.fields[sw.focus].kind == F_TEXT || sw.fields[sw.focus].kind == F_NUMBER))
        ui_edit_select_all(&sw.fields[sw.focus].edit);
}

static void key(const wm_event_t* ev) {
    bool shift = (ev->mods & INPUT_MOD_SHIFT) != 0;
    bool ctrl  = (ev->mods & INPUT_MOD_CTRL) != 0;

    if (ev->key == KEY_TAB && ctrl) {
        switch_page(sw.page == SETTINGS_EDITOR ? SETTINGS_COMPILER : SETTINGS_EDITOR);
        return;
    }
    if (ev->key == KEY_TAB) {
        move_focus(shift ? -1 : 1);
        return;
    }
    if (ev->key == KEY_ESCAPE) {
        close_window();
        return;
    }

    if (sw.focus >= FOCUS_BUTTONS) {
        int b = sw.focus - FOCUS_BUTTONS;
        if (ev->key == KEY_ENTER || ev->key == ' ')
            press_button(b);
        else if (ev->key == KEY_LEFT)
            sw.focus = FOCUS_BUTTONS + (b + BTN_COUNT - 1) % BTN_COUNT;
        else if (ev->key == KEY_RIGHT)
            sw.focus = FOCUS_BUTTONS + (b + 1) % BTN_COUNT;
        return;
    }

    field_t* f = &sw.fields[sw.focus];
    switch (f->kind) {
        case F_CHECK:
            if (ev->key == ' ') *f->flag = !*f->flag;
            else if (ev->key == KEY_ENTER) accept();
            else if (ev->key == KEY_UP) move_focus(-1);
            else if (ev->key == KEY_DOWN) move_focus(1);
            break;
        case F_CHOICE:
            if (ev->key == ' ' || ev->key == KEY_RIGHT)
                *f->number = (*f->number + 1) % f->choice_count;
            else if (ev->key == KEY_LEFT)
                *f->number = (*f->number + f->choice_count - 1) % f->choice_count;
            else if (ev->key == KEY_ENTER) accept();
            else if (ev->key == KEY_UP) move_focus(-1);
            else if (ev->key == KEY_DOWN) move_focus(1);
            break;
        case F_NUMBER:
        case F_TEXT:
            if (f->kind == F_NUMBER && ev->key >= 32 && ev->key < 127 && !(ev->key >= '0' && ev->key <= '9'))
                break;              // Digits only
            switch (ui_edit_key(&f->edit, ev->key, ev->mods)) {
                case UI_EDIT_ACCEPT: accept();       return;
                case UI_EDIT_CANCEL: close_window(); return;
                case UI_EDIT_IGNORED:
                    if (ev->key == KEY_UP) move_focus(-1);
                    else if (ev->key == KEY_DOWN) move_focus(1);
                    break;
                default: break;
            }
            break;
    }
}

static void mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    gfx_surface_t* s = wm_window_surface(sw.win);
    if (s == NULL)
        return;

    for (int t = 0; t < 2; t++)
        if (point_in(&sw.tabs[t], ev->x, ev->y)) {
            switch_page((settings_page_t)t);
            return;
        }

    int b = button_hit(ev->x, ev->y);
    if (b >= 0) {
        sw.pressed = b;
        sw.focus = FOCUS_BUTTONS + b;
        return;
    }

    for (int i = 0; i < sw.count; i++) {
        field_t* f = &sw.fields[i];
        switch (f->kind) {
            case F_CHECK: {
                gfx_rect_t r = gfx_rect(f->rect.x, f->rect.y, CHECK_SIZE + 10 + gfx_text_width(s, f->label),
                                        f->rect.h);
                if (point_in(&r, ev->x, ev->y)) {
                    *f->flag = !*f->flag;
                    sw.focus = i;
                    return;
                }
                break;
            }
            case F_CHOICE:
                for (uint32_t k = 0; k < f->choice_count; k++) {
                    gfx_rect_t r = choice_rect(s, f, k);
                    if (point_in(&r, ev->x, ev->y)) {
                        *f->number = k;
                        sw.focus = i;
                        return;
                    }
                }
                break;
            case F_NUMBER:
            case F_TEXT:
                if (ui_edit_click(&f->edit, ev->x, ev->y, ev->mods, ev->clicks)) {
                    sw.focus = i;
                    sw.dragging = true;
                    return;
                }
                break;
        }
    }
}

void settings_event(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_KEY:
            key(ev);
            break;
        case WM_EVENT_MOUSE_DOWN:
            mouse_down(ev);
            break;
        case WM_EVENT_MOUSE_MOVE:
            if (sw.dragging && (ev->buttons & INPUT_BTN_LEFT) && sw.focus < sw.count)
                ui_edit_drag(&sw.fields[sw.focus].edit, ev->x);
            else
                sw.dragging = false;
            if (sw.pressed >= 0 && !(ev->buttons & INPUT_BTN_LEFT))
                sw.pressed = -1;
            break;
        case WM_EVENT_MOUSE_UP: {
            sw.dragging = false;
            int pressed = sw.pressed;
            sw.pressed = -1;
            if (pressed >= 0 && button_hit(ev->x, ev->y) == pressed) {
                sw.dirty = true;
                press_button(pressed);
                return;
            }
            break;
        }
        case WM_EVENT_CLOSE:
            close_window();
            return;
        case WM_EVENT_DESTROYED:
            sw.win = 0;
            return;
        default:
            break;
    }
    sw.dirty = true;
}
