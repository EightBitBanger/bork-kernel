/*
 * stb_image's implementation, for os/image.h (image.c). Configured for this
 * libc:
 *
 *   - no stdio: image.c reads the file itself and decodes from memory
 *   - no HDR / linear float images (they need libm's pow / ldexp)
 *   - no SIMD (the target is plain i686) and no thread-locals
 *   - at most BITMAP_MAX_SIDE pixels a side, so a hostile header can't ask
 *     for an absurd allocation
 *
 * Its own object file, so a program that never decodes an image does not
 * carry it, and a program built with libc_use_stblib() (os/stblib.h) gets
 * these functions from the stb library instead.
 */

#include <os/image.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS     BITMAP_MAX_SIDE

// Its code isn't ours to tidy: keep the build quiet about it
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "stb_image.h"
#pragma GCC diagnostic pop
