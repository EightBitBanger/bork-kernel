/*
 * 2D graphics: kernel surfaces drawn by batched commands (see os/gfx.h and
 * the "Graphics" section of the kernel's syscall.h for the format).
 *
 * Each gfx_surface_t keeps a command buffer. Drawing calls append to it;
 * when the next command does not fit, the buffer goes to the kernel with
 * SYS_GFX_SUBMIT and recording starts over. Errors from a submit that the
 * program did not trigger itself are kept and reported by the next
 * gfx_flush() / gfx_present().
 *
 * Text: with no font set, text goes to the kernel as commands and is drawn
 * in its built-in font. With one (gfx_set_font, os/font.h), each character
 * is drawn here into a fixed cell the size of the face's '0', so programs
 * that lay text out by gfx_char_width() columns keep working. The font's
 * line is centered on the built-in font's line at the same y, so text
 * placed for the built-in font stays put whatever the font and size.
 * Characters the font lacks, and the control characters the built-in font
 * has symbols for, still come from the kernel, where it would draw them.
 *
 * The state the program can query (origin, blend, text scale, save depth)
 * is mirrored here. Every state change is checked against the kernel's
 * limits before it is recorded, so the kernel never rejects one and the
 * mirror cannot drift from the kernel's copy.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <os/font.h>
#include <os/gfx.h>
#include "internal.h"

#define GFX_BUFFER_WORDS    1024            // 4 KB of commands per submit
#define GFX_STATE_DEPTH     8               // = the kernel's GFX_STATE_DEPTH

_Static_assert(GFX_BUFFER_WORDS * 4 <= SYS_GFX_SUBMIT_MAX, "command buffer larger than a submit");
_Static_assert(sizeof(gfx_rect_t) == sizeof(SysGfxRect), "gfx_rect_t must match SysGfxRect");
_Static_assert(sizeof(gfx_info_t) == sizeof(SysGfxInfo), "gfx_info_t must match SysGfxInfo");

typedef struct {
    int32_t     origin_x, origin_y;
    gfx_blend_t blend;
    int         scale;
} gfx_state_t;

struct gfx_surface {
    uint32_t    handle;
    int32_t     width, height;
    int         error;                      // errno of a deferred failure, 0 = none
    gfx_state_t state;
    gfx_state_t stack[GFX_STATE_DEPTH];
    int         depth;
    uint32_t    used;                       // Words recorded
    uint32_t    cmds[GFX_BUFFER_WORDS];
};

// Font cell at scale 1, from SYS_GFX_INFO on first use
static int32_t font_width  = 0;
static int32_t font_height = 0;

static void font_metrics(void) {
    if (font_width != 0)
        return;
    SysGfxInfo info;
    if (!SYS_IS_ERROR(syscall1(SYS_GFX_INFO, (uint32_t)&info)) && info.char_width != 0) {
        font_width  = (int32_t)info.char_width;
        font_height = (int32_t)info.line_height;
    } else {
        font_width  = 6;
        font_height = 8;
    }
}

// The program's font (gfx_set_font): not owned. A face per text scale,
// made when first needed.
static font_t*      text_font;
static int32_t      text_px;
static font_face_t* text_faces[9];

static font_face_t* face_for(const gfx_surface_t* s) {
    if (text_font == NULL)
        return NULL;
    int scale = s->state.scale;
    if (text_faces[scale] == NULL)
        text_faces[scale] = font_face_create(text_font, text_px * scale);
    return text_faces[scale];
}

static inline bool coord_ok(int32_t v) { return v >= -GFX_COORD_MAX && v <= GFX_COORD_MAX; }

//
// Command buffer
//

// Send what is recorded. Returns 0, or the errno of the failure.
static int submit(gfx_surface_t* s) {
    if (s->used == 0)
        return 0;
    int32_t r = syscall3(SYS_GFX_SUBMIT, s->handle, (uint32_t)s->cmds, s->used * 4U);
    s->used = 0;
    return SYS_IS_ERROR(r) ? __errno_from_sys(r) : 0;
}

// Room for a command of `words` words (header included), flushing if
// needed. Returns where to write it, NULL if it can never fit.
static uint32_t* reserve(gfx_surface_t* s, uint32_t op, uint32_t words) {
    if (words > GFX_BUFFER_WORDS) {
        if (s->error == 0) s->error = EINVAL;
        return NULL;
    }
    if (s->used + words > GFX_BUFFER_WORDS) {
        int e = submit(s);
        if (e != 0 && s->error == 0)
            s->error = e;
    }
    uint32_t* p = &s->cmds[s->used];
    p[0] = SYS_GFX_CMD_HEADER(op, words);
    s->used += words;
    return p + 1;
}

static void cmd(gfx_surface_t* s, uint32_t op, uint32_t argc, const int32_t* args) {
    uint32_t* p = reserve(s, op, argc + 1);
    if (p != NULL)
        memcpy(p, args, argc * 4U);
}

#define CMD(s, op, ...) do { \
        const int32_t args_[] = { __VA_ARGS__ }; \
        cmd((s), (op), sizeof(args_) / sizeof(args_[0]), args_); \
    } while (0)

static void set_error(gfx_surface_t* s, int e) {
    if (s->error == 0)
        s->error = e;
}

//
// Surfaces
//

int gfx_get_info(gfx_info_t* out) {
    return __sys_ret(syscall1(SYS_GFX_INFO, (uint32_t)out));
}

gfx_surface_t* gfx_surface_create(int32_t width, int32_t height) {
    gfx_surface_t* s = (gfx_surface_t*)malloc(sizeof(gfx_surface_t));
    if (s == NULL)
        return NULL;

    int32_t r = syscall2(SYS_GFX_CREATE, (uint32_t)width, (uint32_t)height);
    if (SYS_IS_ERROR(r)) {
        errno = __errno_from_sys(r);
        free(s);
        return NULL;
    }

    memset(s, 0, sizeof(*s) - sizeof(s->cmds));
    s->handle = (uint32_t)r;
    s->width  = width;
    s->height = height;
    s->state.blend = GFX_BLEND_COPY;
    s->state.scale = 1;
    font_metrics();
    return s;
}

int gfx_surface_destroy(gfx_surface_t* s) {
    if (s == NULL)
        return 0;
    int r = __sys_ret(syscall1(SYS_GFX_DESTROY, s->handle));
    free(s);
    return r < 0 ? -1 : 0;
}

int32_t gfx_surface_width(const gfx_surface_t* s)  { return s->width; }
int32_t gfx_surface_height(const gfx_surface_t* s) { return s->height; }

int gfx_flush(gfx_surface_t* s) {
    int e = submit(s);
    if (s->error != 0) {
        e = s->error;       // The earliest failure wins
        s->error = 0;
    }
    if (e != 0) {
        errno = e;
        return -1;
    }
    return 0;
}

int gfx_present_rect(gfx_surface_t* s, const gfx_rect_t* src, int32_t x, int32_t y) {
    int flushed = gfx_flush(s);
    int saved = errno;
    // Shown even when a command failed: everything else was drawn
    int r = __sys_ret(syscall4(SYS_GFX_PRESENT, s->handle, (uint32_t)src, (uint32_t)x, (uint32_t)y));
    if (flushed < 0 && r == 0) {
        errno = saved;
        return -1;
    }
    return r < 0 ? -1 : 0;
}

int gfx_present(gfx_surface_t* s, int32_t x, int32_t y) {
    return gfx_present_rect(s, NULL, x, y);
}

static int transfer(gfx_surface_t* s, uint32_t n, int32_t x, int32_t y, int32_t w, int32_t h,
                    const void* pixels, int32_t stride) {
    if (stride < 0) {
        errno = EINVAL;
        return -1;
    }
    // Commands recorded before this call land first (and a read sees them)
    int e = submit(s);
    if (e != 0)
        set_error(s, e);
    SysGfxRect r = { x, y, w, h };
    return __sys_ret(syscall4(n, s->handle, (uint32_t)&r, (uint32_t)pixels, (uint32_t)stride)) < 0 ? -1 : 0;
}

int gfx_write_pixels(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h,
                     const uint32_t* pixels, int32_t stride) {
    return transfer(s, SYS_GFX_WRITE, x, y, w, h, pixels, stride);
}

int gfx_read_pixels(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h,
                    uint32_t* pixels, int32_t stride) {
    return transfer(s, SYS_GFX_READ, x, y, w, h, pixels, stride);
}

gfx_color_t gfx_get_pixel(gfx_surface_t* s, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= s->width || y >= s->height)
        return 0;
    uint32_t p = 0;
    return gfx_read_pixels(s, x, y, 1, 1, &p, 0) == 0 ? p : 0;
}

//
// State
//

void gfx_set_clip(gfx_surface_t* s, const gfx_rect_t* r) {
    if (r == NULL)
        CMD(s, SYS_GFX_CMD_SET_CLIP, 0, 0, 0, 0, SYS_GFX_F_NO_CLIP);
    else
        CMD(s, SYS_GFX_CMD_SET_CLIP, r->x, r->y, r->w, r->h, 0);
}

void gfx_intersect_clip(gfx_surface_t* s, const gfx_rect_t* r) {
    if (r != NULL)
        CMD(s, SYS_GFX_CMD_INTERSECT_CLIP, r->x, r->y, r->w, r->h);
}

void gfx_set_origin(gfx_surface_t* s, int32_t x, int32_t y) {
    if (!coord_ok(x) || !coord_ok(y)) {
        set_error(s, EINVAL);
        return;
    }
    s->state.origin_x = x;
    s->state.origin_y = y;
    CMD(s, SYS_GFX_CMD_SET_ORIGIN, x, y);
}

void gfx_translate(gfx_surface_t* s, int32_t dx, int32_t dy) {
    // 64-bit sums: dx near INT32_MAX must not wrap back into range
    int64_t x = (int64_t)s->state.origin_x + dx;
    int64_t y = (int64_t)s->state.origin_y + dy;
    if (x < -GFX_COORD_MAX || x > GFX_COORD_MAX || y < -GFX_COORD_MAX || y > GFX_COORD_MAX) {
        set_error(s, EINVAL);
        return;
    }
    s->state.origin_x = (int32_t)x;
    s->state.origin_y = (int32_t)y;
    CMD(s, SYS_GFX_CMD_TRANSLATE, dx, dy);
}

void gfx_get_origin(const gfx_surface_t* s, int32_t* x, int32_t* y) {
    if (x) *x = s->state.origin_x;
    if (y) *y = s->state.origin_y;
}

gfx_blend_t gfx_set_blend(gfx_surface_t* s, gfx_blend_t blend) {
    gfx_blend_t old = s->state.blend;
    if (blend != GFX_BLEND_COPY && blend != GFX_BLEND_ALPHA) {
        set_error(s, EINVAL);
        return old;
    }
    s->state.blend = blend;
    CMD(s, SYS_GFX_CMD_SET_BLEND, (int32_t)blend);
    return old;
}

int gfx_set_text_scale(gfx_surface_t* s, int scale) {
    int old = s->state.scale;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    s->state.scale = scale;
    CMD(s, SYS_GFX_CMD_TEXT_SCALE, scale);
    return old;
}

int gfx_save(gfx_surface_t* s) {
    if (s->depth >= GFX_STATE_DEPTH)
        return -1;
    s->stack[s->depth++] = s->state;
    reserve(s, SYS_GFX_CMD_SAVE, 1);
    return 0;
}

int gfx_restore(gfx_surface_t* s) {
    if (s->depth == 0)
        return -1;
    s->state = s->stack[--s->depth];
    reserve(s, SYS_GFX_CMD_RESTORE, 1);
    return 0;
}

//
// Drawing
//

void gfx_clear(gfx_surface_t* s, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_CLEAR, (int32_t)c);
}

void gfx_set_pixel(gfx_surface_t* s, int32_t x, int32_t y, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_PIXEL, x, y, (int32_t)c);
}

void gfx_draw_line(gfx_surface_t* s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_LINE, x0, y0, x1, y1, (int32_t)c);
}

void gfx_draw_hline(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_FILL_RECT, x, y, w, 1, (int32_t)c);
}

void gfx_draw_vline(gfx_surface_t* s, int32_t x, int32_t y, int32_t h, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_FILL_RECT, x, y, 1, h, (int32_t)c);
}

void gfx_draw_rect(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_RECT, x, y, w, h, (int32_t)c);
}

void gfx_fill_rect(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_FILL_RECT, x, y, w, h, (int32_t)c);
}

void gfx_fill_gradient(gfx_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h,
                       gfx_color_t from, gfx_color_t to, gfx_gradient_t direction) {
    CMD(s, SYS_GFX_CMD_GRADIENT, x, y, w, h, (int32_t)from, (int32_t)to, (int32_t)direction);
}

void gfx_draw_circle(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t r, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_CIRCLE, cx, cy, r, (int32_t)c);
}

void gfx_fill_circle(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t r, gfx_color_t c) {
    CMD(s, SYS_GFX_CMD_FILL_CIRCLE, cx, cy, r, (int32_t)c);
}

static void pie(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t r, int32_t depth, uint32_t flags,
                const uint32_t* values, const gfx_color_t* colors, size_t count) {
    if (values == NULL || colors == NULL || count == 0)
        return;
    if (count > GFX_PIE_MAX_SLICES)
        count = GFX_PIE_MAX_SLICES;     // As the kernel's gfx_draw_pie does

    uint32_t* p = reserve(s, SYS_GFX_CMD_PIE, 7 + 2 * (uint32_t)count);
    if (p == NULL)
        return;
    int32_t head[6] = { cx, cy, r, depth, (int32_t)flags, (int32_t)count };
    memcpy(p, head, sizeof(head));
    memcpy(p + 6, values, count * 4);
    memcpy(p + 6 + count, colors, count * 4);
}

void gfx_draw_pie(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t r,
                  const uint32_t* values, const gfx_color_t* colors, size_t count) {
    pie(s, cx, cy, r, 0, 0, values, colors, count);
}

void gfx_draw_pie_isometric(gfx_surface_t* s, int32_t cx, int32_t cy, int32_t r, int32_t depth,
                            const uint32_t* values, const gfx_color_t* colors, size_t count) {
    pie(s, cx, cy, r, depth, SYS_GFX_F_ISOMETRIC, values, colors, count);
}

//
// Text
//

int gfx_set_font(font_t* font, int32_t px) {
    for (size_t i = 0; i < sizeof(text_faces) / sizeof(text_faces[0]); i++) {
        font_face_destroy(text_faces[i]);
        text_faces[i] = NULL;
    }
    text_font = font;
    text_px   = px;
    if (font == NULL)
        return 0;
    // Make the scale 1 face now, so a failure shows here and not as metrics
    // that change later
    text_faces[1] = font_face_create(font, px);
    if (text_faces[1] == NULL) {
        text_font = NULL;
        return -1;
    }
    return 0;
}

font_t* gfx_get_font(int32_t* px) {
    if (px != NULL)
        *px = text_font != NULL ? text_px : 0;
    return text_font;
}

// Built-in characters: in the kernel's font, from these bytes
static bool builtin_only(unsigned char c) {
    return c < 0x20 || c == 0x7F;
}

// The text with a font: TrueType glyphs in cells, kernel glyphs where the
// font has none. A cell per byte, as programs count them. y is the top of
// the built-in font's line; the font's line is centered on it.
static void text_font_cells(gfx_surface_t* s, font_face_t* face, int32_t x, int32_t y,
                            const char* str, size_t length, gfx_color_t fg, gfx_color_t bg,
                            uint32_t flags) {
    int32_t cw = gfx_char_width(s), lh = gfx_line_height(s);
    int32_t kw = font_width * s->state.scale, kh = font_height * s->state.scale;
    int32_t top = y + (kh - lh) / 2;        // The font's line
    int32_t kx = (cw - kw) / 2, ky = 0;     // Kernel glyphs: centered across, at y

    if (flags & SYS_GFX_F_OPAQUE) {
        int32_t y0 = top < y ? top : y;
        int32_t y1 = (top + lh > y + kh) ? top + lh : y + kh;
        gfx_fill_rect(s, x, y0, (int32_t)length * cw, y1 - y0, bg);
    }

    uint32_t cps[128];
    while (length > 0) {
        size_t n = length > 128 ? 128 : length;
        bool any = false;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)str[i];
            cps[i] = 0;
            if (c == ' ')
                continue;
            if (!builtin_only(c) && font_has_glyph(text_font, c)) {
                cps[i] = c;         // ASCII, and bytes past it as Latin-1
                any = true;
            } else {
                CMD(s, SYS_GFX_CMD_CHAR, x + (int32_t)i * cw + kx, y + ky, c, (int32_t)fg, 0, 0);
            }
        }
        if (any)
            font_draw_cells(face, s, x, top, cps, n, cw, fg);
        str += n;
        length -= n;
        x += (int32_t)n * cw;
    }
}

// Long strings go out as several commands, each placed where the previous
// one ended (as one call would have drawn them)
static void text(gfx_surface_t* s, int32_t x, int32_t y, const char* str, size_t length,
                 gfx_color_t fg, gfx_color_t bg, uint32_t flags) {
    if (str == NULL)
        return;
    length = strnlen(str, length);
    font_face_t* face = face_for(s);
    if (face != NULL) {
        text_font_cells(s, face, x, y, str, length, fg, bg, flags);
        return;
    }
    int32_t advance = gfx_char_width(s);

    while (length > 0) {
        size_t n = (length > SYS_GFX_TEXT_MAX) ? SYS_GFX_TEXT_MAX : length;
        if (!coord_ok(x))
            break;      // Everything further right is off any surface anyway

        uint32_t* p = reserve(s, SYS_GFX_CMD_TEXT, 7 + ((uint32_t)n + 3) / 4);
        if (p == NULL)
            return;
        int32_t head[6] = { x, y, (int32_t)fg, (int32_t)bg, (int32_t)flags, (int32_t)n };
        memcpy(p, head, sizeof(head));
        p[6 + (n - 1) / 4] = 0;            // Zero the padding of the last word
        memcpy(p + 6, str, n);

        str += n;
        length -= n;
        x += (int32_t)n * advance;
    }
}

void gfx_draw_text(gfx_surface_t* s, int32_t x, int32_t y, const char* str, gfx_color_t color) {
    text(s, x, y, str, (size_t)-1, color, 0, 0);
}

void gfx_draw_text_n(gfx_surface_t* s, int32_t x, int32_t y, const char* str, size_t length, gfx_color_t color) {
    text(s, x, y, str, length, color, 0, 0);
}

void gfx_draw_text_opaque(gfx_surface_t* s, int32_t x, int32_t y, const char* str, gfx_color_t fg, gfx_color_t bg) {
    text(s, x, y, str, (size_t)-1, fg, bg, SYS_GFX_F_OPAQUE);
}

void gfx_draw_char(gfx_surface_t* s, int32_t x, int32_t y, unsigned char ch, gfx_color_t color) {
    font_face_t* face = face_for(s);
    if (face != NULL) {
        char c = (char)ch;
        if (ch != 0)
            text_font_cells(s, face, x, y, &c, 1, color, 0, 0);
        return;
    }
    CMD(s, SYS_GFX_CMD_CHAR, x, y, ch, (int32_t)color, 0, 0);
}

void gfx_draw_char_opaque(gfx_surface_t* s, int32_t x, int32_t y, unsigned char ch, gfx_color_t fg, gfx_color_t bg) {
    font_face_t* face = face_for(s);
    if (face != NULL) {
        char c = (char)(ch ? ch : ' ');
        text_font_cells(s, face, x, y, &c, 1, fg, bg, SYS_GFX_F_OPAQUE);
        return;
    }
    CMD(s, SYS_GFX_CMD_CHAR, x, y, ch, (int32_t)fg, (int32_t)bg, SYS_GFX_F_OPAQUE);
}

int32_t gfx_char_width(const gfx_surface_t* s) {
    font_metrics();
    font_face_t* face = face_for(s);
    return face != NULL ? font_face_cell_width(face) : font_width * s->state.scale;
}

int32_t gfx_line_height(const gfx_surface_t* s) {
    font_metrics();
    font_face_t* face = face_for(s);
    return face != NULL ? font_face_height(face) : font_height * s->state.scale;
}

int32_t gfx_text_y(const gfx_surface_t* s, int32_t top, int32_t height) {
    font_metrics();
    return top + (height - font_height * s->state.scale) / 2;
}

// The scale 1 face, or NULL for the built-in font
static font_face_t* face_1(void) {
    if (text_font != NULL && text_faces[1] == NULL)
        text_faces[1] = font_face_create(text_font, text_px);
    return text_faces[1];
}

int32_t __gfx_cell_width(void) {
    font_metrics();
    font_face_t* face = face_1();
    return face != NULL ? font_face_cell_width(face) : font_width;
}

int32_t __gfx_line_height(void) {
    font_metrics();
    font_face_t* face = face_1();
    return face != NULL ? font_face_height(face) : font_height;
}

int32_t gfx_text_width(const gfx_surface_t* s, const char* str) {
    return str ? (int32_t)strlen(str) * gfx_char_width(s) : 0;
}

//
// Images
//

static void blit(gfx_surface_t* dst, gfx_surface_t* src, const gfx_rect_t* r,
                 int32_t x, int32_t y, uint32_t flags, gfx_color_t key) {
    if (src == NULL)
        return;
    if (src == dst) {
        set_error(dst, EINVAL);
        return;
    }
    // The source must be complete when the kernel copies it, and later
    // drawing on src must not show up in this blit: flush both sides now
    int e = submit(src);
    if (e != 0)
        set_error(src, e);

    if (r == NULL)
        CMD(dst, SYS_GFX_CMD_BLIT, (int32_t)src->handle, 0, 0, 0, 0, x, y, (int32_t)(flags | SYS_GFX_F_WHOLE), (int32_t)key);
    else
        CMD(dst, SYS_GFX_CMD_BLIT, (int32_t)src->handle, r->x, r->y, r->w, r->h, x, y, (int32_t)flags, (int32_t)key);

    e = submit(dst);
    if (e != 0)
        set_error(dst, e);
}

void gfx_blit(gfx_surface_t* dst, gfx_surface_t* src, const gfx_rect_t* r, int32_t x, int32_t y) {
    blit(dst, src, r, x, y, 0, 0);
}

void gfx_blit_keyed(gfx_surface_t* dst, gfx_surface_t* src, const gfx_rect_t* r,
                    int32_t x, int32_t y, gfx_color_t key) {
    blit(dst, src, r, x, y, SYS_GFX_F_KEYED, key);
}

// For gfx_draw_resource* (resource.c), which lives with the resource table
// so that only programs using resources carry the image decoder
void __gfx_set_error(gfx_surface_t* s, int e) {
    if (s != NULL)
        set_error(s, e);
}

static void pixels(gfx_surface_t* s, const uint32_t* px, int32_t w, int32_t h, int32_t stride,
                   int32_t x, int32_t y, uint32_t flags, gfx_color_t key) {
    if (px == NULL || stride < 0) {
        set_error(s, EINVAL);
        return;
    }
    // The kernel reads the pixels when the command runs: run it now, while
    // the caller's buffer is known to be valid
    CMD(s, SYS_GFX_CMD_PIXELS, (int32_t)(uintptr_t)px, w, h, stride, x, y, (int32_t)flags, (int32_t)key);
    int e = submit(s);
    if (e != 0)
        set_error(s, e);
}

void gfx_draw_pixels(gfx_surface_t* s, const uint32_t* px, int32_t w, int32_t h,
                     int32_t stride, int32_t x, int32_t y) {
    pixels(s, px, w, h, stride, x, y, 0, 0);
}

void gfx_draw_pixels_keyed(gfx_surface_t* s, const uint32_t* px, int32_t w, int32_t h,
                           int32_t stride, int32_t x, int32_t y, gfx_color_t key) {
    pixels(s, px, w, h, stride, x, y, SYS_GFX_F_KEYED, key);
}


