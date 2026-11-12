#ifndef IMAGELIB_H
#define IMAGELIB_H

/*
 * Image library: image files decoded once into 0xAARRGGBB pixels and kept
 * by name.
 *
 *     imagelib_load_dir(images_dir);          // every file in the folder
 *     const image_t* icon = imagelib_get("icon_folder");
 *     image_draw(surface, icon, 10, 10);
 *
 * A file's name is its reference name: "<dir>/cur_pointer" is "cur_pointer".
 * A picture's known extension is left out of it, so "icon_folder.png" is
 * "icon_folder" too. Images stay loaded until imagelib_free(). Not
 * thread-safe.
 *
 * Formats: whatever libc's os/image.h decodes (stb_image): PNG, JPEG, BMP,
 * GIF (first frame), TGA, PSD, PIC and PNM. A file is told by its first
 * bytes, not its name. The system's own images are PNG files.
 */

#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>

#define IMAGE_NAME_MAX  64      // Bytes, terminator included

// Limits on what is decoded, so a damaged or hostile file can't take all
// the memory
#define IMAGELIB_MAX_SIDE   16384                   // Pixels, width or height
#define IMAGELIB_MAX_PIXELS (16u * 1024u * 1024u)   // width * height (64 MB decoded)
#define IMAGELIB_MAX_FILE   (32u * 1024u * 1024u)   // Bytes of a compressed file read in

typedef struct {
    char      name[IMAGE_NAME_MAX];
    int32_t   width;
    int32_t   height;
    uint32_t* pixels;           // width * height, 0xAARRGGBB, stride = width
} image_t;

// Load every regular file in `dir` that is an image. Files that are not are
// skipped (see imagelib_skipped). Returns how many were added, or -1
// with errno when the folder cannot be opened.
int imagelib_load_dir(const char* dir);

// How many files the last imagelib_load_dir() skipped; *first (may be NULL)
// gets the first one's name ("" when none)
size_t imagelib_skipped(const char** first);

// Load one image file under `name`. 0, or -1 with errno: ENOEXEC (not an
// image, a format not read, or damaged), EFBIG (over the limits above),
// EEXIST (name in use), ENAMETOOLONG (name), ENOMEM, or whatever opening
// or reading the file gave.
int imagelib_load_file(const char* path, const char* name);

// Decode one image file into *out without adding it to the library
// (out->name is ""). The caller frees out->pixels. 0, or -1 with errno as
// for imagelib_load_file.
int imagelib_decode_file(const char* path, image_t* out);

// The same for a file already in memory
int imagelib_decode_memory(const void* data, size_t size, image_t* out);

// Why the last stb_image decode failed ("bad huffman code", ...), "" when
// it didn't or the file wasn't one stb_image reads
const char* imagelib_decode_reason(void);

// The image loaded under `name`, or NULL
const image_t* imagelib_get(const char* name);

// Every image, sorted by name
size_t         imagelib_count(void);

void imagelib_free(void);

// Draw an image with its top-left corner at (x, y), the way the window
// manager draws icons: alpha blended, opaque black (SYS_GFX_SPRITE_KEY)
// see-through. Goes through the surface's clip and origin; its blend mode is
// left as it was. Runs at once (the pixels are read before this returns).
void image_draw(gfx_surface_t* s, const image_t* image, int32_t x, int32_t y);

#endif

