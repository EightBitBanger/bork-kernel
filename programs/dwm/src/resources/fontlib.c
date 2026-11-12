/*
 * Font library (fontlib.h): a sorted table of reference names and the
 * fonts opened under them. The fonts themselves (checking, stb_truetype,
 * rendering) are libc's os/font.h.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <os/filetype.h>

#include "fontlib.h"

typedef struct {
    char    name[FONT_NAME_MAX];
    font_t* font;
} entry_t;

static entry_t* fonts    = NULL;
static size_t   count    = 0;
static size_t   capacity = 0;

// Where `name` is, or where it would go (*found says which)
static size_t find(const char* name, bool* found) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(fonts[mid].name, name);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0) lo = mid + 1;
        else       hi = mid;
    }
    *found = false;
    return lo;
}

int fontlib_load_file(const char* path, const char* name) {
    if (path == NULL || name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strlen(name) >= FONT_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    bool found;
    size_t at = find(name, &found);
    if (found) {
        errno = EEXIST;
        return -1;
    }

    if (count == capacity) {
        size_t grow = capacity ? capacity * 2 : 8;
        entry_t* p = (entry_t*)realloc(fonts, grow * sizeof(entry_t));
        if (p == NULL)
            return -1;
        fonts = p;
        capacity = grow;
    }

    font_t* font = font_open_file(path);
    if (font == NULL)
        return -1;

    memmove(&fonts[at + 1], &fonts[at], (count - at) * sizeof(entry_t));
    memset(&fonts[at], 0, sizeof(entry_t));
    strcpy(fonts[at].name, name);
    fonts[at].font = font;
    count++;
    return 0;
}

static size_t skipped_count;
static char   skipped_first[NAME_MAX + 1];

static void skip(const char* name) {
    if (skipped_count++ == 0)
        strlcpy(skipped_first, name, sizeof(skipped_first));
}

size_t fontlib_skipped(const char** first) {
    if (first != NULL)
        *first = skipped_first;
    return skipped_count;
}

// A font file is known by its first four bytes, not its name (the same
// versions font_open accepts): TrueType 1.0, Apple "true", OpenType with CFF
// outlines "OTTO", and a collection "ttcf"
static bool font_header(const uint8_t* h, size_t n) {
    static const char magic[][4] = {
        { 0x00, 0x01, 0x00, 0x00 }, { 't', 'r', 'u', 'e' },
        { 'O', 'T', 'T', 'O' },     { 't', 't', 'c', 'f' },
    };
    if (n < 4)
        return false;
    for (size_t i = 0; i < sizeof(magic) / sizeof(magic[0]); i++)
        if (memcmp(h, magic[i], 4) == 0)
            return true;
    return false;
}

// Its reference name: the file name without its extension, if it has one
// ("opensans.ttf" and "opensans" are both "opensans"). False when no room.
static bool font_file_name(const char* file, char* out, size_t size) {
    const char* dot = strrchr(file, '.');
    size_t n = (dot != NULL && dot != file) ? (size_t)(dot - file) : strlen(file);
    if (n == 0 || n >= size)
        return false;
    memcpy(out, file, n);
    out[n] = '\0';
    return true;
}

int fontlib_load_dir(const char* dir) {
    skipped_count = 0;
    skipped_first[0] = '\0';

    DIR* d = opendir(dir);
    if (d == NULL)
        return -1;

    int loaded = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR)
            continue;

        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            skip(e->d_name);        // Path too long
            continue;
        }

        uint8_t header[4];
        int got = filetype_read_header(path, header, sizeof(header));
        if (got < 0 || !font_header(header, (size_t)got))
            continue;       // Not a font file: not counted as skipped

        char name[FONT_NAME_MAX];
        if (!font_file_name(e->d_name, name, sizeof(name))) {
            skip(e->d_name);        // Name too long
            continue;
        }

        if (fontlib_load_file(path, name) == 0)
            loaded++;
        else
            skip(e->d_name);
    }
    closedir(d);
    return loaded;
}

font_t* fontlib_get(const char* name) {
    if (name == NULL)
        return NULL;
    bool found;
    size_t at = find(name, &found);
    return found ? fonts[at].font : NULL;
}

size_t fontlib_count(void) {
    return count;
}

font_t* fontlib_at(size_t index) {
    return index < count ? fonts[index].font : NULL;
}

const char* fontlib_name(size_t index) {
    return index < count ? fonts[index].name : NULL;
}

const char* fontlib_name_of(const font_t* font) {
    for (size_t i = 0; font != NULL && i < count; i++)
        if (fonts[i].font == font)
            return fonts[i].name;
    return NULL;
}

void fontlib_free(void) {
    for (size_t i = 0; i < count; i++)
        font_close(fonts[i].font);
    free(fonts);
    fonts = NULL;
    count = capacity = 0;
}
