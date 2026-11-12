/*
 * icon_view: a minimal file explorer in a dwm window.
 *
 * Shows the entries of a directory as an icon grid, drawn with the system
 * icons from <home>/sys/images (resource_open_system + gfx_draw_resource).
 * Double-click a folder to open it, Backspace goes up, close box quits.
 * If an icon file is missing, a plain drawn box stands in for it.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/resource.h>
#include <os/wm.h>

#define MAX_ENTRIES 256
#define CELL_W      84
#define CELL_H      76
#define MARGIN      8
#define ICON_SIZE   48          // Box size for a missing icon

#define COLOR_BG        GFX_RGB(32, 36, 48)
#define COLOR_TEXT      GFX_RGB(230, 230, 236)
#define COLOR_PATH_BG   GFX_RGB(22, 24, 32)
#define COLOR_SELECT    GFX_RGB(60, 90, 150)

typedef struct {
    char name[256];
    bool is_dir;
} entry_t;

static char    cwd[PATH_MAX];
static entry_t entries[MAX_ENTRIES];
static int     entry_count;
static int     selected = -1;

static int icon_folder = -1, icon_file = -1;

// A system icon, or -1 (reported once) when it can't be loaded
static int load_icon(const char* name) {
    int h = resource_open_system(name);
    if (h < 0)
        fprintf(stderr, "icon_view: %s: %s (using a plain box)\n", name, strerror(errno));
    return h;
}

static void read_dir(void) {
    entry_count = 0;
    selected = -1;
    DIR* d = opendir(cwd);
    if (d == NULL) {
        fprintf(stderr, "icon_view: %s: %s\n", cwd, strerror(errno));
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != NULL && entry_count < MAX_ENTRIES) {
        entry_t* en = &entries[entry_count++];
        strlcpy(en->name, e->d_name, sizeof(en->name));
        en->is_dir = (e->d_type == DT_DIR);
    }
    closedir(d);
}

static int path_bar_height(gfx_surface_t* s) {
    return gfx_line_height(s) + 8;
}

static void draw_icon(gfx_surface_t* s, int handle, bool is_dir, int cx, int top) {
    resource_info_t ri;
    if (handle > 0 && resource_info(handle, &ri) == 0) {
        gfx_draw_resource(s, handle, cx - (int)ri.width / 2, top);
        return;
    }
    // Stand-in
    int x = cx - ICON_SIZE / 2;
    gfx_color_t c = is_dir ? GFX_RGB(220, 180, 80) : GFX_RGB(200, 200, 210);
    gfx_fill_rect(s, x, top + 6, ICON_SIZE, ICON_SIZE - 6, c);
    if (is_dir)
        gfx_fill_rect(s, x, top, ICON_SIZE / 2, 8, c);
    gfx_draw_rect(s, x, top + 6, ICON_SIZE, ICON_SIZE - 6, GFX_COLOR_BLACK);
}

static int columns(int32_t width) {
    int n = (width - MARGIN) / CELL_W;
    return n < 1 ? 1 : n;
}

static void draw(wm_window_t win) {
    gfx_surface_t* s = wm_window_surface(win);
    int32_t w, h;
    wm_window_get_size(win, &w, &h);

    gfx_clear(s, COLOR_BG);
    int bar = path_bar_height(s);
    gfx_fill_rect(s, 0, 0, w, bar, COLOR_PATH_BG);
    gfx_draw_text(s, 6, 4, cwd, COLOR_TEXT);

    int cols = columns(w);
    int cw = gfx_char_width(s);
    int max_chars = (CELL_W - 4) / cw;

    for (int i = 0; i < entry_count; i++) {
        int x = MARGIN + (i % cols) * CELL_W;
        int y = bar + MARGIN + (i / cols) * CELL_H;
        if (y > h)
            break;
        if (i == selected)
            gfx_fill_rect(s, x, y, CELL_W - 4, CELL_H - 4, COLOR_SELECT);

        const entry_t* e = &entries[i];
        draw_icon(s, e->is_dir ? icon_folder : icon_file, e->is_dir, x + (CELL_W - 4) / 2, y + 4);

        // Label centered under the icon, cut to the cell
        int len = (int)strlen(e->name);
        if (len > max_chars) len = max_chars;
        int tx = x + (CELL_W - 4 - len * cw) / 2;
        gfx_draw_text_n(s, tx, y + CELL_H - 4 - gfx_line_height(s) - 4, e->name, (size_t)len, COLOR_TEXT);
    }

    if (wm_window_commit(win, NULL) < 0)
        fprintf(stderr, "icon_view: drawing failed: %s\n", strerror(errno));
}

static int hit(wm_window_t win, int32_t x, int32_t y) {
    gfx_surface_t* s = wm_window_surface(win);
    int32_t w, h;
    wm_window_get_size(win, &w, &h);
    int top = path_bar_height(s) + MARGIN;
    if (x < MARGIN || y < top)
        return -1;
    int col = (x - MARGIN) / CELL_W, row = (y - top) / CELL_H;
    if (col >= columns(w))
        return -1;
    int i = row * columns(w) + col;
    return i < entry_count ? i : -1;
}

static void change_dir(const char* name) {
    char next[PATH_MAX * 2];
    snprintf(next, sizeof(next), "%s/%s", cwd, name);
    char abs[PATH_MAX];
    if (os_abspath(next, abs, sizeof(abs)) == NULL)
        return;
    strlcpy(cwd, abs, sizeof(cwd));
    read_dir();
}

int main(void) {
    if (wm_connect(2000) < 0) {
        fprintf(stderr, "icon_view: no window manager: %s\n", strerror(errno));
        return 1;
    }

    if (os_gethome(cwd, sizeof(cwd)) == NULL)
        strlcpy(cwd, "/", sizeof(cwd));
    icon_folder = load_icon("icon_folder");
    icon_file   = load_icon("icon_file");
    read_dir();

    wm_window_desc_t d = {
        .title = "Files", .x = WM_POS_DEFAULT, .y = WM_POS_DEFAULT,
        .width = 6 * CELL_W + MARGIN, .height = 3 * CELL_H + 40,
        .min_width = CELL_W + 2 * MARGIN, .min_height = CELL_H + 40,
        .style = WM_STYLE_RESIZABLE,
    };
    wm_window_t win = wm_window_create(&d);
    if (win == 0) {
        fprintf(stderr, "icon_view: no window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    draw(win);

    wm_event_t ev;
    while (wm_wait_event(&ev, -1) > 0) {
        switch (ev.type) {
            case WM_EVENT_CLOSE:
                wm_disconnect();
                return 0;
            case WM_EVENT_RESIZED:
                draw(win);
                break;
            case WM_EVENT_MOUSE_DOWN: {
                int i = hit(win, ev.x, ev.y);
                if (ev.clicks >= 2 && i >= 0 && entries[i].is_dir)
                    change_dir(entries[i].name);
                else
                    selected = i;
                draw(win);
                break;
            }
            case WM_EVENT_KEY:
                if (ev.key == KEY_BACKSPACE && strcmp(cwd, "/") != 0) {
                    change_dir("..");
                    draw(win);
                }
                break;
        }
    }
    wm_disconnect();
    return 0;
}
