#ifndef _OS_STBLIB_H
#define _OS_STBLIB_H

/*
 * stblib: stb_image, stb_image_write and stb_truetype as a user library
 * (os/ulib.h), so programs can use them without each carrying ~140 KB of
 * decoder and rasterizer code.
 *
 * The library file is <home>/sys/lib/stb, built from programs/stblib. A
 * program opts in with one line in its CMakeLists.txt, after
 * libc_add_program():
 *
 *   libc_use_stblib(<program>)
 *
 * That links libc/stblib/stblib_client.c into it: one small function for
 * each stb function below, under the stb function's own name, which loads
 * the library the first time any of them is called and then calls through
 * its table. libc's os/image.h and os/font.h call these names, so they use
 * the library too, and a program needs no change in its code. Without the
 * line, a program gets the stb code linked in as before.
 *
 * If the library can't be loaded (missing, damaged, a different version),
 * each function returns its failure value (NULL, 0) and
 * stbi_failure_reason() / stblib_error() say why; bitmap_load() and
 * font_open() then fail with ENOEXEC as for a bad file.
 *
 * Changing the table: add new functions at the end of STBLIB_FUNCTIONS and
 * leave STBLIB_ABI alone, programs built earlier still find theirs where
 * they were (the client checks `size` for the ones it was built with).
 * Changing or removing one means a new STBLIB_ABI, and programs built
 * against the old one refuse the new file until they are rebuilt.
 */

#include <stdint.h>
#include <stb_truetype.h>

#define STBLIB_NAME     "stb"
#define STBLIB_ABI      1
#define STBLIB_SYSPATH  "lib/stb"       // Under <home>/sys (os_syspath)

/*
 * Every function in the table, in table order.
 *
 *   X(return type, name, (parameters), (arguments), value on failure)
 *   V(name, (parameters), (arguments))                 returns void
 */
#define STBLIB_FUNCTIONS(X, V) \
    /* stb_image: decoding (from memory: no stdio) */ \
    X(unsigned char*, stbi_load_from_memory, \
      (const unsigned char* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels), \
      (buffer, len, x, y, channels_in_file, desired_channels), NULL) \
    X(int, stbi_info_from_memory, \
      (const unsigned char* buffer, int len, int* x, int* y, int* comp), \
      (buffer, len, x, y, comp), 0) \
    X(int, stbi_is_16_bit_from_memory, (const unsigned char* buffer, int len), (buffer, len), 0) \
    X(unsigned short*, stbi_load_16_from_memory, \
      (const unsigned char* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels), \
      (buffer, len, x, y, channels_in_file, desired_channels), NULL) \
    V(stbi_image_free, (void* retval_from_stbi_load), (retval_from_stbi_load)) \
    X(const char*, stbi_failure_reason, (void), (), stblib_error()) \
    V(stbi_set_flip_vertically_on_load, (int flag_true_if_should_flip), (flag_true_if_should_flip)) \
    V(stbi_set_unpremultiply_on_load, (int flag_true_if_should_unpremultiply), (flag_true_if_should_unpremultiply)) \
    V(stbi_convert_iphone_png_to_rgb, (int flag_true_if_should_convert), (flag_true_if_should_convert)) \
    \
    /* stb_image_write: encoding through a callback */ \
    X(int, stbi_write_png_to_func, \
      (void (*func)(void* context, void* data, int size), void* context, int w, int h, int comp, const void* data, int stride_in_bytes), \
      (func, context, w, h, comp, data, stride_in_bytes), 0) \
    X(int, stbi_write_bmp_to_func, \
      (void (*func)(void* context, void* data, int size), void* context, int w, int h, int comp, const void* data), \
      (func, context, w, h, comp, data), 0) \
    X(int, stbi_write_tga_to_func, \
      (void (*func)(void* context, void* data, int size), void* context, int w, int h, int comp, const void* data), \
      (func, context, w, h, comp, data), 0) \
    X(int, stbi_write_jpg_to_func, \
      (void (*func)(void* context, void* data, int size), void* context, int x, int y, int comp, const void* data, int quality), \
      (func, context, x, y, comp, data, quality), 0) \
    \
    /* stb_truetype: font files */ \
    X(int, stbtt_GetNumberOfFonts, (const unsigned char* data), (data), 0) \
    X(int, stbtt_GetFontOffsetForIndex, (const unsigned char* data, int index), (data, index), -1) \
    X(int, stbtt_InitFont, (stbtt_fontinfo* info, const unsigned char* data, int offset), (info, data, offset), 0) \
    X(const char*, stbtt_GetFontNameString, \
      (const stbtt_fontinfo* font, int* length, int platformID, int encodingID, int languageID, int nameID), \
      (font, length, platformID, encodingID, languageID, nameID), NULL) \
    X(int, stbtt_FindGlyphIndex, (const stbtt_fontinfo* info, int unicode_codepoint), (info, unicode_codepoint), 0) \
    \
    /* stb_truetype: metrics */ \
    X(float, stbtt_ScaleForPixelHeight, (const stbtt_fontinfo* info, float pixels), (info, pixels), 0.0f) \
    X(float, stbtt_ScaleForMappingEmToPixels, (const stbtt_fontinfo* info, float pixels), (info, pixels), 0.0f) \
    V(stbtt_GetFontVMetrics, (const stbtt_fontinfo* info, int* ascent, int* descent, int* lineGap), \
      (info, ascent, descent, lineGap)) \
    V(stbtt_GetFontBoundingBox, (const stbtt_fontinfo* info, int* x0, int* y0, int* x1, int* y1), \
      (info, x0, y0, x1, y1)) \
    V(stbtt_GetCodepointHMetrics, (const stbtt_fontinfo* info, int codepoint, int* advanceWidth, int* leftSideBearing), \
      (info, codepoint, advanceWidth, leftSideBearing)) \
    V(stbtt_GetGlyphHMetrics, (const stbtt_fontinfo* info, int glyph_index, int* advanceWidth, int* leftSideBearing), \
      (info, glyph_index, advanceWidth, leftSideBearing)) \
    X(int, stbtt_GetCodepointKernAdvance, (const stbtt_fontinfo* info, int ch1, int ch2), (info, ch1, ch2), 0) \
    X(int, stbtt_GetGlyphKernAdvance, (const stbtt_fontinfo* info, int glyph1, int glyph2), (info, glyph1, glyph2), 0) \
    X(int, stbtt_IsGlyphEmpty, (const stbtt_fontinfo* info, int glyph_index), (info, glyph_index), 1) \
    \
    /* stb_truetype: rendering */ \
    V(stbtt_GetCodepointBitmapBox, \
      (const stbtt_fontinfo* font, int codepoint, float scale_x, float scale_y, int* ix0, int* iy0, int* ix1, int* iy1), \
      (font, codepoint, scale_x, scale_y, ix0, iy0, ix1, iy1)) \
    V(stbtt_GetGlyphBitmapBox, \
      (const stbtt_fontinfo* font, int glyph, float scale_x, float scale_y, int* ix0, int* iy0, int* ix1, int* iy1), \
      (font, glyph, scale_x, scale_y, ix0, iy0, ix1, iy1)) \
    V(stbtt_MakeCodepointBitmap, \
      (const stbtt_fontinfo* info, unsigned char* output, int out_w, int out_h, int out_stride, float scale_x, float scale_y, int codepoint), \
      (info, output, out_w, out_h, out_stride, scale_x, scale_y, codepoint)) \
    V(stbtt_MakeGlyphBitmap, \
      (const stbtt_fontinfo* info, unsigned char* output, int out_w, int out_h, int out_stride, float scale_x, float scale_y, int glyph), \
      (info, output, out_w, out_h, out_stride, scale_x, scale_y, glyph)) \
    X(unsigned char*, stbtt_GetCodepointBitmap, \
      (const stbtt_fontinfo* info, float scale_x, float scale_y, int codepoint, int* width, int* height, int* xoff, int* yoff), \
      (info, scale_x, scale_y, codepoint, width, height, xoff, yoff), NULL) \
    X(unsigned char*, stbtt_GetGlyphBitmap, \
      (const stbtt_fontinfo* info, float scale_x, float scale_y, int glyph, int* width, int* height, int* xoff, int* yoff), \
      (info, scale_x, scale_y, glyph, width, height, xoff, yoff), NULL) \
    V(stbtt_FreeBitmap, (unsigned char* bitmap, void* userdata), (bitmap, userdata))

// The library's exports: `size` (bytes of the table the library has), then
// one pointer per function above, in that order
#define STBLIB_MEMBER(ret, name, params, args, fail)  ret (*name) params;
#define STBLIB_MEMBER_V(name, params, args)           void (*name) params;

typedef struct {
    uint32_t size;
    STBLIB_FUNCTIONS(STBLIB_MEMBER, STBLIB_MEMBER_V)
} stblib_api_t;

/*
 * In a program that uses libc_use_stblib() (stblib_client.c):
 */

// The library's table, loading it on the first call. NULL when it can't
// be loaded; stblib_error() says why, and later calls don't try again.
const stblib_api_t* stblib(void);

// Why loading failed ("stb library <path>: bad checksum"), "" otherwise
const char* stblib_error(void);

#endif
