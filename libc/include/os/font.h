#ifndef _OS_FONT_H
#define _OS_FONT_H

/*
 * TrueType / OpenType fonts, rendered with stb_truetype (Sean Barrett's
 * stb_truetype.h, in this libc's include/).
 *
 *     font_t* font = font_open_file("/mnt/ata0p1/sys/fonts/LiberationSans-Regular.ttf");
 *     font_face_t* face = font_face_create(font, 13);           // 13 pixels high
 *     font_draw_text(face, surface, 8, 8, "hello", 5, GFX_COLOR_WHITE);
 *     int32_t w = font_text_width(face, "hello", 5);
 *     font_face_destroy(face);
 *     font_close(font);
 *
 * A font_t is the font file, checked before stb sees it: stb follows the
 * file's offsets without bounds checks, so the tables it relies on (the
 * table directory, head, hhea, maxp, hmtx, loca / glyf or CFF, and the cmap
 * subtable it uses) are checked to lie inside the file first. That stops a
 * truncated or damaged file; it is not a full validator, so only use fonts
 * you trust. Kerning is not applied (stb reads those tables unchecked).
 *
 * A font_face_t is the font at one pixel size, with a cache of rendered
 * glyphs. Text is UTF-8 (a byte that is not valid UTF-8 shows as '?',
 * control characters as spaces). Drawing goes through the surface's clip
 * and origin, alpha blended (the surface's blend mode is left as it was),
 * as one gfx_draw_pixels() call per string; the color's alpha is honored.
 *
 * gfx_draw_text() and the other os/gfx.h text calls use a font too once a
 * program has one set (gfx_set_font); wm_connect() sets the one the window
 * manager hands out. Not thread-safe.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>

#define FONT_FILE_MAX       (32u * 1024u * 1024u)   // Largest font file read
#define FONT_FAMILY_MAX     64                      // Bytes, terminator included
#define FONT_PX_MIN         4
#define FONT_PX_MAX         128

typedef struct font      font_t;
typedef struct font_face font_face_t;

//
// Fonts
//

// A font from `size` bytes of a .ttf / .otf / .ttc file (the first font of
// a collection). The bytes are copied. NULL with errno: ENOEXEC (not a font
// this can use, or damaged), EFBIG (over FONT_FILE_MAX), ENOMEM.
font_t* font_open(const void* data, size_t size);

// The same from a file; also whatever opening or reading it gave
font_t* font_open_file(const char* path);

// Free it. Destroy its faces first.
void font_close(font_t* font);

// Family name from the font's name table, ASCII ("" when it has none)
const char* font_family(const font_t* font);

// The whole file as it was opened
const void* font_bytes(const font_t* font, size_t* size);

int  font_glyph_count(const font_t* font);

// Glyph for a Unicode code point; 0 (the "missing" glyph) when the font
// has none
int  font_glyph(const font_t* font, uint32_t codepoint);
bool font_has_glyph(const font_t* font, uint32_t codepoint);

//
// Faces: a font at a pixel size
//

// px is the height from the highest ascender to the lowest descender,
// clamped to FONT_PX_MIN .. FONT_PX_MAX. NULL with errno ENOMEM.
font_face_t* font_face_create(font_t* font, int32_t px);
void         font_face_destroy(font_face_t* face);

font_t*  font_face_font(const font_face_t* face);
int32_t  font_face_px(const font_face_t* face);
int32_t  font_face_height(const font_face_t* face);         // Line height (= px, give or take rounding)
int32_t  font_face_ascent(const font_face_t* face);         // Baseline, from the line's top
int32_t  font_face_cell_width(const font_face_t* face);     // Advance of '0': a fixed cell

// Width of the first `len` bytes of `text`, in pixels
int32_t  font_text_width(font_face_t* face, const char* text, size_t len);

// How many bytes of `text` (at most len, whole characters) fit in `width`
size_t   font_text_fit(font_face_t* face, const char* text, size_t len, int32_t width);

// Draw `len` bytes of `text` with the line's top-left corner at (x, y)
void     font_draw_text(font_face_t* face, gfx_surface_t* s, int32_t x, int32_t y,
                        const char* text, size_t len, gfx_color_t color);

// Draw one code point per fixed cell `cell_w` wide, each glyph centered in
// its cell (0: leave that cell empty). For text laid out by columns.
void     font_draw_cells(font_face_t* face, gfx_surface_t* s, int32_t x, int32_t y,
                         const uint32_t* codepoints, size_t count, int32_t cell_w,
                         gfx_color_t color);

#endif
