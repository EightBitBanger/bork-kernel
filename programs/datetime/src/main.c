/*
 * datetime: set the date and time.
 *
 *     datetime                                 the window
 *     datetime -s "YYYY-MM-DD HH:MM[:SS]"      set it, no window
 *
 * Installed as <home>/sys/settings/datetime, in the settings folder the
 * start menu's "Settings" opens. The window shows:
 *
 *   the clock now     "Friday, November 6, 2026   2:37:05 PM", ticking
 *   Date              a month to pick the day from: << and >> step a year,
 *                     < and > a month (Page Up / Page Down too)
 *   Time              a box: "2:37 PM", "2:37:05 pm", "14:37" or "14:37:05"
 *
 * Until something is changed, the calendar and the box follow the clock.
 * Picking a day, stepping the month or clicking into the box stops that,
 * and Apply sets the clock to what the window shows. Then they follow it
 * again. OK applies and closes, Cancel / Escape / the close box close
 * without setting anything. Enter applies.
 *
 * Setting the clock writes the machine's clock chip (the CMOS clock on a
 * PC, which keeps it across power off) through os_clock_set(). That needs
 * admin: the first Apply asks for it with sec_elevate(), which the desktop
 * shows as a prompt. The taskbar clock picks the new time up at once.
 *
 * There are no time zones: the time set is the one shown everywhere.
 * Years 2000 to 2099 (the PC's clock keeps two digits).
 *
 * With -s it sets the clock and exits: 0 done, 1 refused or failed (a
 * notification says why), 2 bad arguments.
 *
 * Needs SEC_GFX and SEC_IPC, and SEC_ELEVATE to ask for admin: the
 * desktop's launch mask has them.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/security.h>
#include <os/ui.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

// Layout (client coordinates)
#define WIN_W               360
#define WIN_H               352
#define PAD                 14
#define FIELD_X             64
#define FIELD_H             22
#define ROW_NOW             14
#define ROW_NAV             46
#define ROW_WEEKDAYS        76
#define ROW_DAYS            96
#define DAY_H               22
#define ROW_TIME            236
#define ROW_STATUS          272
#define NAV_W               26
#define TIME_W              120
#define BUTTON_W            72
#define BUTTON_H            24

#define YEAR_MIN            2000
#define YEAR_MAX            2099

static const char* const day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char* const month_names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

// What the window shows
static int        sel_year, sel_month, sel_day;   // month 1-12
static bool       date_edited;      // Stop following the clock's date
static bool       time_edited;      // Stop following the clock's time
static ui_edit_t  time_box;
static bool       time_focus;
static bool       dragging;
static bool       have_clock;       // The machine has a clock chip
static char       status[160];
static bool       status_is_error;

static wm_window_t win;
static wm_theme_t  theme;
static int32_t     cw = 6, lh = 8;

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

static bool in_rect(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static bool leap(int y) {
    return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
}

static int days_in(int y, int m) {
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && leap(y)) ? 29 : days[m - 1];
}

// 0 = Sunday
static int weekday_of(int y, int m, int d) {
    struct tm tm = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = 12 };
    timegm(&tm);
    return tm.tm_wday;
}

// The clock now, or false without one
static bool now_tm(struct tm* out, uint64_t* ms_out) {
    uint64_t ms;
    if (os_clock_ms(&ms) < 0)
        return false;
    time_t t = (time_t)(ms / 1000u);
    if (ms_out != NULL)
        *ms_out = ms;
    return gmtime_r(&t, out) != NULL;
}

// "2:37:05 PM"
static void format_time(const struct tm* tm, bool seconds, char* out, size_t size) {
    int h = tm->tm_hour % 12;
    if (seconds)
        snprintf(out, size, "%d:%02d:%02d %s", h == 0 ? 12 : h, tm->tm_min, tm->tm_sec,
                 tm->tm_hour < 12 ? "AM" : "PM");
    else
        snprintf(out, size, "%d:%02d %s", h == 0 ? 12 : h, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

static const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

// A number of 1 to `digits` digits. NULL when there is none.
static const char* parse_number(const char* s, int digits, int* out) {
    int v = 0, n = 0;
    while (n < digits && isdigit((unsigned char)s[n])) {
        v = v * 10 + (s[n] - '0');
        n++;
    }
    if (n == 0)
        return NULL;
    *out = v;
    return s + n;
}

// "2:37 PM", "2:37:05 pm", "14:37", "14:37:05", "2:37p". False if not a time.
static bool parse_time(const char* text, int* hour, int* minute, int* second) {
    int h, m, s = 0;
    const char* p = parse_number(skip_spaces(text), 2, &h);
    if (p == NULL || *p != ':')
        return false;
    p = parse_number(p + 1, 2, &m);
    if (p == NULL || m > 59)
        return false;
    if (*p == ':') {
        p = parse_number(p + 1, 2, &s);
        if (p == NULL || s > 59)
            return false;
    }
    p = skip_spaces(p);
    char ampm = (char)tolower((unsigned char)*p);
    if (ampm == 'a' || ampm == 'p') {
        p++;
        if (tolower((unsigned char)*p) == 'm')
            p++;
        if (h < 1 || h > 12)
            return false;
        h = h % 12 + (ampm == 'p' ? 12 : 0);
    } else if (h > 23) {
        return false;
    }
    if (*skip_spaces(p) != '\0')
        return false;
    *hour = h;
    *minute = m;
    *second = s;
    return true;
}

static const char* set_error(int error) {
    switch (error) {
        case EPERM:  return "Not allowed to change the clock";
        case EINVAL: return "Pick a year from 2000 to 2099";
        case ENOENT: return "This machine has no clock chip";
        case EIO:    return "The clock chip would not take the time";
        case ENOSYS: return "This kernel can't set the clock (update it)";
        case EACCES: return "Not changed: admin was refused";
        default:     return strerror(error);
    }
}

// Set the clock to y-m-d h:m:s, asking for admin first when we don't have
// it. 0, or -1 with errno.
static int set_clock(int y, int mo, int d, int h, int mi, int s) {
    if (y < YEAR_MIN || y > YEAR_MAX) {
        errno = EINVAL;
        return -1;
    }
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
                     .tm_hour = h, .tm_min = mi, .tm_sec = s };
    time_t t = timegm(&tm);
    if (t == (time_t)-1) {
        errno = EINVAL;
        return -1;
    }
    if (!sec_is_admin()) {
        char reason[96];
        snprintf(reason, sizeof(reason), "Set the date and time to %04d-%02d-%02d %02d:%02d:%02d",
                 y, mo, d, h, mi, s);
        if (sec_elevate(reason) < 0)
            return -1;
    }
    int r = os_clock_set((uint64_t)t * 1000u);
    int e = errno;
    sec_drop(SEC_ADMIN);            // Only for this
    errno = e;
    return r;
}

//
// Following the clock
//

static void set_time_text(const char* text) {
    ui_edit_init(&time_box, text, 15, 0);
    time_box.cursor = strlen(time_box.text);
    time_box.anchor = time_box.cursor;
}

// Bring what isn't being edited up to the clock. True if anything changed.
static bool follow_clock(void) {
    struct tm now;
    if (!now_tm(&now, NULL))
        return false;
    bool any = false;
    if (!date_edited && (sel_year != now.tm_year + 1900 || sel_month != now.tm_mon + 1 ||
                         sel_day != now.tm_mday)) {
        sel_year  = now.tm_year + 1900;
        sel_month = now.tm_mon + 1;
        sel_day   = now.tm_mday;
        any = true;
    }
    if (!time_edited) {
        char text[24];
        format_time(&now, true, text, sizeof(text));
        if (strcmp(text, time_box.text) != 0) {
            set_time_text(text);
            any = true;
        }
    }
    return any;
}

static void start_following(void) {
    date_edited = time_edited = false;
    time_focus = false;
    follow_clock();
}

static bool changed(void) {
    return date_edited || time_edited;
}

static bool apply(void) {
    if (!have_clock) {
        set_status(true, "%s", set_error(ENOENT));
        return false;
    }
    if (!changed()) {
        set_status(false, "Nothing changed");
        return true;
    }
    int h, mi, s;
    if (!parse_time(time_box.text, &h, &mi, &s)) {
        set_status(true, "\"%.20s\" is not a time: type 2:37 PM or 14:37", time_box.text);
        return false;
    }
    if (set_clock(sel_year, sel_month, sel_day, h, mi, s) < 0) {
        set_status(true, "%s", set_error(errno));
        return false;
    }
    set_status(false, "Set to %s %d, %d", month_names[sel_month - 1], sel_day, sel_year);
    start_following();
    return true;
}

//
// Drawing
//

static gfx_rect_t nav_rect(int i) {         // 0 "<<", 1 "<", 2 ">", 3 ">>"
    int32_t x = (i < 2) ? FIELD_X + i * (NAV_W + 4) : WIN_W - PAD - (4 - i) * (NAV_W + 4) + 4;
    return gfx_rect(x, ROW_NAV, NAV_W, FIELD_H);
}

static int32_t cell_w(void) {
    return (WIN_W - FIELD_X - PAD) / 7;
}

// Day d of the month showing
static gfx_rect_t day_rect(int d) {
    int first = weekday_of(sel_year, sel_month, 1);
    int slot = first + d - 1;
    return gfx_rect(FIELD_X + (slot % 7) * cell_w(), ROW_DAYS + (slot / 7) * DAY_H, cell_w(), DAY_H);
}

static gfx_rect_t time_rect(void) {
    return gfx_rect(FIELD_X, ROW_TIME, TIME_W, FIELD_H);
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

static void draw_text_centered(gfx_surface_t* s, gfx_rect_t r, const char* text, uint32_t color) {
    gfx_draw_text(s, r.x + (r.w - (int32_t)strlen(text) * cw) / 2, gfx_text_y(s, r.y, r.h), text, color);
}

static void draw_button(gfx_surface_t* s, gfx_rect_t b, const char* label, uint32_t color_text) {
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, theme.button.fill);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, theme.button.border);
    draw_text_centered(s, b, label, color_text);
}

static void draw_label(gfx_surface_t* s, int32_t row, const char* label) {
    gfx_draw_text(s, PAD, gfx_text_y(s, row, FIELD_H), label, theme.client.text);
}

static void draw(void) {
    gfx_surface_t* s = wm_window_surface(win);
    if (s == NULL)
        return;
    cw = gfx_char_width(s);
    lh = gfx_line_height(s);
    gfx_clear(s, theme.client.background);

    // The clock now
    struct tm now;
    char line[80];
    bool today_known = now_tm(&now, NULL);
    if (today_known) {
        char t[24];
        format_time(&now, true, t, sizeof(t));
        snprintf(line, sizeof(line), "%s, %s %d, %d   %s", day_names[now.tm_wday], month_names[now.tm_mon],
                 now.tm_mday, now.tm_year + 1900, t);
    } else {
        snprintf(line, sizeof(line), "No clock chip: the date and time can't be set");
    }
    gfx_draw_text(s, PAD, gfx_text_y(s, ROW_NOW, FIELD_H), line,
                  today_known ? theme.client.text_soft : theme.client.text_danger);

    // Date: the month and its days
    draw_label(s, ROW_NAV, "Date");
    static const char* const nav[4] = { "<<", "<", ">", ">>" };
    for (int i = 0; i < 4; i++)
        draw_button(s, nav_rect(i), nav[i], theme.button.text);
    snprintf(line, sizeof(line), "%s %d", month_names[sel_month - 1], sel_year);
    gfx_rect_t title = gfx_rect(nav_rect(1).x + NAV_W, ROW_NAV, nav_rect(2).x - nav_rect(1).x - NAV_W, FIELD_H);
    draw_text_centered(s, title, line, date_edited ? theme.client.accent : theme.client.text);

    static const char* const short_days[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    for (int i = 0; i < 7; i++)
        draw_text_centered(s, gfx_rect(FIELD_X + i * cell_w(), ROW_WEEKDAYS, cell_w(), 18), short_days[i],
                           theme.client.text_muted);
    gfx_draw_hline(s, FIELD_X, ROW_DAYS - 3, 7 * cell_w(), theme.edit.border);

    int count = days_in(sel_year, sel_month);
    for (int d = 1; d <= count; d++) {
        gfx_rect_t r = day_rect(d);
        char num[4];
        snprintf(num, sizeof(num), "%d", d);
        bool selected = (d == sel_day);
        bool today = today_known && now.tm_year + 1900 == sel_year && now.tm_mon + 1 == sel_month &&
                     now.tm_mday == d;
        if (selected)
            gfx_fill_rect(s, r.x + 2, r.y + 1, r.w - 4, r.h - 2, theme.edit.selection);
        if (today)
            gfx_draw_rect(s, r.x + 2, r.y + 1, r.w - 4, r.h - 2, theme.client.accent);
        draw_text_centered(s, r, num, selected ? theme.edit.text : theme.client.text_soft);
    }

    // Time
    draw_label(s, ROW_TIME, "Time");
    gfx_rect_t tr = time_rect();
    ui_edit_place(&time_box, tr.x + tr.w / 2, gfx_text_y(s, tr.y, tr.h), tr.w, tr.w, gfx_rect(0, 0, WIN_W, WIN_H));
    ui_edit_style_t st = {
        .background = theme.edit.background,
        .border     = time_focus ? theme.button.border_active : theme.edit.border,
        .text       = theme.edit.text,
        .cursor     = time_focus ? theme.edit.cursor : theme.edit.background,
        .selection  = time_focus ? theme.edit.selection : theme.edit.background,
    };
    ui_edit_paint(&time_box, s, &st);
    gfx_draw_text(s, tr.x + tr.w + 10, gfx_text_y(s, tr.y, tr.h), "2:37 PM or 14:37", theme.client.text_muted);

    // A hint, or the last thing that happened
    if (status[0] != '\0')
        gfx_draw_text(s, PAD, ROW_STATUS, status, status_is_error ? theme.client.text_danger : theme.client.text_soft);
    else if (changed())
        gfx_draw_text(s, PAD, ROW_STATUS, "Apply sets the clock (asks for admin).", theme.client.text_muted);
    else
        gfx_draw_text(s, PAD, ROW_STATUS, "Pick a day or type a time to change it.", theme.client.text_muted);

    draw_button(s, apply_rect(), "Apply", changed() ? theme.button.text : theme.client.text_muted);
    draw_button(s, ok_rect(), "OK", theme.button.text);
    draw_button(s, cancel_rect(), "Cancel", theme.button.text);
    wm_window_commit(win, NULL);
}

//
// Input
//

// Move the month showing by `months`, keeping the day where it fits
static void step_month(int months) {
    int index = sel_year * 12 + (sel_month - 1) + months;
    int y = index / 12, m = index % 12 + 1;
    if (y < YEAR_MIN || y > YEAR_MAX)
        return;
    sel_year = y;
    sel_month = m;
    if (sel_day > days_in(y, m))
        sel_day = days_in(y, m);
    date_edited = true;
    status[0] = '\0';
}

// A key for the window. False: close it.
static bool key(const wm_event_t* ev) {
    if (ev->key == KEY_PAGE_UP || ev->key == KEY_PAGE_DOWN) {
        step_month(ev->key == KEY_PAGE_UP ? -1 : 1);
        draw();
        return true;
    }
    if (ev->key == KEY_TAB) {
        time_focus = !time_focus;
        if (time_focus) {
            time_edited = true;
            ui_edit_select_all(&time_box);
        }
        draw();
        return true;
    }
    if (!time_focus) {
        if (ev->key == KEY_ESCAPE)
            return false;
        if (ev->key == KEY_ENTER)
            apply();
        draw();
        return true;
    }
    switch (ui_edit_key(&time_box, ev->key, ev->mods)) {
        case UI_EDIT_ACCEPT:
            apply();
            break;
        case UI_EDIT_CANCEL:
            return false;
        case UI_EDIT_HANDLED:
            status[0] = '\0';
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

    if (ui_edit_click(&time_box, x, y, ev->mods, ev->clicks)) {
        time_focus = true;
        time_edited = true;         // Hold the time still while it is edited
        dragging = true;
        draw();
        return true;
    }
    time_focus = false;

    if (in_rect(cancel_rect(), x, y))
        return false;
    if (in_rect(ok_rect(), x, y)) {
        if (!changed() || apply())
            return false;
    } else if (in_rect(apply_rect(), x, y)) {
        apply();
    } else if (in_rect(nav_rect(0), x, y)) {
        step_month(-12);
    } else if (in_rect(nav_rect(1), x, y)) {
        step_month(-1);
    } else if (in_rect(nav_rect(2), x, y)) {
        step_month(1);
    } else if (in_rect(nav_rect(3), x, y)) {
        step_month(12);
    } else {
        int count = days_in(sel_year, sel_month);
        for (int d = 1; d <= count; d++) {
            if (in_rect(day_rect(d), x, y)) {
                sel_day = d;
                date_edited = true;
                status[0] = '\0';
                break;
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
    wm_notify("datetime", "Usage: datetime [-s \"YYYY-MM-DD HH:MM[:SS]\"]");
    return 2;
}

// datetime -s "YYYY-MM-DD HH:MM[:SS]"
static int run_arguments(int argc, char** argv) {
    if (argc != 3 || strcmp(argv[1], "-s") != 0)
        return usage();
    int y, mo, d, h, mi, s;
    const char* p = parse_number(skip_spaces(argv[2]), 4, &y);
    if (p == NULL || *p != '-' || (p = parse_number(p + 1, 2, &mo)) == NULL || *p != '-' ||
        (p = parse_number(p + 1, 2, &d)) == NULL || (*p != ' ' && *p != 'T'))
        return usage();
    if (mo < 1 || mo > 12 || d < 1 || y < YEAR_MIN || y > YEAR_MAX || d > days_in(y, mo) ||
        !parse_time(p + 1, &h, &mi, &s))
        return usage();
    if (set_clock(y, mo, d, h, mi, s) < 0) {
        wm_notify_ex(WM_NOTIFY_ERROR, "Clock not set", set_error(errno), WM_NOTIFY_DEFAULT_MS);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (wm_get_theme(&theme) < 0)
        default_theme();
    if (argc > 1)
        return run_arguments(argc, argv);

    struct tm now;
    have_clock = now_tm(&now, NULL);
    sel_year = YEAR_MIN;
    sel_month = sel_day = 1;
    set_time_text("");
    start_following();

    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    wm_window_desc_t desc = {
        .title  = "Date and time",
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
        // Wake when the next second begins, to tick the clock line
        int32_t wait = -1;
        uint64_t ms;
        if (have_clock && os_clock_ms(&ms) == 0)
            wait = (int32_t)(1000u - (uint32_t)(ms % 1000u)) + 5;
        int r = wm_wait_event(&ev, wait);
        if (r < 0)
            break;
        if (r == 0) {
            follow_clock();
            draw();
            continue;
        }
        if (ev.window != 0 && ev.window != win)
            continue;

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
            case WM_EVENT_MOUSE_DOWN:
                if (ev.button == INPUT_BTN_LEFT)
                    running = click(&ev);
                break;
            case WM_EVENT_MOUSE_MOVE:
                if (dragging && (ev.buttons & INPUT_BTN_LEFT) && time_focus) {
                    ui_edit_drag(&time_box, ev.x);
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

    wm_window_destroy(win);
    wm_disconnect();
    return 0;
}
