/*
 * Image library (imagelib.h).
 *
 * Each file is decoded with libc's os/image.h (stb_image: PNG, JPEG, BMP,
 * GIF, TGA, PSD, PIC, PNM), told by its first bytes, and only the
 * 0xAARRGGBB pixels are kept. The table is kept sorted by name, so lookups
 * are a binary search.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <os/image.h>

#include "imagelib.h"

_Static_assert(IMAGELIB_MAX_SIDE == BITMAP_MAX_SIDE && IMAGELIB_MAX_PIXELS == BITMAP_MAX_PIXELS &&
               IMAGELIB_MAX_FILE == BITMAP_MAX_FILE, "imagelib's limits are os/image.h's");

static image_t* images   = NULL;
static size_t   count    = 0;
static size_t   capacity = 0;

//
// Table
//

// Where `name` is, or where it would go (*found says which)
static size_t find(const char* name, bool* found) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(images[mid].name, name);
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

//
// Decoding
//

int imagelib_decode_memory(const void* data, size_t size, image_t* out) {
    if (data == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    bitmap_t b;
    if (bitmap_decode(data, size, &b) < 0)
        return -1;
    out->width  = b.width;
    out->height = b.height;
    out->pixels = b.pixels;
    return 0;
}

const char* imagelib_decode_reason(void) {
    return bitmap_error();
}

int imagelib_decode_file(const char* path, image_t* out) {
    if (path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    bitmap_t b;
    if (bitmap_load(path, &b) < 0)
        return -1;
    out->width  = b.width;
    out->height = b.height;
    out->pixels = b.pixels;
    return 0;
}

int imagelib_load_file(const char* path, const char* name) {
    if (path == NULL || name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strlen(name) >= IMAGE_NAME_MAX) {
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
        size_t grow = capacity ? capacity * 2 : 32;
        image_t* p = (image_t*)realloc(images, grow * sizeof(image_t));
        if (p == NULL)
            return -1;
        images = p;
        capacity = grow;
    }

    image_t decoded;
    if (imagelib_decode_file(path, &decoded) < 0)
        return -1;

    memmove(&images[at + 1], &images[at], (count - at) * sizeof(image_t));
    image_t* image = &images[at];
    *image = decoded;
    strcpy(image->name, name);
    count++;
    return 0;
}

static size_t skipped_count;
static char   skipped_first[NAME_MAX + 1];

static void skip(const char* name) {
    if (skipped_count++ == 0)
        strlcpy(skipped_first, name, sizeof(skipped_first));
}

size_t imagelib_skipped(const char** first) {
    if (first != NULL)
        *first = skipped_first;
    return skipped_count;
}

// A file name with a picture's extension left out ("icon_folder.png" ->
// "icon_folder"); any other name as it is
static void reference_name(const char* file, char* out, size_t size) {
    static const char* const extensions[] = {
        ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".psd", ".pic", ".ppm", ".pgm", ".pnm",
    };
    strlcpy(out, file, size);
    const char* dot = strrchr(out, '.');
    if (dot == NULL || dot == out)
        return;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if (strcasecmp(dot, extensions[i]) == 0) {
            out[dot - out] = '\0';
            return;
        }
    }
}

int imagelib_load_dir(const char* dir) {
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

        char name[NAME_MAX + 1];
        reference_name(e->d_name, name, sizeof(name));
        if (imagelib_load_file(path, name) == 0)
            loaded++;
        else
            skip(e->d_name);
    }
    closedir(d);
    return loaded;
}

const image_t* imagelib_get(const char* name) {
    if (name == NULL)
        return NULL;
    bool found;
    size_t at = find(name, &found);
    return found ? &images[at] : NULL;
}

size_t imagelib_count(void) {
    return count;
}

void imagelib_free(void) {
    for (size_t i = 0; i < count; i++)
        free(images[i].pixels);
    free(images);
    images = NULL;
    count = capacity = 0;
}

//
// Drawing
//

void image_draw(gfx_surface_t* s, const image_t* image, int32_t x, int32_t y) {
    if (s == NULL || image == NULL)
        return;
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_draw_pixels_keyed(s, image->pixels, image->width, image->height, 0, x, y, SYS_GFX_SPRITE_KEY);
    gfx_set_blend(s, old);
}

