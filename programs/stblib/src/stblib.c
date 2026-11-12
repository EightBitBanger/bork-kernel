/*
 * stblib: stb_image, stb_image_write and stb_truetype as a user library
 * (os/ulib.h, os/stblib.h). Built into <home>/sys/lib/stb by build.sh.
 *
 * Configured as libc's own copies are (libc/src/stbimage.c,
 * stbimagewrite.c, stbtruetype.c), so a program decodes and draws the same
 * with the library as without it:
 *
 *   - no stdio: everything is decoded from and encoded to memory
 *   - no HDR / linear float images, no SIMD, no thread-locals
 *   - at most BITMAP_MAX_SIDE pixels a side
 *
 * Memory comes from the program (ulib_host_t), so what stbi_load_* and
 * stbtt_Get*Bitmap return can be freed with the program's free(), as
 * libc's image.c does. Pure helpers (memcpy, floor, sqrt, ...) come from
 * libc/ulib/runtime.c and libc's math.c (build-ulib.sh).
 */

#include <stddef.h>
#include <string.h>
#include <math.h>
#include <os/image.h>
#include <os/stblib.h>
#include <os/ulib.h>

static const ulib_host_t* host;

static void* lib_malloc(size_t size)            { return host->malloc((uint32_t)size); }
static void* lib_realloc(void* p, size_t size)  { return host->realloc(p, (uint32_t)size); }
static void  lib_free(void* p)                  { host->free(p); }

// Its code isn't ours to tidy: keep the build quiet about it
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS     BITMAP_MAX_SIDE
#define STBI_ASSERT(x)          ((void)0)
#define STBI_MALLOC(sz)         lib_malloc(sz)
#define STBI_REALLOC(p, newsz)  lib_realloc(p, newsz)
#define STBI_FREE(p)            lib_free(p)
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x)         ((void)0)
#define STBIW_MALLOC(sz)        lib_malloc(sz)
#define STBIW_REALLOC(p, newsz) lib_realloc(p, newsz)
#define STBIW_FREE(p)           lib_free(p)
#include "stb_image_write.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x)         ((void)0)
#define STBTT_malloc(x, u)      ((void)(u), lib_malloc(x))
#define STBTT_free(x, u)        ((void)(u), lib_free(x))
#include <stb_truetype.h>

#pragma GCC diagnostic pop

// The table, filled in by name from the list in os/stblib.h, so it can't
// get out of order
#define FILL(ret, name, params, args, fail)  .name = name,
#define FILL_V(name, params, args)           .name = name,

static const stblib_api_t exports = {
    .size = sizeof(stblib_api_t),
    STBLIB_FUNCTIONS(FILL, FILL_V)
};

__attribute__((section(".text.ulib_entry")))
const void* ulib_entry(const ulib_host_t* h) {
    if (h == NULL || h->size < offsetof(ulib_host_t, free) + sizeof(h->free) ||
        h->malloc == NULL || h->realloc == NULL || h->free == NULL)
        return NULL;
    host = h;
    return &exports;
}
