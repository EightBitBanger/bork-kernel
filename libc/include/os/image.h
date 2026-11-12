#ifndef _OS_IMAGE_H
#define _OS_IMAGE_H

/*
 * Image files: decode a picture into 0xAARRGGBB pixels, and save pixels as
 * a PNG. The system's own images (sys/images, wallpapers) are plain PNG
 * files; nothing has to be converted first.
 *
 *     bitmap_t icon;
 *     if (bitmap_load("/mnt/ata0p1/sys/images/icon_folder.png", &icon) == 0) {
 *         gfx_draw_pixels(surface, icon.pixels, icon.width, icon.height, 0, 10, 10);
 *         bitmap_free(&icon);
 *     }
 *
 * Decoding is stb_image's: PNG, JPEG (baseline and progressive), BMP, GIF
 * (first frame), TGA, PSD (composited), PIC and PNM. A file is told by its
 * first bytes, not its name. Saving writes PNG (stb_image_write), 8 bits
 * per channel with alpha.
 *
 * The window manager draws images alpha blended, with opaque black
 * (SYS_GFX_SPRITE_KEY) see-through; os/resource.h draws them the same way.
 *
 * Errors: -1 with errno.
 *   ENOEXEC  not a picture these formats cover, or damaged (bitmap_error()
 *            says what stb_image found)
 *   EFBIG    past the limits below
 *   ENOMEM   out of memory
 *   anything opening, reading or writing the file gave
 */

#include <stddef.h>
#include <stdint.h>

// Limits on what is decoded, so a damaged or hostile file can't take all
// the memory
#define BITMAP_MAX_SIDE     16384                   // Pixels, width or height
#define BITMAP_MAX_PIXELS   (16u * 1024u * 1024u)   // width * height (64 MB decoded)
#define BITMAP_MAX_FILE     (32u * 1024u * 1024u)   // Bytes of a file read in

typedef struct {
    int32_t   width;
    int32_t   height;
    uint32_t* pixels;       // width * height, 0xAARRGGBB, stride = width (malloc'd)
} bitmap_t;

// Decode the image file at `path` into *out. 0, or -1 with errno.
int bitmap_load(const char* path, bitmap_t* out);

// The same for a file already in memory
int bitmap_decode(const void* data, size_t size, bitmap_t* out);

// Why the last decode failed ("bad huffman code", ...), "" when it didn't
// or the data wasn't a picture at all. In a program built with
// libc_use_stblib, also why the stb library couldn't be loaded.
const char* bitmap_error(void);

// Save width x height pixels (0xAARRGGBB, `stride` pixels a row, 0 =
// width) to `path` as a PNG, replacing what is there. 0, or -1 with errno
// (EINVAL for a bad size).
int bitmap_save_png(const char* path, const uint32_t* pixels, int32_t width, int32_t height,
                    int32_t stride);

// Free out->pixels and clear *b
void bitmap_free(bitmap_t* b);

#endif
