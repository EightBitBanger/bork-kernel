// In-place rename of desktop icons.
//
// Works like the rename box in the file explorer: an edit box replaces the
// icon's label, typed characters go into it, Enter (0x02) or clicking
// anywhere outside the box commits, Esc cancels and Backspace (0x01) deletes.
//
// The desktop is not a window, so this cannot use the window edit fields.
// It keeps its own small state here and is drawn by dwm_draw_desktop.
// Keyboard input reaches it through dwm_desktop_rename_key, which the DWM
// thread calls before posting a key to the focused window.

#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/events.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/display.h>

#include <kernel/util/string.h>
#include <kernel/util/list.h>

// Same look and limits as the explorer rename box (see draw of
// WindowEditField in dwm_draw.c and MAX_RENAME_WIDTH in explorer.h)
#define RENAME_FONT_W        6
#define RENAME_FONT_H        8
#define RENAME_BOX_H         18
#define RENAME_MIN_W         30
#define RENAME_MAX_W         140
#define RENAME_LABEL_OFF_Y   45     // Label y offset used by dwm_draw_desktop

#define RENAME_COLOR_BG      (theme.edit.background)
#define RENAME_COLOR_BORDER  (theme.edit.border)
#define RENAME_COLOR_TEXT    (theme.edit.text)
#define RENAME_COLOR_CURSOR  (theme.edit.cursor)

#define RENAME_KEY_BACKSPACE 0x01
#define RENAME_KEY_ENTER     0x02
#define RENAME_KEY_ESCAPE    0x1B

#define RENAME_TEXT_MAX      128

static struct {
    struct IconObject* icon;        // NULL when no rename is active
    char     text[RENAME_TEXT_MAX];
    uint16_t cursor;
} rename_state = { NULL, {0}, 0 };

static uint16_t rename_name_limit(void) {
    uint16_t limit = RENAME_TEXT_MAX - 1;
    if (VFS_NAME_MAX < limit) limit = VFS_NAME_MAX;
    return limit;
}

// Inner box (without its 1px border) for a given text length
static void rename_box_rect(const struct IconObject* icon, size_t text_len,
                            int* out_x, int* out_y, int* out_w, int* out_h) {
    int w = (int)text_len * RENAME_FONT_W;
    if (w < RENAME_MIN_W) w = RENAME_MIN_W;
    if (w > RENAME_MAX_W) w = RENAME_MAX_W;
    
    int x = icon->x + ((int)icon->width - w) / 2;
    int y = icon->y + RENAME_LABEL_OFF_Y - ((RENAME_BOX_H - RENAME_FONT_H) / 2);
    
    // Keep the box (and its border) on screen
    int display_w = display_get_width();
    if (x < 1) x = 1;
    if (x + w + 1 > display_w) x = display_w - w - 1;
    
    *out_x = x;
    *out_y = y;
    *out_w = w;
    *out_h = RENAME_BOX_H;
}

// Repaint the largest area the box can ever cover, so a shrinking box leaves
// nothing behind
static void rename_invalidate_box(const struct IconObject* icon) {
    int x, y, w, h;
    rename_box_rect(icon, RENAME_TEXT_MAX, &x, &y, &w, &h);
    dwm_invalidate_region(x - 1, y - 1, w + 2, h + 2);
}

static void rename_invalidate_label(const struct IconObject* icon) {
    dwm_invalidate_region(icon->x + icon->bounds_x, icon->y + icon->bounds_y,
                          icon->bounds_w, icon->bounds_h);
}

static void rename_end(void) {
    struct IconObject* icon = rename_state.icon;
    if (icon == NULL) return;
    
    rename_invalidate_box(icon);
    rename_invalidate_label(icon);
    
    rename_state.icon = NULL;
    rename_state.text[0] = '\0';
    rename_state.cursor = 0;
}

struct IconObject* dwm_desktop_rename_icon(void) {
    return rename_state.icon;
}

bool dwm_desktop_rename_active(void) {
    return rename_state.icon != NULL;
}

void dwm_desktop_rename_cancel(void) {
    rename_end();
}

// Forget the rename if its icon is about to be freed
void dwm_desktop_rename_forget_icon(struct IconObject* icon) {
    if (icon != NULL && rename_state.icon == icon) rename_end();
}

bool dwm_desktop_rename_begin(struct IconObject* icon) {
    if (icon == NULL) return false;
    
    // Only one rename at a time: finish the previous one first
    if (rename_state.icon != NULL) {
        if (rename_state.icon == icon) return true;
        dwm_desktop_rename_commit();
    }
    
    // Mount icons are device nodes rather than files in a file system
    if (vfs_directory_check_mounted(icon->path)) {
        dwm_notify("Rename", "Storage devices cannot be renamed", DWM_NOTIFY_DEFAULT_MS);
        return false;
    }
    
    memset(rename_state.text, '\0', sizeof(rename_state.text));
    strncpy(rename_state.text, icon->name, rename_name_limit());
    rename_state.cursor = (uint16_t)strlen(rename_state.text);
    rename_state.icon = icon;
    
    // Drop any pending click/drag on the icon
    if (dragdrop.dragged_icon == icon) dragdrop.dragged_icon = NULL;
    
    rename_invalidate_label(icon);
    rename_invalidate_box(icon);
    return true;
}

bool dwm_desktop_rename_commit(void) {
    struct IconObject* icon = rename_state.icon;
    if (icon == NULL) return false;
    
    char new_name[RENAME_TEXT_MAX];
    strncpy(new_name, rename_state.text, sizeof(new_name) - 1);
    new_name[sizeof(new_name) - 1] = '\0';
    
    // Clear the edit state before touching the icon so the old label and box
    // get repainted
    rename_end();
    
    // Empty or unchanged name: nothing to do 
    if (new_name[0] == '\0' || strcmp(new_name, icon->name) == 0)
        return true;
    
    bool has_slash = false;
    for (const char* c = new_name; *c != '\0'; c++) {
        if (*c == '/') { has_slash = true; break; }
    }
    
    if (has_slash || !vfs_rename(icon->path, new_name)) {
        // A notification doesn't interrupt anything, so this can come back
        dwm_notify("Rename", "Name is invalid, too long or taken", DWM_NOTIFY_LONG_MS);
        return false;
    }
    
    // Rebuild the icon's path: same parent directory, new last segment
    char new_path[DWM_MAX_PATH_LEN];
    memset(new_path, '\0', sizeof(new_path));
    strncpy(new_path, icon->path, sizeof(new_path) - 1);
    
    char* last_slash = strrchr(new_path, '/');
    if (last_slash != NULL) {
        last_slash[1] = '\0';
    } else {
        new_path[0] = '\0';
    }
    strncat(new_path, new_name, sizeof(new_path) - strlen(new_path) - 1);
    
    // Old label area, then the icon's new name, path and bounds
    rename_invalidate_label(icon);
    
    strncpy(icon->name, new_name, DWM_MAX_NAME_LEN - 1);
    icon->name[DWM_MAX_NAME_LEN - 1] = '\0';
    strncpy(icon->path, new_path, DWM_MAX_PATH_LEN - 1);
    icon->path[DWM_MAX_PATH_LEN - 1] = '\0';
    
    dwm_calculate_icon_bounds(icon);
    rename_invalidate_label(icon);
    
    // Let open explorer windows pick up the new name
    dwm_refresh_all_windows();
    return true;
}

bool dwm_desktop_rename_key(uint16_t key) {
    DWM_GUARD();
    if (rename_state.icon == NULL) return false;
    
    // Keys beyond one byte (arrows, function keys) are swallowed while
    // renaming so they do not reach a window behind the desktop
    if (key > 0xFF) return true;
    
    uint8_t ch = (uint8_t)key;
    size_t len = strlen(rename_state.text);
    if (rename_state.cursor > len) rename_state.cursor = (uint16_t)len;
    
    switch (ch) {
    case RENAME_KEY_ENTER:
        dwm_desktop_rename_commit();
        return true;
    
    case RENAME_KEY_ESCAPE:
        rename_end();
        return true;
    
    case RENAME_KEY_BACKSPACE:
        if (rename_state.cursor > 0) {
            memmove(&rename_state.text[rename_state.cursor - 1],
                    &rename_state.text[rename_state.cursor],
                    len - rename_state.cursor + 1);
            rename_state.cursor--;
            rename_invalidate_box(rename_state.icon);
        }
        return true;
    }
    
    // Printable ASCII only; '/' would turn the name into a path
    if (ch < 0x20 || ch > 0x7E || ch == '/') return true;
    
    // Same cap the explorer applies (what the file system can store)
    if (len >= rename_name_limit()) return true;
    
    memmove(&rename_state.text[rename_state.cursor + 1],
            &rename_state.text[rename_state.cursor],
            len - rename_state.cursor + 1);
    rename_state.text[rename_state.cursor] = (char)ch;
    rename_state.cursor++;
    
    rename_invalidate_box(rename_state.icon);
    return true;
}

// A new click while renaming: inside the box it is swallowed, anywhere else
// it commits the rename and the click carries on as normal
bool dwm_desktop_rename_handle_click(const struct WindowContext* ctx) {
    if (rename_state.icon == NULL) return false;
    
    int x, y, w, h;
    rename_box_rect(rename_state.icon, strlen(rename_state.text), &x, &y, &w, &h);
    
    if (ctx->mouse.x >= x - 1 && ctx->mouse.x <= x + w &&
        ctx->mouse.y >= y - 1 && ctx->mouse.y <= y + h) {
        return true;
    }
    
    dwm_desktop_rename_commit();
    return false;
}

void dwm_desktop_rename_draw(const struct WindowContext* ctx) {
    struct IconObject* icon = rename_state.icon;
    if (icon == NULL) return;
    
    size_t len = strlen(rename_state.text);
    int box_x, box_y, box_w, box_h;
    rename_box_rect(icon, len, &box_x, &box_y, &box_w, &box_h);
    
    bool dirty = false;
    for (int i = 0; i < ctx->dirty_count; i++) {
        struct Rect r = ctx->dirty_regions[i];
        if (rects_intersect(r.x, r.y, r.w, r.h, box_x - 1, box_y - 1, box_w + 2, box_h + 2)) {
            dirty = true;
            break;
        }
    }
    if (!dirty) return;
    
    GfxContext* gc = dwm_screen_context();
    
    gfx_fill_rect(gc, box_x, box_y, box_w, box_h, RENAME_COLOR_BG);
    gfx_draw_rect(gc, box_x - 1, box_y - 1, box_w + 2, box_h + 2, RENAME_COLOR_BORDER);
    
    // Text that fits is centered like the explorer box. Longer text scrolls
    // so the cursor stays visible.
    int visible = (box_w - 4) / RENAME_FONT_W;
    size_t first = 0;
    int text_x;
    
    if ((int)len * RENAME_FONT_W <= box_w) {
        text_x = box_x + (box_w - (int)len * RENAME_FONT_W) / 2;
    } else {
        text_x = box_x + 2;
        if ((int)rename_state.cursor > visible)
            first = rename_state.cursor - visible;
    }
    
    char shown[RENAME_TEXT_MAX];
    memset(shown, '\0', sizeof(shown));
    strncpy(shown, &rename_state.text[first], (size_t)visible + 1);
    
    int text_y = box_y + (box_h - RENAME_FONT_H) / 2;
    gfx_draw_text(gc, text_x, text_y, shown, RENAME_COLOR_TEXT);
    
    // Two pixel wide cursor, same as the window edit fields
    int cursor_x = text_x + (int)(rename_state.cursor - first) * RENAME_FONT_W;
    gfx_fill_rect(gc, cursor_x, text_y, 2, RENAME_FONT_H + 1, RENAME_COLOR_CURSOR);
}

