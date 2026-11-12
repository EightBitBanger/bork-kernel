/*
 * Resources (os/resource.h): image files decoded in this process
 * (os/image.h) and kept in a table, handle h being table[h - 1]. Drawing
 * them (gfx_draw_resource*, declared in os/gfx.h) is here too, so that a
 * program which never uses a resource does not carry the image decoder.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <os/os.h>
#include <os/gfx.h>
#include <os/image.h>
#include <os/resource.h>
#include "internal.h"

typedef struct {
    char     name[RESOURCE_NAME_MAX];
    char*    path;          // As it was loaded (malloc'd); NULL: free slot
    bitmap_t image;
} resource_t;

static resource_t table[RESOURCE_MAX];

// The name a file registers under: its last path component, with a
// picture's extension left out ("icon_folder.png" -> "icon_folder"). False
// with errno when it is empty or too long.
static bool resource_name(const char* path, char out[RESOURCE_NAME_MAX]) {
    static const char* const extensions[] = {
        ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".psd", ".pic", ".ppm", ".pgm", ".pnm",
    };
    const char* slash = strrchr(path, '/');
    const char* file = slash ? slash + 1 : path;
    size_t length = strlen(file);
    const char* dot = strrchr(file, '.');
    if (dot != NULL && dot != file) {
        for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
            if (strcasecmp(dot, extensions[i]) == 0) {
                length = (size_t)(dot - file);
                break;
            }
        }
    }
    if (length == 0) {
        errno = EINVAL;
        return false;
    }
    if (length >= RESOURCE_NAME_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(out, file, length);
    out[length] = '\0';
    return true;
}

static int find(const char* name) {
    for (int i = 0; i < RESOURCE_MAX; i++)
        if (table[i].path != NULL && strcmp(table[i].name, name) == 0)
            return i;
    return -1;
}

static const resource_t* lookup(int handle) {
    if (handle < 1 || handle > RESOURCE_MAX || table[handle - 1].path == NULL)
        return NULL;
    return &table[handle - 1];
}

int resource_load(const char* path) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    char name[RESOURCE_NAME_MAX];
    if (!resource_name(path, name))
        return -1;

    int existing = find(name);
    if (existing >= 0) {
        if (strcmp(table[existing].path, path) == 0)
            return 0;               // Already loaded
        errno = EEXIST;
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < RESOURCE_MAX && slot < 0; i++)
        if (table[i].path == NULL)
            slot = i;
    if (slot < 0) {
        errno = EMFILE;
        return -1;
    }

    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }

    bitmap_t image;
    if (bitmap_load(path, &image) < 0) {
        if (errno == EFBIG)
            errno = ENOEXEC;
        return -1;
    }
    if (image.width > SYS_GFX_MAX_DIM || image.height > SYS_GFX_MAX_DIM) {
        bitmap_free(&image);
        errno = ENOEXEC;
        return -1;
    }
    char* copy = strdup(path);
    if (copy == NULL) {
        bitmap_free(&image);
        errno = ENOMEM;
        return -1;
    }

    resource_t* r = &table[slot];
    memcpy(r->name, name, sizeof(name));
    r->path  = copy;
    r->image = image;
    return 0;
}

int resource_get(const char* name) {
    if (name == NULL) {
        errno = EFAULT;
        return -1;
    }
    size_t length = strlen(name);
    if (length == 0) {
        errno = EINVAL;
        return -1;
    }
    if (length >= RESOURCE_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int i = find(name);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    return i + 1;
}

int resource_info(int handle, resource_info_t* out) {
    const resource_t* r = lookup(handle);
    if (r == NULL) {
        errno = EBADF;
        return -1;
    }
    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }
    out->type     = RESOURCE_IMAGE;
    out->width    = (uint32_t)r->image.width;
    out->height   = (uint32_t)r->image.height;
    out->reserved = 0;
    return 0;
}

const uint32_t* resource_pixels(int handle) {
    const resource_t* r = lookup(handle);
    return r ? r->image.pixels : NULL;
}

int resource_load_system(const char* name) {
    if (name == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (name[0] == '\0' || strchr(name, '/') != NULL) {
        errno = EINVAL;
        return -1;
    }

    // Loaded already, from whichever of the two files
    if (find(name) >= 0)
        return 0;

    char path[SYS_PATH_MAX];
    if (os_syspath("images", path, sizeof(path)) == NULL)
        return -1;

    size_t used = strlen(path), length = strlen(name);
    if (used + 1 + length + sizeof(".png") > sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    path[used] = '/';
    memcpy(path + used + 1, name, length);
    memcpy(path + used + 1 + length, ".png", sizeof(".png"));
    if (resource_load(path) == 0)
        return 0;
    if (errno != ENOENT)
        return -1;

    // No <name>.png: a file called just <name>, in any format
    path[used + 1 + length] = '\0';
    return resource_load(path);
}

int resource_open(const char* path) {
    if (resource_load(path) < 0)
        return -1;
    char name[RESOURCE_NAME_MAX];
    if (!resource_name(path, name))
        return -1;
    return resource_get(name);
}

int resource_open_system(const char* name) {
    if (resource_load_system(name) < 0)
        return -1;
    return resource_get(name);
}

//
// Drawing (os/gfx.h): the way the window manager draws icons, alpha blended
// with opaque black (SYS_GFX_SPRITE_KEY) see-through
//

void gfx_draw_resource_rect(gfx_surface_t* s, int resource, const gfx_rect_t* src, int32_t x, int32_t y) {
    if (s == NULL)
        return;
    const resource_t* r = lookup(resource);
    if (r == NULL) {
        __gfx_set_error(s, EBADF);
        return;
    }

    int32_t sx = 0, sy = 0, sw = r->image.width, sh = r->image.height;
    if (src != NULL) {
        // Cut to the image, moving the destination with the cut
        sx = src->x;
        sy = src->y;
        sw = src->w;
        sh = src->h;
        if (sx < 0) { sw += sx; x -= sx; sx = 0; }
        if (sy < 0) { sh += sy; y -= sy; sy = 0; }
        if (sw > r->image.width - sx)  sw = r->image.width - sx;
        if (sh > r->image.height - sy) sh = r->image.height - sy;
        if (sw <= 0 || sh <= 0)
            return;
    }

    const uint32_t* px = r->image.pixels + (size_t)sy * (size_t)r->image.width + (size_t)sx;
    gfx_blend_t old = gfx_set_blend(s, GFX_BLEND_ALPHA);
    gfx_draw_pixels_keyed(s, px, sw, sh, r->image.width, x, y, SYS_GFX_SPRITE_KEY);
    gfx_set_blend(s, old);
}

void gfx_draw_resource(gfx_surface_t* s, int resource, int32_t x, int32_t y) {
    gfx_draw_resource_rect(s, resource, NULL, x, y);
}
