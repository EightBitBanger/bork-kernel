/*
 * stb_image's implementation, built once for the image library
 * (imagelib.c). Configured for this libc:
 *
 *   - no stdio: imagelib reads the file itself and decodes from memory
 *   - no HDR / linear float images (they need libm's pow / ldexp, and the
 *     desktop has no use for them)
 *   - no SIMD (the target is plain i686) and no thread-locals (one thread)
 *   - at most IMAGELIB_MAX_SIDE pixels a side, so a hostile header can't ask
 *     for an absurd allocation
 *
 * Formats: PNG, JPEG (baseline and progressive), BMP, GIF (first frame),
 * TGA, PSD (composited), PIC and PNM.
 */

#include "imagelib.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS     IMAGELIB_MAX_SIDE

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
