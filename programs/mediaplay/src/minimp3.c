/*
 * minimp3 (https://github.com/lieff/minimp3, CC0 / public domain, see
 * minimp3-LICENSE.txt): the decoder itself, built once here. Plain C, no
 * SSE (the libc's programs are built for i686).
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"
