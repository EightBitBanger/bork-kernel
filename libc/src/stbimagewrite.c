/*
 * stb_image_write's implementation, for os/image.h (image_png.c). Its own
 * object file, so a program built with libc_use_stblib() (os/stblib.h) gets
 * these functions from the stb library instead and this object is never
 * linked.
 */

#include <stddef.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "stb_image_write.h"
#pragma GCC diagnostic pop
