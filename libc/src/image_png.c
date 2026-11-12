/*
 * Image files (os/image.h): saving as PNG through stb_image_write
 * (stbimagewrite.c, or the stb library for a program built with
 * libc_use_stblib). Its own object file, so only a program that saves
 * images carries the encoder.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/image.h>

#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"     // Declarations only: stbimagewrite.c has the code

typedef struct {
    FILE* f;
    bool  failed;
} sink_t;

static void put(void* ctx, void* data, int size) {
    sink_t* sink = (sink_t*)ctx;
    if (!sink->failed && size > 0 && fwrite(data, 1, (size_t)size, sink->f) != (size_t)size)
        sink->failed = true;
}

int bitmap_save_png(const char* path, const uint32_t* pixels, int32_t width, int32_t height,
                    int32_t stride) {
    if (path == NULL || pixels == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (stride == 0)
        stride = width;
    if (width <= 0 || height <= 0 || width > BITMAP_MAX_SIDE || height > BITMAP_MAX_SIDE ||
        stride < width || (uint64_t)width * (uint64_t)height > BITMAP_MAX_PIXELS) {
        errno = EINVAL;
        return -1;
    }

    // 0xAARRGGBB words -> RGBA bytes
    size_t row = (size_t)width * 4u;
    unsigned char* rgba = (unsigned char*)malloc(row * (size_t)height);
    if (rgba == NULL) {
        errno = ENOMEM;
        return -1;
    }
    for (int32_t y = 0; y < height; y++) {
        const uint32_t* src = pixels + (size_t)y * (size_t)stride;
        unsigned char* dst = rgba + (size_t)y * row;
        for (int32_t x = 0; x < width; x++) {
            uint32_t c = src[x];
            dst[x * 4 + 0] = (unsigned char)(c >> 16);
            dst[x * 4 + 1] = (unsigned char)(c >> 8);
            dst[x * 4 + 2] = (unsigned char)c;
            dst[x * 4 + 3] = (unsigned char)(c >> 24);
        }
    }

    sink_t sink = { .f = fopen(path, "wb"), .failed = false };
    if (sink.f == NULL) {
        int err = errno;
        free(rgba);
        errno = err;
        return -1;
    }
    int ok = stbi_write_png_to_func(put, &sink, width, height, 4, rgba, (int)row);
    free(rgba);
    int err = sink.failed ? (errno ? errno : EIO) : 0;
    if (fclose(sink.f) != 0 && err == 0)
        err = errno ? errno : EIO;
    if (!ok && err == 0)
        err = ENOMEM;               // The encoder's only failure is an allocation
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}
