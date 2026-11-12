#ifndef DWM_TEXT_H
#define DWM_TEXT_H

/*
 * Text in the DWM's UI font.
 *
 * At start-up the DWM picks a TrueType font from the font library
 * (resources/fontlib.h, <home>/sys/fonts): DWM_FONT_NAME at DWM_FONT_PX
 * pixels when that font is there, else the first font loaded (by name).
 * Without any, text falls back to the kernel's built-in 6 x 8 font, and
 * every call here still works (widths are then whole character cells).
 *
 *     int32_t w = dwm_text_width(title, strlen(title));
 *     dwm_text_draw(s, x, y, title, strlen(title), color);   // y: top of the line
 *
 * Text is UTF-8; a byte that isn't valid UTF-8 shows as one '?'. Control
 * characters show as spaces. Widths and fits are in pixels and match what
 * dwm_text_draw() puts on the surface.
 *
 * Fonts and glyph rendering are libc's (os/font.h): glyphs are rasterized
 * once per font and size. Each dwm_text_draw() is one gfx_draw_pixels()
 * call, alpha blended through the surface's clip and origin (its blend mode
 * is left as it was). The color's alpha is honored.
 *
 * Programs: the DWM hands every connected program the app font below
 * (os/wm.h fetches it on connect), and libc draws their gfx_draw_text() with
 * it, one fixed cell per character so column layouts stay put. A monospaced
 * font suits that best. Not found: the UI font; no fonts: the built-in one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/font.h>
#include <os/gfx.h>

#define DWM_FONT_NAME       "opensans"
#define DWM_FONT_PX         16
#define DWM_FONT_PX_MIN     6
#define DWM_FONT_PX_MAX     48

// The font programs draw their text in
#define DWM_APP_FONT_NAME   "opensans"
#define DWM_APP_FONT_PX     16

// Use the loaded font `name` (NULL: DWM_FONT_NAME) at `px` pixels high
// (clamped to the range above). When there is no such font, the first one
// loaded is used instead and this returns false; with no fonts at all, the
// built-in font (also false). Every window is repainted.
bool dwm_text_set_font(const char* name, int32_t px);

// The font in use (NULL: the built-in one) and its size in pixels
const char* dwm_text_font_name(void);
int32_t     dwm_text_font_px(void);

// The font handed to programs (NULL: none, they use the built-in one)
font_t*     dwm_text_app_font(int32_t* px);

// Whether a TrueType font is in use (false: the built-in font)
bool dwm_text_truetype(void);

// Height of one line; draw lines this far apart
int32_t dwm_text_height(void);

// Width of the first `len` bytes of `text`
int32_t dwm_text_width(const char* text, size_t len);

// How many bytes of `text` (at most len, whole characters) fit in `width`
size_t dwm_text_fit(const char* text, size_t len, int32_t width);

// Draw `len` bytes of `text` with the line's top-left corner at (x, y)
void dwm_text_draw(gfx_surface_t* s, int32_t x, int32_t y, const char* text, size_t len,
                   gfx_color_t color);

// `text` cut to fit `width`, ending in `suffix` ("..", "...") when cut, or
// always when `force` (the caller cut it already). Into buf; its length.
size_t dwm_text_ellipsize(const char* text, size_t len, int32_t width, const char* suffix,
                          bool force, char* buf, size_t size);

// A line of wrapped text: `len` bytes from text + start
typedef struct {
    size_t start;
    size_t len;
} dwm_text_line_t;

// Break `text` into lines at most `width` wide: after the last space that
// fits, at every '\n', and inside a word longer than a line. Spaces at the
// ends of lines are left out. Returns how many lines (at most max); *more:
// some text didn't fit in `max` lines.
size_t dwm_text_wrap(const char* text, size_t len, int32_t width, dwm_text_line_t* out,
                     size_t max, bool* more);

#endif
