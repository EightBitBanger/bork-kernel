/*
 * menu.c: the start menu. Two lists, the theater (the endless world, or
 * any .map file in maps/) and the aircraft, over a fly-around of where the
 * chosen theater starts. Up / Down picks in a list, Left / Right (or Tab)
 * goes to the other list, Enter flies. A click picks, a double click (or
 * the FLY button) flies. Under them, the two kinds of weapon to carry: 1
 * and 2 (or a click on either) step through what each can be. It comes after
 * the pilot screen (pilot.c); Escape goes back to that.
 */

#include <dirent.h>
#include <os/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flight.h"

#define MAX_MAPS        32
#define MENU_W          600         // The panel, at most
#define MENU_H          380

typedef struct {
    char path[256];                 // "" for the endless world
    char name[48];
    bool broken;                    // Wouldn't load
} map_entry_t;

static map_entry_t maps[MAX_MAPS];
static int         map_count;
static int         sel_map, sel_craft;
static int         focus;           // 0: theaters, 1: aircraft
static int         first_map, first_craft;     // Scrolled to

// Where things were drawn, for clicks
static rect_t      map_rows[MAX_MAPS], craft_rows[16], fly_button, night_button, slot_button[LOAD_SLOTS];
static int         map_rows_n, craft_rows_n;
static bool        fly_hover;

static bool ends_with_map(const char* s) {
    size_t n = strlen(s);
    return n > 4 && !strcasecmp(s + n - 4, ".map");
}

static void add_map(const char* path) {
    if (map_count >= MAX_MAPS)
        return;
    const char* file = strrchr(path, '/');
    file = file ? file + 1 : path;
    for (int i = 1; i < map_count; i++) {
        const char* other = strrchr(maps[i].path, '/');
        if (!strcasecmp(other ? other + 1 : maps[i].path, file))
            return;                 // The same map from another folder
    }
    map_entry_t* m = &maps[map_count];
    if (!map_peek_name(path, m->name, sizeof(m->name)))
        return;
    if (!m->name[0])
        snprintf(m->name, sizeof(m->name), "%.*s", (int)(strlen(file) - 4), file);
    snprintf(m->path, sizeof(m->path), "%s", path);
    m->broken = false;
    map_count++;
}

static void scan(const char* dir) {
    DIR* d = opendir(dir);
    if (!d)
        return;
    struct dirent* e;
    char path[sizeof(e->d_name) + 256];
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR || !ends_with_map(e->d_name))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        add_map(path);
    }
    closedir(d);
}

static int by_name(const void* a, const void* b) {
    return strcasecmp(((const map_entry_t*)a)->name, ((const map_entry_t*)b)->name);
}

// Load the chosen theater, so it is ready (and behind the menu)
static void apply_map(void) {
    map_entry_t* m = &maps[sel_map];
    if (!m->path[0] || m->broken) {
        map_unload();
        return;
    }
    if (!map_load(m->path)) {
        m->broken = true;
        map_unload();
    }
}

void menu_init(const char* exe, const char* preselect) {
    map_count = 1;
    snprintf(maps[0].name, sizeof(maps[0].name), "Endless farmland");
    maps[0].path[0] = 0;

    // maps/ here, and next to the program
    scan("maps");
    if (exe && strrchr(exe, '/')) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%.*s/maps", (int)(strrchr(exe, '/') - exe), exe);
        scan(dir);
    }
    qsort(maps + 1, (size_t)(map_count - 1), sizeof(map_entry_t), by_name);

    // One named on the command line: chosen to start with
    sel_map = map_count > 1 ? 1 : 0;
    if (preselect) {
        char with_ext[256];
        snprintf(with_ext, sizeof(with_ext), "%s.map", preselect);
        const char* file = strrchr(preselect, '/');
        file = file ? file + 1 : preselect;
        int found = -1;
        for (int i = 1; i < map_count && found < 0; i++) {
            const char* other = strrchr(maps[i].path, '/');
            other = other ? other + 1 : maps[i].path;
            size_t n = strlen(file);
            if (!strcasecmp(other, file) || (!strncasecmp(other, file, n) && !strcasecmp(other + n, ".map")))
                found = i;
        }
        if (found < 0) {
            int before = map_count;
            add_map(preselect);
            if (map_count == before)
                add_map(with_ext);
            if (map_count > before)
                found = map_count - 1;
        }
        if (found >= 0)
            sel_map = found;
    }
    apply_map();
}

// The next weapon for a slot: not what the other carries; the second may be empty
static void next_weapon(int slot) {
    int other = loadout[1 - slot], t = loadout[slot];
    for (int k = 0; k < WPN_TYPES + 1; k++) {
        t = t + 1 >= WPN_TYPES ? (slot == 1 ? -1 : 0) : t + 1;
        if (t != other || t < 0)
            break;
    }
    loadout[slot] = t;
}

static void move_in_list(int dir) {
    if (focus == 0) {
        int was = sel_map;
        sel_map = (sel_map + dir + map_count) % map_count;
        if (sel_map != was)
            apply_map();
    } else {
        sel_craft = (sel_craft + dir + aircraft_count) % aircraft_count;
        set_aircraft(sel_craft);
    }
}

bool menu_key(uint16_t key) {
    if (key == '\t')
        key = KEY_TAB;
    switch (key) {
        case KEY_UP:    move_in_list(-1); return false;
        case KEY_DOWN:  move_in_list(1);  return false;
        case KEY_LEFT:  focus = 0;        return false;
        case KEY_RIGHT: focus = 1;        return false;
        case KEY_TAB:   focus = !focus;   return false;
        case 'n': case 'N': night = !night; return false;
        case '1':       next_weapon(0);   return false;
        case '2':       next_weapon(1);   return false;
        case KEY_ENTER: case '\r': case '\n': case ' ':
            return true;
    }
    return false;
}

static bool in_rect(const rect_t* r, int32_t x, int32_t y) {
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

bool menu_in_rect(const rect_t* r, int32_t x, int32_t y) {
    return in_rect(r, x, y);
}

bool menu_click(int32_t x, int32_t y, int clicks) {
    if (in_rect(&fly_button, x, y))
        return true;
    for (int i = 0; i < map_rows_n; i++) {
        if (!in_rect(&map_rows[i], x, y))
            continue;
        focus = 0;
        if (first_map + i != sel_map) {
            sel_map = first_map + i;
            apply_map();
        }
        return clicks >= 2;
    }
    for (int k = 0; k < LOAD_SLOTS; k++)
        if (in_rect(&slot_button[k], x, y)) {
            next_weapon(k);
            return false;
        }
    if (in_rect(&night_button, x, y)) {
        night = !night;
        return false;
    }
    for (int i = 0; i < craft_rows_n; i++) {
        if (!in_rect(&craft_rows[i], x, y))
            continue;
        focus = 1;
        sel_craft = first_craft + i;
        set_aircraft(sel_craft);
        return clicks >= 2;
    }
    return false;
}

void menu_move(int32_t x, int32_t y) {
    fly_hover = in_rect(&fly_button, x, y);
}

//
// Drawing
//

#define C_PANEL         GFX_RGB(236, 240, 244)
#define C_PANEL_EDGE    GFX_RGB(40, 52, 70)
#define C_PICK          GFX_RGB(150, 190, 240)      // The choice, in the list being used
#define C_PICK_IDLE     GFX_RGB(200, 204, 210)      // ... in the other one
#define C_BUTTON        GFX_RGB(120, 200, 130)
#define C_BUTTON_HOVER  GFX_RGB(150, 230, 160)

// Lighten the picture under a box toward the panel color
static void panel(int32_t x, int32_t y, int32_t w, int32_t h) {
    for (int32_t j = y < 0 ? 0 : y; j < y + h && j < sh; j++)
        for (int32_t i = x < 0 ? 0 : x; i < x + w && i < sw; i++)
            frame[(size_t)j * sw + i] = mix(frame[(size_t)j * sw + i], C_PANEL, 200);
    draw_rect(x, y, w - 1, h - 1, C_PANEL_EDGE);
}

// A list: its title, rows (the chosen one marked), scrolled to keep the
// choice in view; where each row went
static int draw_list(int32_t x, int32_t y, int32_t w, int rows, const char* title, int count, int sel,
                     int* first, bool active, const char* (*name)(int), rect_t* where) {
    int32_t lh = gfx_line_height(surf), row_h = lh + 4;
    text_at(x, y, -1, C_TEXT, "%s", title);
    fill_rect(x, y + lh + 1, w, 1, C_PANEL_EDGE, 1);
    y += lh + 5;
    if (rows < 1)
        rows = 1;
    if (sel < *first)
        *first = sel;
    if (sel >= *first + rows)
        *first = sel - rows + 1;
    if (*first > count - rows)
        *first = count - rows > 0 ? count - rows : 0;
    int n = 0;
    for (int i = *first; i < count && n < rows; i++, n++) {
        rect_t r = { x, y + n * row_h, w, row_h };
        if (i == sel)
            fill_rect(r.x, r.y, r.w, r.h, active ? C_PICK : C_PICK_IDLE, 1);
        text_at(x + 6, r.y + 2, -1, C_TEXT, "%s", name(i));
        where[n] = r;
    }
    if (*first > 0)
        text_at(x + w - 4, y - lh - 5, 1, C_TEXT, "^");
    if (*first + rows < count)
        text_at(x + w - 4, y + rows * row_h, 1, C_TEXT, "v");
    return n;
}

void menu_panel(int32_t x, int32_t y, int32_t w, int32_t h) {
    panel(x, y, w, h);
}

int menu_list(int32_t x, int32_t y, int32_t w, int rows, const char* title, int count, int sel,
              int* first, bool active, const char* (*name)(int), rect_t* where) {
    return draw_list(x, y, w, rows, title, count, sel, first, active, name, where);
}

static const char* map_name(int i) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "%s%s", maps[i].name, maps[i].broken ? " (won't load)" : "");
    return buf;
}

static const char* craft_name(int i) {
    return aircraft_types[i].name;
}

void menu_draw(void) {
    int32_t lh = gfx_line_height(surf), cw = gfx_char_width(surf);
    // Tall enough for the longer list (scrolling if the window isn't)
    int want_rows = map_count > aircraft_count ? map_count : aircraft_count;
    int32_t want_h = 14 + lh * 2 + lh + 8 + want_rows * (lh + 4) + lh * 8 + 32;
    if (want_h > MENU_H)
        want_h = MENU_H;
    int32_t w = sw - 32 < MENU_W ? sw - 32 : MENU_W, h = sh - 24 < want_h ? sh - 24 : want_h;
    int32_t x = (sw - w) / 2, y = (sh - h) / 2;
    panel(x, y, w, h);

    text_at(sw / 2, y + 8, 0, C_TEXT, "F L I G H T");
    text_at(x + w - 12, y + 8, 1, C_TEXT, "%s", pilot_title());

    // The two lists, side by side, down to the details
    int32_t col_w = (w - 36) / 2, top = y + 14 + lh * 2;
    int32_t details = y + h - lh * 8 - 24;
    int rows = (int)((details - top - lh - 8) / (lh + 4));
    map_rows_n = draw_list(x + 12, top, col_w, rows, "THEATER", map_count, sel_map, &first_map,
                           focus == 0, map_name, map_rows);
    craft_rows_n = draw_list(x + 24 + col_w, top, col_w, rows, "AIRCRAFT", aircraft_count, sel_craft,
                             &first_craft, focus == 1, craft_name, craft_rows);

    // About the choices
    int32_t dx = x + 12, dy = details;
    fill_rect(x + 12, dy - 4, w - 24, 1, C_PANEL_EDGE, 1);
    if (map.loaded)
        text_at(dx, dy, -1, C_TEXT, "%s: %d targets%s", map.name, map.target_count,
                map.problems ? " (the map has problems)" : "");
    else if (maps[sel_map].broken)
        text_at(dx, dy, -1, C_TEXT_ALERT, "%s won't load", maps[sel_map].path);
    else
        text_at(dx, dy, -1, C_TEXT, "Endless made-up farmland, hills, bases and SAM sites");
    const aircraft_t* a = &aircraft_types[sel_craft];
    float top_speed = (a->thrust_ab > 0 ? a->cruise * sqrtf(a->thrust_ab / a->thrust_mil) : a->cruise) * SPEED_BOOST;
    text_at(dx, dy + lh * 2, -1, C_TEXT, "%s", a->name);
    text_at(dx, dy + lh * 3, -1, C_TEXT, "%s", a->about);
    text_at(dx, dy + lh * 4, -1, C_TEXT, "%d kt%s, %.0f g, %.0f min of fuel",
            (int)(top_speed * 1.944f + 5) / 10 * 10, a->thin_air > 0 ? " low, far more high" : "",
            (double)a->max_g, (double)a->fuel_minutes);

    // The weapons: two kinds, how many of each it carries
    text_at(dx, dy + lh * 5 + 4, -1, C_TEXT, "WEAPONS (1, 2)");
    int32_t sx = dx + cw * 16, slot_w = (x + w - 12 - sx - 8) / 2;
    for (int k = 0; k < LOAD_SLOTS; k++) {
        rect_t r = { sx + k * (slot_w + 8), dy + lh * 5 + 2, slot_w, lh + 4 };
        slot_button[k] = r;
        fill_rect(r.x, r.y, r.w, r.h, C_PICK_IDLE, 1);
        draw_rect(r.x, r.y, r.w - 1, r.h - 1, C_PANEL_EDGE);
        int t = loadout[k];
        if (t >= 0) {
            int n = weapon_types[t].count * (a->load > 0 ? a->load : 1);
            text_at(r.x + r.w / 2, r.y + 2, 0, C_TEXT, "%d x %s", n, weapon_types[t].name);
        } else {
            text_at(r.x + r.w / 2, r.y + 2, 0, C_TEXT, "(nothing)");
        }
        text_at(r.x + r.w / 2, r.y + r.h + 2, 0, C_TEXT, "%s", t >= 0 ? weapon_types[t].about : "");
    }

    // FLY, and the keys
    int32_t bw = cw * 12, bh = lh + 8;
    fly_button = (rect_t){ x + w - 12 - bw, y + h - bh - 8, bw, bh };
    fill_rect(fly_button.x, fly_button.y, bw, bh, fly_hover ? C_BUTTON_HOVER : C_BUTTON, 1);
    draw_rect(fly_button.x, fly_button.y, bw - 1, bh - 1, C_PANEL_EDGE);
    text_at(fly_button.x + bw / 2, fly_button.y + 6, 0, C_TEXT, "FLY");
    night_button = (rect_t){ fly_button.x - 8 - bw, fly_button.y, bw, bh };
    fill_rect(night_button.x, night_button.y, bw, bh, night ? C_PICK : C_PICK_IDLE, 1);
    draw_rect(night_button.x, night_button.y, bw - 1, bh - 1, C_PANEL_EDGE);
    text_at(night_button.x + bw / 2, night_button.y + 6, 0, C_TEXT, night ? "NIGHT" : "DAY");
    text_at(x + 12, y + h - lh - 10, -1, C_TEXT, "Arrows: choose  1 2: weapons  Esc: pilots");
}
