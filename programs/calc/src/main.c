/*
 * calc: a desktop calculator.
 *
 *     calc
 *
 * One window with a display and a grid of buttons, working like a pocket
 * calculator: operators apply left to right as they are entered (no
 * precedence), = repeats the last operation when pressed again, % takes a
 * percentage of the first operand for + and - (200 + 10 % = 220) and divides
 * by 100 otherwise. Numbers are doubles, shown with up to 16 significant
 * digits.
 *
 * Keyboard: 0-9 and . (or ,), + - * / (x also multiplies), Enter or = ,
 * Backspace, Escape clears all, Delete clears the entry, % percent,
 * @ square root, q square, r reciprocal, n changes the sign. Ctrl+C copies
 * the display, Ctrl+V pastes a number. Right click for Copy / Paste.
 *
 * Built on os/wm.h, the same way as notepad and hexedit. Needs SEC_GFX and
 * SEC_IPC (the default clearance has both).
 */

#include <errno.h>
#include <math.h>
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
#define APP_NAME        "calc"
#endif

// Window
#define WIN_W           248
#define WIN_H           318
#define WIN_MIN_W       180
#define WIN_MIN_H       230

#define PAD             8
#define GAP             4           // Between buttons
#define DISPLAY_H       52
#define DISPLAY_PAD     6
#define DISPLAY_SCALE   2           // Text scale of the number when it fits

#define COLS            4
#define ROWS            6

#define ENTRY_DIGITS    16          // Most digits the user can type
#define ENTRY_MAX       32
#define EXPR_MAX        96
#define SHOW_DIGITS     16          // Significant digits shown for results

enum {
    CMD_COPY = 1,
    CMD_PASTE,
};

// What a button does
typedef enum {
    K_DIGIT,
    K_POINT,
    K_OP,           // + - * /
    K_EQUALS,
    K_PERCENT,
    K_UNARY,        // sqrt, square, reciprocal
    K_NEGATE,
    K_BACKSPACE,
    K_CLEAR_ENTRY,
    K_CLEAR,
} key_kind_t;

typedef enum {
    LOOK_DIGIT,
    LOOK_FUNCTION,
    LOOK_EQUALS,
} key_look_t;

typedef struct {
    const char* label;
    key_kind_t  kind;
    char        arg;        // The digit, operator or unary function ('s' sqrt, 'q' square, 'r' 1/x)
    key_look_t  look;
} calc_key_t;

static const calc_key_t keys[ROWS][COLS] = {
    { { "%",    K_PERCENT,     0,   LOOK_FUNCTION }, { "CE",  K_CLEAR_ENTRY, 0, LOOK_FUNCTION },
      { "C",    K_CLEAR,       0,   LOOK_FUNCTION }, { "<-",  K_BACKSPACE, 0,   LOOK_FUNCTION } },
    { { "1/x",  K_UNARY,       'r', LOOK_FUNCTION }, { "x^2", K_UNARY,     'q', LOOK_FUNCTION },
      { "sqrt", K_UNARY,       's', LOOK_FUNCTION }, { "/",   K_OP,        '/', LOOK_FUNCTION } },
    { { "7",    K_DIGIT,       '7', LOOK_DIGIT },    { "8",   K_DIGIT,     '8', LOOK_DIGIT },
      { "9",    K_DIGIT,       '9', LOOK_DIGIT },    { "*",   K_OP,        '*', LOOK_FUNCTION } },
    { { "4",    K_DIGIT,       '4', LOOK_DIGIT },    { "5",   K_DIGIT,     '5', LOOK_DIGIT },
      { "6",    K_DIGIT,       '6', LOOK_DIGIT },    { "-",   K_OP,        '-', LOOK_FUNCTION } },
    { { "1",    K_DIGIT,       '1', LOOK_DIGIT },    { "2",   K_DIGIT,     '2', LOOK_DIGIT },
      { "3",    K_DIGIT,       '3', LOOK_DIGIT },    { "+",   K_OP,        '+', LOOK_FUNCTION } },
    { { "+/-",  K_NEGATE,      0,   LOOK_DIGIT },    { "0",   K_DIGIT,     '0', LOOK_DIGIT },
      { ".",    K_POINT,       0,   LOOK_DIGIT },    { "=",   K_EQUALS,    0,   LOOK_EQUALS } },
};

//
// State
//

static struct {
    char    entry[ENTRY_MAX];   // What the display shows: the number being typed, a result or an error
    bool    fresh;              // The next digit starts a new number
    bool    error;              // entry holds a message; only typing or clearing gets out
    double  acc;                // First operand
    char    op;                 // Pending operator, 0: none
    char    last_op;            // For repeating =
    double  last_operand;
    char    expr[EXPR_MAX];     // The line above the number: "12 + ", "12 + 3 ="
} calc;

static wm_window_t  main_win;
static wm_theme_t   theme;
static bool         running = true;
static bool         dirty;
static int          pressed_row = -1;   // Button held down with the mouse
static int          pressed_col = -1;

static void notify_error(const char* title, const char* message) {
    wm_notify_ex(WM_NOTIFY_ERROR, title, message, 0);
}

static void default_theme(void) {
    memset(&theme, 0, sizeof(theme));
    theme.client.background     = GFX_RGB(236, 236, 236);
    theme.client.text           = GFX_RGB(20, 20, 20);
    theme.client.text_soft      = GFX_RGB(80, 80, 80);
    theme.client.accent         = GFX_RGB(40, 100, 200);
    theme.button.fill           = GFX_RGB(222, 222, 222);
    theme.button.border         = GFX_RGB(140, 140, 140);
    theme.button.border_active  = GFX_RGB(40, 100, 200);
    theme.button.text           = GFX_RGB(20, 20, 20);
    theme.button.fill_pressed   = GFX_RGB(190, 190, 190);
    theme.button.border_pressed = GFX_RGB(90, 90, 90);
    theme.edit.background       = GFX_COLOR_WHITE;
    theme.edit.border           = GFX_RGB(140, 140, 140);
    theme.edit.text             = GFX_RGB(20, 20, 20);
}

static void load_theme(void) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
}

//
// Numbers
//

// Shortest form with at most SHOW_DIGITS significant digits that fits in
// `max` characters. false for an infinity or NaN.
static bool format_number(double v, char* out, size_t size, size_t max) {
    if (!isfinite(v))
        return false;
    if (v == 0)
        v = 0;                              // No "-0"
    for (int digits = SHOW_DIGITS; digits >= 6; digits--) {
        snprintf(out, size, "%.*g", digits, v);
        if (strlen(out) <= max)
            break;
    }
    return true;
}

static double entry_value(void) {
    return calc.error ? 0 : strtod(calc.entry, NULL);
}

static void set_error(const char* message) {
    strlcpy(calc.entry, message, sizeof(calc.entry));
    calc.error = true;
    calc.fresh = true;
    calc.op = 0;
    calc.last_op = 0;
    calc.expr[0] = 0;
}

// Show a result; false (and an error on the display) when it isn't a number
static bool set_value(double v) {
    if (!format_number(v, calc.entry, sizeof(calc.entry), ENTRY_DIGITS + 6)) {
        set_error(isnan(v) ? "Invalid input" : "Overflow");
        return false;
    }
    calc.error = false;
    return true;
}

static const char* op_text(char op) {
    switch (op) {
        case '+': return "+";
        case '-': return "-";
        case '*': return "*";
        case '/': return "/";
        default:  return "";
    }
}

// a op b; false when it can't be done (the display says why)
static bool apply(double a, char op, double b, double* out) {
    switch (op) {
        case '+': *out = a + b; break;
        case '-': *out = a - b; break;
        case '*': *out = a * b; break;
        case '/':
            if (b == 0) {
                set_error(a == 0 ? "Result is undefined" : "Cannot divide by zero");
                return false;
            }
            *out = a / b;
            break;
        default:  *out = b; break;
    }
    if (!isfinite(*out)) {
        set_error("Overflow");
        return false;
    }
    return true;
}

static void clear_all(void) {
    memset(&calc, 0, sizeof(calc));
    strlcpy(calc.entry, "0", sizeof(calc.entry));
    calc.fresh = true;
}

static void clear_entry(void) {
    if (calc.error) {
        clear_all();
        return;
    }
    strlcpy(calc.entry, "0", sizeof(calc.entry));
    calc.fresh = true;
}

static size_t digit_count(const char* s) {
    size_t n = 0;
    for (; *s; s++)
        if (*s >= '0' && *s <= '9')
            n++;
    return n;
}

static void begin_entry(void) {
    if (calc.error)
        clear_all();
    if (calc.fresh) {
        strlcpy(calc.entry, "0", sizeof(calc.entry));
        calc.fresh = false;
        if (calc.op == 0)
            calc.expr[0] = 0;               // A new calculation after "="
    }
}

static void type_digit(char d) {
    begin_entry();
    if (digit_count(calc.entry) >= ENTRY_DIGITS)
        return;
    if (strcmp(calc.entry, "0") == 0) {
        calc.entry[0] = d;
    } else if (strcmp(calc.entry, "-0") == 0) {
        calc.entry[1] = d;
    } else {
        char tail[2] = { d, 0 };
        strlcat(calc.entry, tail, sizeof(calc.entry));
    }
}

static void type_point(void) {
    begin_entry();
    if (strchr(calc.entry, '.') == NULL)
        strlcat(calc.entry, ".", sizeof(calc.entry));
}

static void backspace(void) {
    if (calc.error) {
        clear_all();
        return;
    }
    if (calc.fresh) {
        if (calc.op == 0)
            calc.expr[0] = 0;               // After "=": drop the finished sum, keep the result
        return;                             // A result isn't edited
    }
    size_t n = strlen(calc.entry);
    if (n > 0)
        calc.entry[--n] = 0;
    if (n == 0 || strcmp(calc.entry, "-") == 0)
        strlcpy(calc.entry, "0", sizeof(calc.entry));
}

static void negate(void) {
    if (calc.error)
        return;
    if (!calc.fresh) {
        // Still typing: flip the sign of the text, keeping a trailing "."
        if (calc.entry[0] == '-')
            memmove(calc.entry, calc.entry + 1, strlen(calc.entry));
        else if (strlen(calc.entry) + 1 < sizeof(calc.entry)) {
            memmove(calc.entry + 1, calc.entry, strlen(calc.entry) + 1);
            calc.entry[0] = '-';
        }
        return;
    }
    set_value(-entry_value());
}

static void operator(char op) {
    if (calc.error)
        return;
    double v = entry_value();
    if (calc.op != 0 && !calc.fresh) {
        double r;
        if (!apply(calc.acc, calc.op, v, &r))
            return;
        calc.acc = r;
        set_value(r);
    } else if (calc.op == 0) {
        calc.acc = v;
    }
    // With an operator pending and nothing typed since, this just swaps it
    calc.op = op;
    calc.last_op = 0;
    calc.fresh = true;

    char a[ENTRY_MAX];
    format_number(calc.acc, a, sizeof(a), ENTRY_DIGITS + 6);
    snprintf(calc.expr, sizeof(calc.expr), "%s %s", a, op_text(op));
}

static void equals(void) {
    if (calc.error)
        return;
    double a, b;
    char op;
    if (calc.op != 0) {
        a = calc.acc;
        op = calc.op;
        b = entry_value();
    } else if (calc.last_op != 0) {
        a = entry_value();                  // Again: repeat the last step on the result
        op = calc.last_op;
        b = calc.last_operand;
    } else {
        return;
    }

    double r;
    if (!apply(a, op, b, &r))
        return;
    char as[ENTRY_MAX], bs[ENTRY_MAX];
    format_number(a, as, sizeof(as), ENTRY_DIGITS + 6);
    format_number(b, bs, sizeof(bs), ENTRY_DIGITS + 6);
    snprintf(calc.expr, sizeof(calc.expr), "%s %s %s =", as, op_text(op), bs);

    calc.op = 0;
    calc.last_op = op;
    calc.last_operand = b;
    calc.fresh = true;
    set_value(r);
}

static void percent(void) {
    if (calc.error)
        return;
    double v = entry_value();
    if (calc.op == '+' || calc.op == '-')
        v = calc.acc * v / 100;             // 200 + 10% -> 200 + 20
    else
        v = v / 100;
    if (set_value(v))
        calc.fresh = true;
}

static void unary(char fn) {
    if (calc.error)
        return;
    double v = entry_value();
    switch (fn) {
        case 's':
            if (v < 0) {
                set_error("Invalid input");
                return;
            }
            v = sqrt(v);
            break;
        case 'q':
            v = v * v;
            break;
        case 'r':
            if (v == 0) {
                set_error("Cannot divide by zero");
                return;
            }
            v = 1 / v;
            break;
    }
    if (set_value(v))
        calc.fresh = true;
}

static void press(const calc_key_t* k) {
    switch (k->kind) {
        case K_DIGIT:       type_digit(k->arg); break;
        case K_POINT:       type_point();       break;
        case K_OP:          operator(k->arg);   break;
        case K_EQUALS:      equals();           break;
        case K_PERCENT:     percent();          break;
        case K_UNARY:       unary(k->arg);      break;
        case K_NEGATE:      negate();           break;
        case K_BACKSPACE:   backspace();        break;
        case K_CLEAR_ENTRY: clear_entry();      break;
        case K_CLEAR:       clear_all();        break;
    }
    dirty = true;
}

static const calc_key_t* find_key(key_kind_t kind, char arg) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (keys[r][c].kind == kind && keys[r][c].arg == arg)
                return &keys[r][c];
    return NULL;
}

//
// Clipboard
//

static void copy_display(void) {
    if (wm_clip_set_text(calc.entry) < 0)
        notify_error("Can't copy", strerror(errno));
}

static void paste_number(void) {
    wm_clip_info_t info;
    char* data = wm_clip_get(&info);
    if (data == NULL)
        return;                             // Empty
    if (info.type == WM_CLIP_TEXT) {
        // A number, possibly with spaces around it and ',' for the point
        char text[ENTRY_MAX + 8];
        const char* p = data;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        strlcpy(text, p, sizeof(text));
        for (char* q = text; *q; q++)
            if (*q == ',')
                *q = '.';
        char* end;
        double v = strtod(text, &end);
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
            end++;
        if (end != text && *end == 0 && isfinite(v)) {
            if (calc.error)
                clear_all();
            if (calc.op == 0)
                calc.expr[0] = 0;
            set_value(v);
            calc.fresh = false;             // Can be edited like typed digits
            dirty = true;
        } else {
            wm_notify("Can't paste", "Not a number");
        }
    }
    free(data);
}

//
// Drawing
//

typedef struct {
    int32_t    width, height;
    gfx_rect_t display;
    int32_t    grid_y;
    int32_t    btn_w, btn_h;
} metrics_t;

static metrics_t get_metrics(void) {
    metrics_t m;
    wm_window_get_size(main_win, &m.width, &m.height);
    m.display = gfx_rect(PAD, PAD, m.width - 2 * PAD, DISPLAY_H);
    m.grid_y = PAD + DISPLAY_H + PAD;
    m.btn_w = (m.width - 2 * PAD - (COLS - 1) * GAP) / COLS;
    m.btn_h = (m.height - m.grid_y - PAD - (ROWS - 1) * GAP) / ROWS;
    if (m.btn_w < 1) m.btn_w = 1;
    if (m.btn_h < 1) m.btn_h = 1;
    return m;
}

static gfx_rect_t button_rect(const metrics_t* m, int row, int col) {
    return gfx_rect(PAD + col * (m->btn_w + GAP), m->grid_y + row * (m->btn_h + GAP), m->btn_w, m->btn_h);
}

static bool button_at(const metrics_t* m, int32_t x, int32_t y, int* row, int* col) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            gfx_rect_t b = button_rect(m, r, c);
            if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
                *row = r;
                *col = c;
                return true;
            }
        }
    }
    return false;
}

static void draw_button(gfx_surface_t* s, const gfx_rect_t* r, const calc_key_t* k, bool pressed) {
    gfx_color_t fill, border, text;
    if (k->look == LOOK_EQUALS) {
        fill   = pressed ? theme.button.border_pressed : theme.client.accent;
        border = pressed ? theme.button.border_pressed : theme.button.border_active;
        text   = GFX_COLOR_WHITE;
    } else {
        fill   = pressed ? theme.button.fill_pressed
               : k->look == LOOK_DIGIT ? theme.edit.background : theme.button.fill;
        border = pressed ? theme.button.border_pressed : theme.button.border;
        text   = theme.button.text;
    }
    gfx_fill_rect(s, r->x, r->y, r->w, r->h, fill);
    gfx_draw_rect(s, r->x, r->y, r->w, r->h, border);

    int32_t tw = gfx_text_width(s, k->label);
    gfx_draw_text(s, r->x + (r->w - tw) / 2, r->y + (r->h - gfx_line_height(s)) / 2, k->label, text);
}

static void draw_display(gfx_surface_t* s, const metrics_t* m) {
    const gfx_rect_t* d = &m->display;
    gfx_fill_rect(s, d->x, d->y, d->w, d->h, theme.edit.background);
    gfx_draw_rect(s, d->x, d->y, d->w, d->h, theme.edit.border);

    gfx_save(s);
    gfx_rect_t inner = gfx_rect(d->x + 1, d->y + 1, d->w - 2, d->h - 2);
    gfx_intersect_clip(s, &inner);

    // The expression line, right-aligned; its start is cut when too long
    int32_t avail = d->w - 2 * DISPLAY_PAD;
    const char* expr = calc.expr;
    while (*expr && gfx_text_width(s, expr) > avail)
        expr++;
    gfx_draw_text(s, d->x + d->w - DISPLAY_PAD - gfx_text_width(s, expr), d->y + DISPLAY_PAD,
                  expr, theme.client.text_soft);

    // The number: big when it fits
    gfx_set_text_scale(s, DISPLAY_SCALE);
    if (gfx_text_width(s, calc.entry) > avail)
        gfx_set_text_scale(s, 1);
    int32_t tw = gfx_text_width(s, calc.entry);
    int32_t y = d->y + d->h - DISPLAY_PAD - gfx_line_height(s);
    gfx_draw_text(s, d->x + d->w - DISPLAY_PAD - tw, y, calc.entry, theme.edit.text);
    gfx_restore(s);
}

static void draw_main(void) {
    dirty = false;
    gfx_surface_t* s = wm_window_surface(main_win);
    if (s == NULL)
        return;
    metrics_t m = get_metrics();

    gfx_clear(s, theme.client.background);
    draw_display(s, &m);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            gfx_rect_t b = button_rect(&m, r, c);
            draw_button(s, &b, &keys[r][c], r == pressed_row && c == pressed_col);
        }
    }
    wm_window_commit(main_win, NULL);
}

//
// Events
//

static void main_key(const wm_event_t* ev) {
    uint16_t key = ev->key;

    if (ev->mods & INPUT_MOD_CTRL) {
        // The letter, whether the dwm sends 'c', 'C' or the control code 3
        char letter = 0;
        if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))
            letter = (char)(key | 0x20);
        else if (key >= 3 && key <= 26 && key != KEY_TAB)
            letter = (char)('a' + key - 1);
        if (letter == 'c')
            copy_display();
        else if (letter == 'v')
            paste_number();
        return;
    }

    const calc_key_t* k = NULL;
    switch (key) {
        case KEY_ENTER:
        case '\r':
        case '\n':
        case '=':           k = find_key(K_EQUALS, 0);      break;
        case KEY_BACKSPACE: k = find_key(K_BACKSPACE, 0);   break;
        case KEY_ESCAPE:    k = find_key(K_CLEAR, 0);       break;
        case KEY_DELETE:    k = find_key(K_CLEAR_ENTRY, 0); break;
        case '.':
        case ',':           k = find_key(K_POINT, 0);       break;
        case '%':           k = find_key(K_PERCENT, 0);     break;
        case '@':           k = find_key(K_UNARY, 's');     break;
        case 'q':           k = find_key(K_UNARY, 'q');     break;
        case 'r':           k = find_key(K_UNARY, 'r');     break;
        case 'n':           k = find_key(K_NEGATE, 0);      break;
        case 'x':
        case 'X':           k = find_key(K_OP, '*');        break;
        case '+': case '-': case '*': case '/':
                            k = find_key(K_OP, (char)key);  break;
        default:
            if (key >= '0' && key <= '9')
                k = find_key(K_DIGIT, (char)key);
            break;
    }
    if (k != NULL)
        press(k);
}

static void main_mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return;
    metrics_t m = get_metrics();
    int r, c;
    if (button_at(&m, ev->x, ev->y, &r, &c)) {
        pressed_row = r;
        pressed_col = c;
        dirty = true;
    }
}

// The button acts on release, if the pointer is still on it
static void main_mouse_up(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT || pressed_row < 0)
        return;
    metrics_t m = get_metrics();
    int r, c;
    if (button_at(&m, ev->x, ev->y, &r, &c) && r == pressed_row && c == pressed_col)
        press(&keys[r][c]);
    pressed_row = pressed_col = -1;
    dirty = true;
}

static void show_context_menu(int32_t x, int32_t y) {
    wm_clip_info_t info;
    bool can_paste = wm_clip_info(&info) == 0 && info.type == WM_CLIP_TEXT;
    wm_menu_item_t items[] = {
        { "Copy",  CMD_COPY,  0 },
        { "Paste", CMD_PASTE, can_paste ? 0 : WM_MENU_DISABLED },
    };
    wm_menu_show(main_win, x, y, items, 2);
}

static void main_event(const wm_event_t* ev) {
    switch (ev->type) {
        case WM_EVENT_KEY:          main_key(ev);                     break;
        case WM_EVENT_MOUSE_DOWN:   main_mouse_down(ev);              break;
        case WM_EVENT_MOUSE_UP:     main_mouse_up(ev);                break;
        case WM_EVENT_CONTEXT_MENU: show_context_menu(ev->x, ev->y);  break;

        case WM_EVENT_MENU_COMMAND:
            if (ev->param == CMD_COPY)
                copy_display();
            else if (ev->param == CMD_PASTE)
                paste_number();
            break;

        case WM_EVENT_MOUSE_LEAVE:
        case WM_EVENT_FOCUS_LOST:
            if (pressed_row >= 0) {
                pressed_row = pressed_col = -1;
                dirty = true;
            }
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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    load_theme();
    clear_all();

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title      = "Calculator";
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
    draw_main();
    wm_window_focus(main_win);

    // Events are handled as they come; the window is repainted once the
    // queue runs dry, so a burst of keys costs one frame
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
            load_theme();
            dirty = true;
        } else if (ev.window == main_win) {
            main_event(&ev);
        }
    }

    wm_disconnect();
    return 0;
}
