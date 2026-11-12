/*
 * Desktop icon layout (layout.h): <home>/usr/icons.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <os/os.h>

#include "dwm/dwm.h"
#include "layout.h"

typedef struct {
    char*   path;           // malloc'd
    int32_t x, y;
} spot_t;

static spot_t spots[LAYOUT_MAX];
static size_t spot_count;

// "<home>/usr" into dir, "<home>/usr/icons" into file (either may be NULL)
static int layout_paths(char* dir, char* file, size_t size) {
    char home[PATH_MAX];
    if (os_gethome(home, sizeof(home)) == NULL)
        return -1;
    int n = 0;
    if (dir != NULL)
        n = snprintf(dir, size, "%s/%s", home, LAYOUT_SUB);
    if (n >= 0 && (size_t)n < size && file != NULL)
        n = snprintf(file, size, "%s/%s/%s", home, LAYOUT_SUB, LAYOUT_FILE);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int find(const char* path) {
    for (size_t i = 0; i < spot_count; i++)
        if (strcmp(spots[i].path, path) == 0)
            return (int)i;
    return -1;
}

void layout_free(void) {
    for (size_t i = 0; i < spot_count; i++)
        free(spots[i].path);
    spot_count = 0;
}

static void add_spot(const char* path, int32_t x, int32_t y) {
    if (spot_count == LAYOUT_MAX || find(path) >= 0)
        return;
    char* copy = strdup(path);
    if (copy == NULL)
        return;
    spots[spot_count].path = copy;
    spots[spot_count].x = x;
    spots[spot_count].y = y;
    spot_count++;
}

int layout_load(void) {
    layout_free();

    char file[PATH_MAX];
    if (layout_paths(NULL, file, sizeof(file)) < 0)
        return -1;

    // No layout but a finished "icons.tmp": the last save was cut off between
    // writing it and renaming it over the old one. It is the newest layout.
    FILE* f = fopen(file, "r");
    if (f == NULL && errno == ENOENT) {
        char tmp[PATH_MAX + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", file);
        f = fopen(tmp, "r");
        if (f == NULL)
            return (errno == ENOENT) ? 0 : -1;  // First start: nothing saved yet
        fclose(f);
        if (rename(tmp, file) == 0)
            os_sync();                          // Put right for good
        f = fopen(file, "r");
        if (f == NULL)
            f = fopen(tmp, "r");                // Couldn't rename: read it where it is
    }
    if (f == NULL)
        return -1;

    char* line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;

        // "x y path": two numbers, then an absolute path to the line's end
        char* p = line;
        char* end;
        long x = strtol(p, &end, 10);
        if (end == p) continue;
        p = end;
        long y = strtol(p, &end, 10);
        if (end == p) continue;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '/' || x < -GFX_COORD_MAX || x > GFX_COORD_MAX || y < -GFX_COORD_MAX || y > GFX_COORD_MAX)
            continue;
        add_spot(p, (int32_t)x, (int32_t)y);
    }
    free(line);
    fclose(f);
    return 0;
}

void layout_rename(const char* old_path, const char* new_path) {
    size_t old_len = strlen(old_path);
    for (size_t i = 0; i < spot_count; i++) {
        const char* p = spots[i].path;
        if (strncmp(p, old_path, old_len) != 0 || (p[old_len] != '\0' && p[old_len] != '/'))
            continue;
        char moved[PATH_MAX];
        int n = snprintf(moved, sizeof(moved), "%s%s", new_path, p + old_len);
        char* copy = (n >= 0 && (size_t)n < sizeof(moved)) ? strdup(moved) : NULL;
        if (copy == NULL)
            continue;
        free(spots[i].path);
        spots[i].path = copy;
    }
}

bool layout_find(const char* path, int32_t* x, int32_t* y) {
    int i = (path != NULL) ? find(path) : -1;
    if (i < 0)
        return false;

    // Keep it on the screen (the resolution may have changed since)
    int32_t max_x = dwm_screen_width()  - DWM_ICON_WIDTH;
    int32_t max_y = dwm_work_area().h - DWM_ICON_GRID_Y;      // Above the taskbar
    int32_t px = spots[i].x, py = spots[i].y;
    if (px > max_x) px = max_x;
    if (py > max_y) py = max_y;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    *x = px;
    *y = py;
    return true;
}

// A saved place for an icon that isn't showing is worth keeping only when
// it can come back: its device (/mnt/<device>, the path's first two parts)
// is unplugged, or it is still there (the desktop was full, say). A path
// that is gone from a device that is present was deleted or moved away.
static bool may_come_back(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0 || errno != ENOENT)
        return true;
    const char* end = strchr(path + 1, '/');
    if (end == NULL)
        return true;                    // A device itself: unplugged
    end = strchr(end + 1, '/');
    if (end == NULL)
        return true;                    // Ditto
    char device[PATH_MAX];
    size_t len = (size_t)(end - path);
    if (len >= sizeof(device))
        return true;
    memcpy(device, path, len);
    device[len] = '\0';
    return stat(device, &st) < 0;
}

void layout_capture(void) {
    spot_t next[LAYOUT_MAX];
    size_t n = 0;

    // The icons showing now, then what was saved for icons that aren't
    for (size_t i = 0; i < dwm_icon_count() && n < LAYOUT_MAX; i++) {
        dwm_icon_t icon = dwm_icon_at_index(i);
        const char* path = (const char*)dwm_icon_user(icon);
        gfx_rect_t r;
        if (path == NULL || !dwm_icon_get_rect(icon, &r))
            continue;
        char* copy = strdup(path);
        if (copy == NULL)
            continue;
        next[n].path = copy;
        next[n].x = r.x;
        next[n].y = r.y;
        n++;
    }
    size_t shown = n;

    for (size_t i = 0; i < spot_count; i++) {
        bool showing = false;
        for (size_t j = 0; j < shown && !showing; j++)
            showing = strcmp(next[j].path, spots[i].path) == 0;
        if (!showing && n < LAYOUT_MAX && may_come_back(spots[i].path))
            next[n++] = spots[i];       // Moves the string over
        else
            free(spots[i].path);
    }

    memcpy(spots, next, n * sizeof(spot_t));
    spot_count = n;
}

int layout_save(void) {
    layout_capture();

    char dir[PATH_MAX], file[PATH_MAX], tmp[PATH_MAX + 8];
    if (layout_paths(dir, file, sizeof(file)) < 0)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);

    // <home>/usr may not exist yet
    struct stat st;
    if (stat(dir, &st) < 0 && (errno != ENOENT || mkdir(dir, 0755) < 0))
        return -1;

    FILE* f = fopen(tmp, "w");
    if (f == NULL)
        return -1;
    fprintf(f, "# Desktop icon positions: x y path\n");
    for (size_t i = 0; i < spot_count; i++) {
        if (strchr(spots[i].path, '\n') != NULL)
            continue;       // Can't be written as one line
        fprintf(f, "%d %d %s\n", (int)spots[i].x, (int)spots[i].y, spots[i].path);
    }

    bool failed = ferror(f) != 0;
    int err = errno;
    if (fclose(f) != 0 && !failed) {
        failed = true;
        err = errno;
    }
    if (failed) {
        remove(tmp);
        errno = err;
        return -1;
    }

    // Replace the old layout only once the new one is complete. A failed
    // rename may already have removed the old "icons", so the new one stays
    // as "icons.tmp": layout_load() picks it up from there.
    return rename(tmp, file) < 0 ? -1 : 0;
}
