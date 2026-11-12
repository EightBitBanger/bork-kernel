/*
 * Painting, plus the geometry input handling hit-tests against (kept here
 * so what is clicked is always what was drawn).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <os/resource.h>

#include "internal.h"

#define T explorer_theme

#define FALLBACK_FOLDER_COLOR   GFX_RGB(226, 184, 82)   // Drawn when icon_folder is missing

static inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }

static bool in_rect(const gfx_rect_t* r, int32_t x, int32_t y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

//
// Grid geometry
//

int explorer_columns(const explorer_t* s) {
    int n = (s->win_width - NAV_X) / ITEM_WIDTH;
    return n < 1 ? 1 : n;
}

void explorer_item_rect(const explorer_t* s, int index, gfx_rect_t* out) {
    int cols = explorer_columns(s);
    out->x = NAV_X + (index % cols) * ITEM_WIDTH;
    out->y = NAV_Y + (index / cols) * ITEM_HEIGHT - s->scroll_y;
    out->w = ITEM_WIDTH;
    out->h = ITEM_HEIGHT;
}

int32_t explorer_grid_bottom(const explorer_t* s) {
    return max32(s->win_height - NOTICE_H, GRID_TOP);
}

int explorer_item_at(const explorer_t* s, int32_t x, int32_t y) {
    if (y < GRID_TOP || y >= explorer_grid_bottom(s) || x < NAV_X)
        return -1;
    int cols = explorer_columns(s);
    int32_t gy = y - NAV_Y + s->scroll_y;
    if (gy < 0)
        return -1;
    int col = (x - NAV_X) / ITEM_WIDTH, row = gy / ITEM_HEIGHT;
    if (col >= cols)
        return -1;
    int index = row * cols + col;
    return index < s->total_items ? index : -1;
}

int32_t explorer_max_scroll(const explorer_t* s) {
    int rows = (s->total_items + explorer_columns(s) - 1) / explorer_columns(s);
    int32_t bottom = NAV_Y + rows * ITEM_HEIGHT + 4;
    return max32(0, bottom - explorer_grid_bottom(s));
}

void explorer_clamp_scroll(explorer_t* s) {
    s->scroll_y = min32(max32(s->scroll_y, 0), explorer_max_scroll(s));
}

void explorer_ensure_visible(explorer_t* s, int index) {
    if (index >= 0 && index < s->total_items) {
        gfx_rect_t r;
        explorer_item_rect(s, index, &r);
        if (r.y < GRID_TOP)
            s->scroll_y -= GRID_TOP - r.y + (NAV_Y - GRID_TOP);
        else if (r.y + r.h > explorer_grid_bottom(s))      // Above the status bar
            s->scroll_y += r.y + r.h - explorer_grid_bottom(s);
    }
    explorer_clamp_scroll(s);
}

int explorer_rows_per_page(const explorer_t* s) {
    int n = (explorer_grid_bottom(s) - NAV_Y) / ITEM_HEIGHT;
    return n < 1 ? 1 : n;
}

static void back_rect(gfx_rect_t* out) {
    // The border box, grown to cover the sprite
    int32_t x0 = BACK_BTN_BORDER_X, y0 = BACK_BTN_SPRITE_Y;
    int32_t x1 = BACK_BTN_BORDER_X + BACK_BTN_BORDER_W, y1 = BACK_BTN_BORDER_Y + BACK_BTN_BORDER_H;
    resource_info_t ri;
    if (explorer_back_icon() > 0 && resource_info(explorer_back_icon(), &ri) == 0) {
        x1 = max32(x1, BACK_BTN_SPRITE_X + (int32_t)ri.width);
        y1 = max32(y1, BACK_BTN_SPRITE_Y + (int32_t)ri.height);
    }
    *out = gfx_rect(x0, y0, x1 - x0, y1 - y0);
}

bool explorer_back_hit(int32_t x, int32_t y) {
    gfx_rect_t r;
    back_rect(&r);
    return in_rect(&r, x, y);
}

//
// Menu labels (the dwm draws the menus)
//

const char* explorer_action_label(action_t a) {
    switch (a) {
        case ACT_OPEN:              return "Open";
        case ACT_EDIT:              return "Edit";
        case ACT_OPEN_NEW_WINDOW:   return "Open in New Window";
        case ACT_CUT:               return "Cut";
        case ACT_COPY:              return "Copy";
        case ACT_RENAME:            return "Rename";
        case ACT_DELETE:            return "Delete";
        case ACT_FORMAT:            return "Format...";
        case ACT_ITEM_PROPERTIES:   return "Properties";
        case ACT_REFRESH:           return "Refresh";
        case ACT_NEW_FOLDER:        return "New Folder";
        case ACT_NEW_FILE:          return "New File";
        case ACT_PASTE:             return "Paste";
        case ACT_OPEN_CONSOLE:      return "Open Console Here";
        case ACT_FOLDER_PROPERTIES: return "Properties";
        case ACT_SORT_MENU:         return "Sort By";      // The dwm draws the ">"
        case ACT_SORT_NAME:         return "Name";
        case ACT_SORT_TYPE:         return "Type";
        case ACT_SORT_SIZE:         return "Size";
        case ACT_SORT_DATE:         return "Date Modified";
        case ACT_SORT_ASCENDING:    return "Ascending";
        case ACT_SORT_DESCENDING:   return "Descending";
        case ACT_DROP_MOVE:         return "Move here";
        case ACT_DROP_COPY:         return "Copy here";
        case ACT_DROP_CANCEL:       return "Cancel";
    }
    return "";
}

//
// Painting
//

static void draw_toolbar(explorer_t* s, gfx_surface_t* g) {
    int32_t w = s->win_width;
    int32_t fw = gfx_char_width(g);

    // Back button
    gfx_color_t fill = s->back_hover ? T.edit.selection : T.edit.background;
    gfx_fill_rect(g, BACK_BTN_CONTAINER_X, BACK_BTN_CONTAINER_Y, BACK_BTN_CONTAINER_W, BACK_BTN_CONTAINER_H, fill);
    gfx_draw_rect(g, BACK_BTN_BORDER_X, BACK_BTN_BORDER_Y, BACK_BTN_BORDER_W, BACK_BTN_BORDER_H, T.edit.border);
    if (explorer_back_icon() > 0)
        gfx_draw_resource(g, explorer_back_icon(), BACK_BTN_SPRITE_X, BACK_BTN_SPRITE_Y);
    else
        gfx_draw_text(g, BACK_BTN_BORDER_X + (BACK_BTN_BORDER_W - fw) / 2,
                      gfx_text_y(g, BACK_BTN_BORDER_Y, BACK_BTN_BORDER_H), "<", T.edit.text);

    // Path field
    int32_t field_w = max32(w - PATH_FIELD_BG_X - 10, 10);
    gfx_fill_rect(g, PATH_FIELD_BG_X, PATH_FIELD_BG_Y, field_w, PATH_FIELD_BG_H, T.edit.background);
    gfx_draw_rect(g, PATH_FIELD_BORDER_X, PATH_FIELD_BORDER_Y, field_w + 2, PATH_FIELD_BORDER_H, T.edit.border);

    gfx_save(g);
    gfx_rect_t clip = gfx_rect(PATH_FIELD_BG_X, PATH_FIELD_BG_Y, field_w, PATH_FIELD_BG_H);
    gfx_set_clip(g, &clip);

    // A path too long for the field shows its end, after "..."
    const char* path = s->full_path;
    size_t len = strlen(path);
    size_t fit = (size_t)max32((field_w - (PATH_TEXT_X - PATH_FIELD_BG_X) - 4) / fw, 4);
    size_t start = 0;
    int32_t x = PATH_TEXT_X;
    if (len > fit) {
        start = len - (fit - 3);
        gfx_draw_text(g, x, PATH_TEXT_Y, "...", T.edit.text);
        x += 3 * fw;
    }
    size_t split = s->in_mount ? s->mount_split : len;
    if (split > start) {
        gfx_draw_text_n(g, x, PATH_TEXT_Y, path + start, split - start, T.edit.text);
        x += (int32_t)(split - start) * fw;
    }
    size_t from = split > start ? split : start;
    if (from < len)
        gfx_draw_text(g, x, PATH_TEXT_Y, path + from, T.client.text_mount);
    gfx_restore(g);

    gfx_draw_hline(g, 0, NAV_DIVIDER_Y, w, T.client.divider_soft);
}

static void draw_fallback_icon(gfx_surface_t* g, bool is_dir, int32_t cx, int32_t top) {
    int32_t n = FALLBACK_ICON_SIZE, x = cx - n / 2;
    gfx_color_t c = is_dir ? FALLBACK_FOLDER_COLOR : T.client.text_soft;
    if (is_dir) {
        gfx_fill_rect(g, x, top + 2, n / 2, 6, c);
        gfx_fill_rect(g, x, top + 6, n, n - 8, c);
        gfx_draw_rect(g, x, top + 6, n, n - 8, T.edit.border);
    } else {
        gfx_fill_rect(g, x + 4, top, n - 8, n, c);
        gfx_draw_rect(g, x + 4, top, n - 8, n, T.edit.border);
        for (int32_t ly = top + 8; ly < top + n - 4; ly += 5)
            gfx_draw_hline(g, x + 8, ly, n - 16, T.edit.border);
    }
}

void explorer_place_rename(explorer_t* s) {
    gfx_rect_t tile;
    explorer_item_rect(s, s->rename_index, &tile);
    int32_t label_y = tile.y + NAV_Y_OFF + ITEM_TEXT_HEIGHT_OFF;
    ui_edit_place(&s->edit, tile.x + ITEM_WIDTH / 2, label_y, 30, MAX_RENAME_WIDTH,
                  gfx_rect(2, 0, s->win_width - 4, s->win_height));
}

static void draw_rename_box(explorer_t* s, gfx_surface_t* g) {
    ui_edit_style_t style = {
        .background = T.edit.background,
        .border     = T.button.border_active,
        .text       = T.edit.text,
        .cursor     = T.edit.cursor,
        .selection  = T.edit.selection,
    };
    explorer_place_rename(s);
    ui_edit_paint(&s->edit, g, &style);
}

// The selected item's whole name, wrapped, on a plate over the tiles below
static void draw_full_name(explorer_t* s, gfx_surface_t* g, int index) {
    gfx_rect_t tile;
    explorer_item_rect(s, index, &tile);
    int32_t fw = gfx_char_width(g), lh = gfx_line_height(g) + 1;
    size_t fit = (size_t)max32((ITEM_WIDTH - 4) / fw, 3);

    ui_line_t lines[NAME_LINES_MAX];
    bool more;
    int n = (int)ui_wrap_text(s->items[index].name, fit, lines, NAME_LINES_MAX, &more);
    int32_t label_y = tile.y + NAV_Y_OFF + ITEM_TEXT_HEIGHT_OFF;
    int32_t line_top = label_y - gfx_text_y(g, 0, lh - 1);     // A taller font's line starts higher

    gfx_rect_t p = gfx_rect(tile.x + 1, line_top - 3, tile.w - 2, n * lh + 5);
    gfx_fill_rect(g, p.x, p.y, p.w, p.h, T.client.background);     // Hides the tiles below
    gfx_blend_t old = gfx_set_blend(g, GFX_BLEND_ALPHA);
    gfx_fill_rect(g, p.x, p.y, p.w, p.h, T.desktop.icon_selected);
    gfx_set_blend(g, old);
    gfx_draw_rect(g, p.x, p.y, p.w, p.h, T.desktop.icon_selected_border);

    for (int i = 0; i < n; i++) {
        char line[NAME_MAX + 1];
        size_t len = ui_line_copy(&lines[i], more && i == n - 1, fit, line, sizeof(line));
        gfx_draw_text_n(g, tile.x + (ITEM_WIDTH - (int32_t)len * fw) / 2, label_y + i * lh, line, len, T.client.text);
    }
}

static void draw_grid(explorer_t* s, gfx_surface_t* g) {
    int32_t w = s->win_width, h = explorer_grid_bottom(s);
    int32_t fw = gfx_char_width(g);

    gfx_save(g);
    gfx_rect_t clip = gfx_rect(0, GRID_TOP, w, h - GRID_TOP);
    gfx_set_clip(g, &clip);

    if (s->total_items == 0) {
        const char* msg = "This folder is empty";
        gfx_draw_text(g, (w - (int32_t)strlen(msg) * fw) / 2, NAV_Y + 20, msg, T.client.text_muted);
    }

    int max_chars = (ITEM_WIDTH - 4) / fw;
    int full_name = -1;             // The focused selected item, when its name is cut
    for (int i = 0; i < s->total_items; i++) {
        gfx_rect_t tile;
        explorer_item_rect(s, i, &tile);
        if (tile.y + tile.h < GRID_TOP)
            continue;
        if (tile.y > h)
            break;

        const struct Item* item = &s->items[i];
        bool selected = s->picked[i];
        if (selected) {
            // As the desktop marks a selected icon
            gfx_blend_t old = gfx_set_blend(g, GFX_BLEND_ALPHA);
            gfx_fill_rect(g, tile.x + 2, tile.y + 2, tile.w - 4, tile.h - 4, T.desktop.icon_selected);
            gfx_set_blend(g, old);
            gfx_draw_rect(g, tile.x + 1, tile.y + 1, tile.w - 2, tile.h - 2, T.desktop.icon_selected_border);
        }

        int32_t icon_top = tile.y + NAV_Y_OFF;
        int handle = (item->icon > 0) ? item->icon : explorer_icon(item->icon_index);
        if (handle < 0 && item->icon_index == ICON_STORAGE)
            handle = explorer_icon(ICON_FOLDER);
        resource_info_t ri;
        if (handle > 0 && resource_info(handle, &ri) == 0)
            gfx_draw_resource(g, handle, tile.x + (ITEM_WIDTH - (int32_t)ri.width) / 2, icon_top);
        else
            draw_fallback_icon(g, item->is_dir, tile.x + ITEM_WIDTH / 2, icon_top + 4);

        int32_t label_y = icon_top + ITEM_TEXT_HEIGHT_OFF;
        if (s->renaming && i == s->rename_index)
            continue;           // The rename box goes over it, last

        // Names wider than the tile end in "..", unless selected: then all
        // of it shows, over the tiles below (drawn last)
        char label[NAME_MAX + 1];
        size_t len = strlen(item->name);
        if ((int)len > max_chars && selected && i == s->selected) {
            full_name = i;
            continue;
        }
        len = ui_fit_text(item->name, (size_t)max_chars, label, sizeof(label));
        int32_t tx = tile.x + (ITEM_WIDTH - (int32_t)len * fw) / 2;
        gfx_draw_text(g, tx, label_y, label, selected ? T.client.text : T.client.text_soft);
    }

    if (full_name >= 0)
        draw_full_name(s, g, full_name);
    if (s->renaming && s->rename_index >= 0 && s->rename_index < s->total_items)
        draw_rename_box(s, g);

    // Selection rectangle, as the desktop draws its own
    if (s->banding) {
        int32_t x0 = min32(s->band_x0, s->band_x1), x1 = max32(s->band_x0, s->band_x1);
        int32_t y0 = min32(s->band_y0, s->band_y1) - s->scroll_y;
        int32_t y1 = max32(s->band_y0, s->band_y1) - s->scroll_y;
        if (x1 > x0 && y1 > y0) {
            gfx_blend_t old = gfx_set_blend(g, GFX_BLEND_ALPHA);
            gfx_fill_rect(g, x0, y0, x1 - x0, y1 - y0, T.desktop.band_fill);
            gfx_draw_rect(g, x0, y0, x1 - x0, y1 - y0, T.desktop.band_border);
            gfx_set_blend(g, old);
        }
    }

    // Scroll position
    int32_t max_scroll = explorer_max_scroll(s);
    if (max_scroll > 0) {
        int32_t track = h - GRID_TOP - 4;
        int32_t view = h - GRID_TOP;
        int32_t thumb = max32(16, track * view / (view + max_scroll));
        int32_t ty = GRID_TOP + 2 + (track - thumb) * s->scroll_y / max_scroll;
        gfx_fill_rect(g, w - 5, ty, 3, thumb, T.button.border);
    }
    gfx_restore(g);
}

// 512 -> "512 bytes", 1234 -> "1.2 KB (1,234 bytes)", as prop counts (1 KB = 1,000 bytes)
static void format_size(uint64_t bytes, char* out, size_t size) {
    static const char* const units[] = { "KB", "MB", "GB", "TB" };
    char digits[24], commas[32];
    int n = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)bytes);
    size_t at = 0;
    for (int i = 0; i < n && at + 1 < sizeof(commas); i++) {
        if (i > 0 && (n - i) % 3 == 0)
            commas[at++] = ',';
        commas[at++] = digits[i];
    }
    commas[at] = '\0';
    if (bytes < 1000) {
        snprintf(out, size, "%s byte%s", commas, bytes == 1 ? "" : "s");
        return;
    }
    size_t unit = 0;
    uint64_t divisor = 1000;
    while (unit + 1 < sizeof(units) / sizeof(units[0]) && (bytes * 10 + divisor / 2) / divisor >= 10000) {
        divisor *= 1000;
        unit++;
    }
    uint64_t tenths = (bytes * 10 + divisor / 2) / divisor;
    snprintf(out, size, "%u.%u %s (%s bytes)", (unsigned)(tenths / 10), (unsigned)(tenths % 10),
             units[unit], commas);
}

// What the status bar says when no message shows: the selection (name and
// size of one file, how many and their size for several), else the folder
static void status_text(const explorer_t* s, char* out, size_t size) {
    int picked = 0, files = 0, other = -1;
    uint64_t bytes = 0;
    for (int i = 0; i < s->total_items; i++) {
        if (!s->picked[i])
            continue;
        picked++;
        if (!s->items[i].is_dir) {
            files++;
            bytes += s->items[i].size;
        }
        other = i;
    }
    char sz[64];
    format_size(bytes, sz, sizeof(sz));

    if (picked == 0) {
        snprintf(out, size, "%d item%s%s", s->total_items, s->total_items == 1 ? "" : "s",
                 s->truncated ? " shown" : "");
    } else if (picked == 1) {
        const struct Item* item = &s->items[s->selected >= 0 && s->picked[s->selected] ? s->selected : other];
        const char* what = item->is_dir ? (item->icon_index == ICON_STORAGE ? "drive" : "folder") : sz;
        snprintf(out, size, "%s %s", item->name, what);
    } else if (files == 0) {
        snprintf(out, size, "%d items selected", picked);
    } else {
        snprintf(out, size, "%d items selected %s%s", picked, sz, files < picked ? " in files" : "");
    }
}

// The status bar: a message while one shows, else status_text, cut to fit.
// The text is centred in the bar's lower NOTICE_H - 1 pixels: one pixel
// higher than it was on the 18 pixel bar.
static void draw_status(explorer_t* s, gfx_surface_t* g) {
    int32_t y = explorer_grid_bottom(s), fw = gfx_char_width(g);
    gfx_fill_rect(g, 0, y, s->win_width, s->win_height - y, T.button.fill);
    gfx_draw_hline(g, 0, y, s->win_width, T.button.border);

    char text[NOTICE_LEN + NAME_MAX], line[NOTICE_LEN + NAME_MAX];
    bool notice = s->notice[0] != '\0';
    if (notice)
        strlcpy(text, s->notice, sizeof(text));
    else
        status_text(s, text, sizeof(text));
    size_t fit = (size_t)max32((s->win_width - 16) / fw, 4);
    ui_fit_text(text, fit, line, sizeof(line));
    gfx_draw_text(g, 8, gfx_text_y(g, y + 1, NOTICE_H - 1), line, notice ? T.client.text : T.client.text_soft);
}

void explorer_draw(explorer_t* s) {
    gfx_surface_t* g = wm_window_surface(s->handle);
    if (g == NULL)
        return;
    s->dirty = false;

    gfx_set_clip(g, NULL);
    gfx_set_blend(g, GFX_BLEND_COPY);
    gfx_clear(g, T.client.background);

    draw_grid(s, g);
    draw_toolbar(s, g);
    draw_status(s, g);

    if (wm_window_commit(s->handle, NULL) < 0)
        fprintf(stderr, "explorer: drawing failed: %s\n", strerror(errno));
}
