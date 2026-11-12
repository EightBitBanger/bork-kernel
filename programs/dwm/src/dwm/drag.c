/*
 * Drag and drop of files (drag.h): the paths being dragged or just
 * dropped, delivering the drop, and the plate by the pointer while a drag
 * is on. input.c decides when a drag starts and ends; the window server
 * (server.c) copies the paths in from and out to programs.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <os/ipc.h>

#include "compositor.h"
#include "dirty.h"
#include "drag.h"
#include "internal.h"
#include "theme.h"
#include "timer.h"

#define BADGE_MAX           (DWM_ICON_LABEL_MAX + 16)
#define BADGE_PAD_X         6
#define BADGE_PAD_Y         3
#define BADGE_OFFSET_X      16      // From the pointer's hot spot
#define BADGE_OFFSET_Y      20

static dwm_clip_info_t current;         // type DWM_CLIP_NONE: nothing dragged yet
static char*           data;            // size + 1 bytes (a 0 after the paths)
static uint32_t        serial;
static size_t          paths;           // Lines in data

static char            desktop_folder[DWM_DROP_FOLDER_MAX];   // "": not known

static char            badge[BADGE_MAX];
static gfx_rect_t      badge_rect;      // Empty: not showing

//
// The paths
//

// Every line an absolute path, none empty
static size_t count_paths(const char* list, uint32_t size) {
    size_t n = 0;
    const char* p = list;
    const char* stop = list + size;
    while (p < stop) {
        const char* end = (const char*)memchr(p, '\n', (size_t)(stop - p));
        size_t len = end ? (size_t)(end - p) : (size_t)(stop - p);
        if (len == 0 || p[0] != '/' || memchr(p, '\0', len) != NULL)
            return 0;
        n++;
        if (end == NULL)
            break;
        p = end + 1;
    }
    return n;
}

bool drag_set_from(const dwm_clip_info_t* info, const void* bytes, uint32_t owner) {
    if (info == NULL || info->type != DWM_CLIP_FILES || info->size == 0 || info->size > DWM_CLIP_MAX ||
        bytes == NULL) {
        errno = EINVAL;
        return false;
    }
    char* copy = (char*)malloc((size_t)info->size + 1);
    if (copy == NULL) {
        errno = ENOMEM;
        return false;
    }
    memcpy(copy, bytes, info->size);
    copy[info->size] = '\0';
    size_t n = count_paths(copy, info->size);
    if (n == 0) {
        free(copy);
        errno = EINVAL;
        return false;
    }

    free(data);
    data  = copy;
    paths = n;
    memset(&current, 0, sizeof(current));
    current.type  = DWM_CLIP_FILES;
    current.flags = info->flags & DWM_CLIP_CUT;
    current.size  = info->size;
    current.owner = owner;
    serial++;
    if (serial == 0)
        serial = 1;
    current.serial = serial;
    return true;
}

static bool set_paths(const char* const* list, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (list[i] == NULL || list[i][0] != '/' || strchr(list[i], '\n') != NULL) {
            errno = EINVAL;
            return false;
        }
        total += strlen(list[i]) + 1;
    }
    if (count == 0 || total > DWM_CLIP_MAX) {
        errno = EINVAL;
        return false;
    }
    char* joined = (char*)malloc(total);
    if (joined == NULL) {
        errno = ENOMEM;
        return false;
    }
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(list[i]);
        memcpy(joined + at, list[i], n);
        at += n;
        if (i + 1 < count)
            joined[at++] = '\n';
    }
    dwm_clip_info_t info;
    memset(&info, 0, sizeof(info));
    info.type = DWM_CLIP_FILES;
    info.size = (uint32_t)at;
    bool ok = drag_set_from(&info, joined, 0);
    free(joined);
    return ok;
}

uint32_t drag_serial(void) {
    return current.type == DWM_CLIP_NONE ? 0 : serial;
}

size_t drag_path_count(void) {
    return current.type == DWM_CLIP_NONE ? 0 : paths;
}

const char* dwm_drop_data(uint32_t which, dwm_clip_info_t* info) {
    if (current.type == DWM_CLIP_NONE || which == 0 || which != serial) {
        if (info != NULL)
            memset(info, 0, sizeof(*info));
        return NULL;
    }
    if (info != NULL)
        *info = current;
    return data;
}

int drag_share(uint32_t which) {
    if (dwm_drop_data(which, NULL) == NULL) {
        errno = ENOENT;
        return -1;
    }
    // A fresh block for the one program, as the clipboard does
    int shm = shm_create(current.size);
    if (shm < 0)
        return -1;
    void* p = shm_map(shm);
    if (p == NULL) {
        ipc_close(shm);
        return -1;
    }
    memcpy(p, data, current.size);
    shm_unmap(shm);
    return shm;
}

//
// Drives: a move only happens within one
//

#define DRIVES_DIR          "/mnt"

// How long the drive part of `path` is: "/mnt/<drive>" for a path on a
// drive, 0 for the system's own folders
static size_t drive_prefix(const char* path) {
    if (strncmp(path, DRIVES_DIR "/", sizeof(DRIVES_DIR)) != 0)
        return 0;
    const char* slash = strchr(path + sizeof(DRIVES_DIR), '/');
    return (slash != NULL) ? (size_t)(slash - path) : strlen(path);
}

bool dwm_same_drive(const char* a, const char* b) {
    size_t n = drive_prefix(a);
    return n == drive_prefix(b) && strncmp(a, b, n) == 0;
}

int drag_other_drive(const char* folder) {
    if (current.type == DWM_CLIP_NONE || folder == NULL || folder[0] != '/')
        return -1;
    char path[DWM_DROP_FOLDER_MAX];
    for (const char* at = data; *at != '\0'; ) {
        const char* end = strchr(at, '\n');
        size_t len = (end != NULL) ? (size_t)(end - at) : strlen(at);
        // Only the drive part is compared: a longer path is cut, not skipped
        if (len >= sizeof(path))
            len = sizeof(path) - 1;
        memcpy(path, at, len);
        path[len] = '\0';
        if (!dwm_same_drive(path, folder))
            return 1;
        if (end == NULL)
            break;
        at = end + 1;
    }
    return 0;
}

void dwm_set_drop_folder(dwm_handle_t window, const char* folder) {
    char* to;
    if (window == DWM_DESKTOP) {
        to = desktop_folder;
    } else {
        dwm_window_t* w = window_get(window);
        if (w == NULL)
            return;
        to = w->drop_folder;
    }
    char old[DWM_DROP_FOLDER_MAX];
    strlcpy(old, to, sizeof(old));
    to[0] = '\0';
    if (folder != NULL && folder[0] == '/')
        strlcpy(to, folder, DWM_DROP_FOLDER_MAX);
    if (strcmp(old, to) != 0)
        input_drag_refresh();       // An answer to DRAG_OVER: the plate names it
}

const char* dwm_drop_folder(dwm_handle_t window) {
    if (window == DWM_DESKTOP)
        return desktop_folder;
    dwm_window_t* w = window_get(window);
    return (w != NULL) ? w->drop_folder : "";
}

//
// Dropping
//

static void post_drop(dwm_handle_t target, int32_t x, int32_t y, uint8_t mods, uint8_t button,
                      uint32_t icon) {
    dwm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type     = DWM_EVENT_DROP;
    ev.x        = x;
    ev.y        = y;
    ev.mods     = mods;
    ev.button   = button;
    ev.time_ms  = (uint32_t)dwm_time_ms();
    ev.param[0] = icon;
    ev.param[1] = serial;
    dwm_post(target, &ev);
}

void drag_drop_at(int32_t x, int32_t y, uint8_t mods, uint8_t button) {
    if (current.type == DWM_CLIP_NONE)
        return;
    dwm_window_t* w = window_top_visible_at(x, y);
    if (w == NULL) {
        post_drop(DWM_DESKTOP, x, y, mods, button, icons_drop_target_at(x, y, false));
        return;
    }
    gfx_rect_t client = window_client(w);
    post_drop(w->handle, x - client.x, y - client.y, mods, button, 0);
}

bool dwm_drop_files(dwm_handle_t window, int32_t x, int32_t y, uint8_t mods, uint8_t button,
                    const char* const* list, size_t count) {
    dwm_window_t* w = window_get(window);
    if (w == NULL) {
        errno = EBADF;
        return false;
    }
    if (!set_paths(list, count))
        return false;
    gfx_rect_t client = window_client(w);
    post_drop(window, x - client.x, y - client.y, mods, button, 0);
    return true;
}

//
// The plate by the pointer
//

static gfx_rect_t badge_at(int32_t x, int32_t y) {
    int32_t w = dwm_text_width(badge, strlen(badge)) + 2 * BADGE_PAD_X;
    int32_t h = dwm_text_height() + 2 * BADGE_PAD_Y;
    int32_t bx = x + BADGE_OFFSET_X, by = y + BADGE_OFFSET_Y;
    int32_t sw = compositor_width(), sh = compositor_height();
    if (bx + w > sw) bx = sw - w;
    if (by + h > sh) by = y - h - 4;        // Above the pointer near the bottom
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    return gfx_rect(bx, by, w, h);
}

void drag_badge(const char* text, int32_t x, int32_t y) {
    if (!rect_empty(badge_rect))
        compositor_invalidate(badge_rect);
    if (text == NULL || text[0] == '\0') {
        badge[0] = '\0';
        badge_rect = gfx_rect(0, 0, 0, 0);
        return;
    }
    strlcpy(badge, text, sizeof(badge));
    badge_rect = badge_at(x, y);
    compositor_invalidate(badge_rect);
}

void drag_paint(gfx_surface_t* s, gfx_rect_t area) {
    if (rect_empty(badge_rect) || !rect_overlaps(area, badge_rect))
        return;
    const dwm_theme_menu_t* t = &dwm_theme()->menu;
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_rect_t r = badge_rect;
    gfx_fill_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, t->background);
    gfx_draw_rect(s, r.x, r.y, r.w, r.h, t->border);
    dwm_text_draw(s, r.x + BADGE_PAD_X, r.y + BADGE_PAD_Y, badge, strlen(badge), t->text);
    gfx_set_blend(s, old);
}
