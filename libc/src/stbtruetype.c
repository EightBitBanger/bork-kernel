/*
 * stb_truetype's implementation, for os/font.h (font.c). Its own object
 * file, so a program built with libc_use_stblib() (os/stblib.h) gets these
 * functions from the stb library instead and this object is never linked.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
