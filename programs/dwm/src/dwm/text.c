/*
 * Text in the UI font (text.h), on libc's fonts (os/font.h): a face of the
 * chosen font at the chosen size, which caches its glyphs, plus wrapping
 * and cutting by pixel width.
 *
 * Programs get the app font (DWM_APP_FONT_NAME) from the server: libc draws
 * their gfx_draw_text() with it in fixed cells. The DWM sets the same one
 * for its own gfx text (the rename box), so it matches the programs.
 */

#include <stdlib.h>
#include <string.h>
#include <os/font.h>

#include "text.h"
#include "compositor.h"
#include "internal.h"
#include "server.h"
#include "../resources/fontlib.h"

static font_face_t* face;           // NULL: the built-in font
static int32_t      cell_w, cell_h; // Built-in font cell, read once

static font_t*      app_font;       // Handed to programs (NULL: built-in)
static int32_t      app_px;

static void cell_size(void) {
    if (cell_w > 0)
        return;
    gfx_info_t info;
    if (gfx_get_info(&info) == 0 && info.char_width != 0 && info.line_height != 0) {
        cell_w = (int32_t)info.char_width;
        cell_h = (int32_t)info.line_height;
    } else {
        cell_w = 6;
        cell_h = 8;
    }
}

//
// Font choice
//

// DWM_APP_FONT_NAME, else the UI font, else none
static void choose_app_font(void) {
    font_t* font = fontlib_get(DWM_APP_FONT_NAME);
    if (font == NULL && face != NULL)
        font = font_face_font(face);
    app_font = font;
    app_px   = DWM_APP_FONT_PX;
    if (gfx_set_font(app_font, app_px) < 0)
        app_font = NULL;
}

bool dwm_text_set_font(const char* name, int32_t px) {
    font_t* font = fontlib_get(name != NULL ? name : DWM_FONT_NAME);
    bool found = (font != NULL);
    if (font == NULL)
        font = fontlib_at(0);       // NULL too when none are loaded

    if (px < DWM_FONT_PX_MIN) px = DWM_FONT_PX_MIN;
    if (px > DWM_FONT_PX_MAX) px = DWM_FONT_PX_MAX;

    font_face_t* next = (font != NULL) ? font_face_create(font, px) : NULL;
    if (font != NULL && next == NULL)
        return false;               // Out of memory: keep what there is
    font_face_destroy(face);
    face = next;

    choose_app_font();
    compositor_invalidate_all();
    server_font_changed();
    return found && face != NULL;
}

void text_shutdown(void) {
    gfx_set_font(NULL, 0);
    app_font = NULL;
    font_face_destroy(face);
    face = NULL;
}

const char* dwm_text_font_name(void) {
    return face != NULL ? fontlib_name_of(font_face_font(face)) : NULL;
}

int32_t dwm_text_font_px(void) {
    if (face != NULL)
        return font_face_px(face);
    cell_size();
    return cell_h;
}

font_t* dwm_text_app_font(int32_t* px) {
    if (px != NULL)
        *px = app_font != NULL ? app_px : 0;
    return app_font;
}

bool dwm_text_truetype(void) {
    return face != NULL;
}

int32_t dwm_text_height(void) {
    if (face != NULL)
        return font_face_height(face);
    cell_size();
    return cell_h;
}

//
// Measuring and drawing
//

int32_t dwm_text_width(const char* text, size_t len) {
    if (text == NULL)
        return 0;
    if (face == NULL) {
        cell_size();
        return (int32_t)len * cell_w;
    }
    return font_text_width(face, text, len);
}

size_t dwm_text_fit(const char* text, size_t len, int32_t width) {
    if (text == NULL || width <= 0)
        return 0;
    if (face == NULL) {
        cell_size();
        size_t n = (size_t)(width / cell_w);
        return n < len ? n : len;
    }
    return font_text_fit(face, text, len, width);
}

void dwm_text_draw(gfx_surface_t* s, int32_t x, int32_t y, const char* text, size_t len,
                   gfx_color_t color) {
    if (s == NULL || text == NULL || len == 0)
        return;
    if (face == NULL)
        gfx_draw_text_n(s, x, y, text, len, color);
    else
        font_draw_text(face, s, x, y, text, len, color);
}

//
// Fitting and wrapping
//

size_t dwm_text_ellipsize(const char* text, size_t len, int32_t width, const char* suffix,
                          bool force, char* buf, size_t size) {
    if (buf == NULL || size == 0)
        return 0;
    size_t n;
    size_t slen = (suffix != NULL) ? strlen(suffix) : 0;

    if (!force && dwm_text_width(text, len) <= width) {
        n = len < size - 1 ? len : size - 1;
        memcpy(buf, text, n);
        buf[n] = '\0';
        return n;
    }

    n = dwm_text_fit(text, len, width - dwm_text_width(suffix, slen));
    while (n > 0 && text[n - 1] == ' ')
        n--;
    if (n + slen > size - 1) {
        if (slen > size - 1)
            slen = size - 1;
        n = size - 1 - slen;
        while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80)
            n--;                    // Not inside a character
    }
    memcpy(buf, text, n);
    memcpy(buf + n, suffix, slen);
    buf[n + slen] = '\0';
    return n + slen;
}

size_t dwm_text_wrap(const char* text, size_t len, int32_t width, dwm_text_line_t* out,
                     size_t max, bool* more) {
    size_t lines = 0, pos = 0;
    *more = false;

    while (pos < len) {
        while (pos < len && text[pos] == ' ')
            pos++;
        if (pos >= len)
            break;
        if (lines == max) {
            *more = true;
            break;
        }

        size_t end = pos;
        while (end < len && text[end] != '\n')
            end++;
        size_t n = dwm_text_fit(text + pos, end - pos, width), next;
        if (n == end - pos) {
            next = (end < len) ? end + 1 : end;     // The whole line, past its '\n'
        } else {
            size_t brk = n;
            if (text[pos + n] != ' ')
                while (brk > 0 && text[pos + brk - 1] != ' ')
                    brk--;
            if (brk == 0) {
                // A word longer than the line: cut it, at least one character
                brk = n;
                if (brk == 0) {
                    brk = 1;
                    while (brk < end - pos && ((uint8_t)text[pos + brk] & 0xC0) == 0x80)
                        brk++;
                }
            }
            n = brk;
            next = pos + brk;
        }

        while (n > 0 && text[pos + n - 1] == ' ')
            n--;
        out[lines].start = pos;
        out[lines].len   = n;
        lines++;
        pos = next;
    }

    // Only spaces and line breaks left: nothing was lost
    if (*more) {
        size_t rest = pos;
        while (rest < len && (text[rest] == ' ' || text[rest] == '\n'))
            rest++;
        *more = rest < len;
    }
    return lines;
}
