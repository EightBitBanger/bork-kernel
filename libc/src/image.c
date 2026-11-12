/*
 * Image files (os/image.h): reading and decoding. The decoder is
 * stb_image (stbimage.c); saving is in image_png.c.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/image.h>

#include "stb_image.h"     // Declarations only: stbimage.c has the code

static const char* reason = "";

const char* bitmap_error(void) {
    return reason;
}

void bitmap_free(bitmap_t* b) {
    if (b == NULL)
        return;
    free(b->pixels);
    memset(b, 0, sizeof(*b));
}

int bitmap_decode(const void* data, size_t size, bitmap_t* out) {
    if (data == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    reason = "";
    memset(out, 0, sizeof(*out));
    if (size > (size_t)INT_MAX) {
        errno = EFBIG;
        return -1;
    }

    int w, h, comp;
    if (!stbi_info_from_memory((const stbi_uc*)data, (int)size, &w, &h, &comp)) {
        // Not a picture it knows, or (a program built with libc_use_stblib)
        // no decoder: the stb library didn't load, and its reason says why
        const char* why = stbi_failure_reason();
        if (why != NULL && strncmp(why, "stb library", 11) == 0)
            reason = why;
        errno = ENOEXEC;
        return -1;
    }
    if (w <= 0 || h <= 0 || w > BITMAP_MAX_SIDE || h > BITMAP_MAX_SIDE ||
        (uint64_t)w * (uint64_t)h > BITMAP_MAX_PIXELS) {
        errno = EFBIG;
        return -1;
    }

    // RGBA bytes in memory order, 4 per pixel...
    unsigned char* rgba = stbi_load_from_memory((const stbi_uc*)data, (int)size, &w, &h, &comp, 4);
    if (rgba == NULL) {
        const char* why = stbi_failure_reason();
        reason = (why != NULL) ? why : "";
        errno = (why != NULL && strcmp(why, "outofmem") == 0) ? ENOMEM : ENOEXEC;
        return -1;
    }
    // ...turned into 0xAARRGGBB words in place (the buffer came from malloc)
    uint32_t* pixels = (uint32_t*)(void*)rgba;
    size_t total = (size_t)w * (size_t)h;
    for (size_t i = 0; i < total; i++) {
        const unsigned char* p = &rgba[i * 4];
        pixels[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    out->width  = w;
    out->height = h;
    out->pixels = pixels;
    return 0;
}

// The whole file into a malloc'd buffer (*size bytes). NULL with errno
// (EFBIG past BITMAP_MAX_FILE).
static unsigned char* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    unsigned char* data = NULL;
    long length = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        length = ftell(f);
    if (length < 0 || fseek(f, 0, SEEK_SET) != 0) {
        errno = EIO;
        goto done;
    }
    if ((unsigned long)length > BITMAP_MAX_FILE) {
        errno = EFBIG;
        goto done;
    }
    if (length == 0) {
        errno = ENOEXEC;
        goto done;
    }
    data = (unsigned char*)malloc((size_t)length);
    if (data == NULL) {
        errno = ENOMEM;
        goto done;
    }
    if (fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        data = NULL;
        errno = EIO;
        goto done;
    }
    *size = (size_t)length;
done:
    fclose(f);
    return data;
}

int bitmap_load(const char* path, bitmap_t* out) {
    if (path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    reason = "";
    size_t size = 0;
    unsigned char* data = read_file(path, &size);
    if (data == NULL)
        return -1;
    int rc = bitmap_decode(data, size, out);
    int err = errno;
    free(data);
    errno = err;
    return rc;
}
