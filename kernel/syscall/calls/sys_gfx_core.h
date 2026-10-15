
#ifndef _KERNEL_SYS_GFX_CORE_H_
#define _KERNEL_SYS_GFX_CORE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/gfx/gfx.h>
#include <kernel/dwm/rendering/sprite.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// Graphics surfaces for user programs: the command interpreter
//
// Everything here works on a SysGfxSurface and reaches user memory only
// through the callbacks in SysGfxEnv, so it knows nothing about processes,
// address spaces or locking (sys_gfx.h supplies those). That also lets the
// libc test suite run this exact code on a development machine.
//
// Every value that reaches the gfx layer is range checked first. The gfx
// code does its clipping in int32 arithmetic (Cohen-Sutherland products,
// radius squared, ...), and coordinates near INT32_MAX would overflow it
// into writes outside the surface. Within the SYS_GFX_*_MAX limits every
// intermediate value stays below 2^31.
//

// One surface: its pixels and its drawing state
typedef struct {
    GfxSurface surface;     // Owns its pixels (gfx_surface_create)
    GfxContext gc;
} SysGfxSurface;

// One loaded resource (SYS_RES_LOAD): a decoded, read-only image
typedef struct {
    GfxSurface surface;                 // Owns its pixels
    uint32_t   type;                    // SYS_RES_TYPE_*
    char       name[SYS_RES_NAME_MAX];
    char*      path;                    // Absolute path it was loaded from (malloc'd)
} SysGfxResource;

typedef struct {
    void* ctx;

    // Is [addr, addr + len) user memory, readable (and writable if write)?
    bool (*check)(void* ctx, uint32_t addr, uint32_t len, bool write);

    // Copy to / from user memory already passed by check()
    bool (*copy_in)(void* ctx, void* dst, uint32_t addr, uint32_t len);
    bool (*copy_out)(void* ctx, uint32_t addr, const void* src, uint32_t len);

    // Another surface of the same process (BLIT's source), or NULL
    SysGfxSurface* (*lookup)(void* ctx, uint32_t handle);

    // A resource of the same process (RESOURCE's source), or NULL. May be
    // NULL itself: then there are no resources.
    const SysGfxResource* (*resource)(void* ctx, uint32_t handle);
} SysGfxEnv;

#define SYS_GFX_BOUNCE_BYTES  16384     // PIXELS strips go through this much

static inline bool sys_gfx_coord_ok(int32_t v) {
    return v >= -SYS_GFX_COORD_MAX && v <= SYS_GFX_COORD_MAX;
}

static inline bool sys_gfx_size_ok(int32_t v) {
    return v >= -SYS_GFX_SIZE_MAX && v <= SYS_GFX_SIZE_MAX;
}

static inline bool sys_gfx_rect_ok(const int32_t* a) {
    return sys_gfx_coord_ok(a[0]) && sys_gfx_coord_ok(a[1]) && sys_gfx_size_ok(a[2]) && sys_gfx_size_ok(a[3]);
}

// Bytes a surface of this size takes, 0 if the size is not allowed
static inline uint32_t sys_gfx_surface_bytes(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0 || width > SYS_GFX_MAX_DIM || height > SYS_GFX_MAX_DIM)
        return 0;
    return (uint32_t)width * (uint32_t)height * 4U;
}

// Allocate the pixels, clear to opaque black (never hand out old kernel
// memory) and set up the default drawing state
static int32_t sys_gfx_surface_init(SysGfxSurface* s, int32_t width, int32_t height) {
    if (sys_gfx_surface_bytes(width, height) == 0)
        return SYS_EINVAL;
    memset(s, 0, sizeof(*s));
    if (!gfx_surface_create(&s->surface, width, height))
        return SYS_ENOMEM;
    gfx_context_init(&s->gc, &s->surface);
    gfx_clear(&s->gc, GFX_COLOR_BLACK);
    return SYS_OK;
}

static void sys_gfx_surface_free(SysGfxSurface* s) {
    gfx_surface_destroy(&s->surface);
}

//
// Resources (SYS_RES_*): naming and decoding. Loading the file and the
// per-process table are the caller's (syscall/calls/sys_resource.h).
//

// A resource's name: the last part of `path` (absolute, normalized).
// SYS_ENAMETOOLONG past SYS_RES_NAME_MAX - 1 bytes, SYS_EINVAL for "/".
static int32_t sys_gfx_resource_name(const char* path, char out[SYS_RES_NAME_MAX]) {
    const char* name = path;
    for (const char* p = path; *p != '\0'; p++) {
        if (*p == '/')
            name = p + 1;
    }
    size_t length = strlen(name);
    if (length == 0)
        return SYS_EINVAL;
    if (length >= SYS_RES_NAME_MAX)
        return SYS_ENAMETOOLONG;
    memcpy(out, name, length + 1);
    return SYS_OK;
}

// Pixel bytes a sprite decodes to, 0 if it can't be a surface
static uint32_t sys_gfx_sprite_bytes(const struct Sprite* sprite) {
    return sys_gfx_surface_bytes(sprite->width, sprite->height);
}

// Decode a sprite into r->surface. SYS_EBADFMT when its size is not
// allowed for a surface, SYS_ENOMEM when out of memory.
static int32_t sys_gfx_resource_from_sprite(SysGfxResource* r, const struct Sprite* sprite) {
    if (sys_gfx_sprite_bytes(sprite) == 0)
        return SYS_EBADFMT;
    if (!gfx_surface_create(&r->surface, sprite->width, sprite->height))
        return SYS_ENOMEM;
    // A created surface's stride is its width: the bitmap fits row for row
    sprite_get_bitmap(r->surface.pixels, sprite);
    r->type = SYS_RES_TYPE_SPRITE;
    return SYS_OK;
}

static void sys_gfx_resource_free(SysGfxResource* r) {
    gfx_surface_destroy(&r->surface);
    free(r->path);
    r->path = NULL;
}

//
// Raw pixel transfer (SYS_GFX_WRITE / SYS_GFX_READ)
//

// The rectangle lies inside the surface, the stride is sane, and the user
// buffer is all there. Fills *stride_px (0 = width).
static int32_t sys_gfx_transfer_check(const SysGfxSurface* s, const SysGfxRect* r, uint32_t addr,
                                      uint32_t* stride_px, bool write, const SysGfxEnv* env) {
    if (r->x < 0 || r->y < 0 || r->w <= 0 || r->h <= 0 ||
        r->w > s->surface.width - r->x || r->h > s->surface.height - r->y)
        return SYS_EINVAL;

    uint32_t stride = (*stride_px == 0) ? (uint32_t)r->w : *stride_px;
    if (stride < (uint32_t)r->w || stride > SYS_GFX_STRIDE_MAX)
        return SYS_EINVAL;
    *stride_px = stride;

    uint64_t span = ((uint64_t)(r->h - 1) * stride + (uint64_t)r->w) * 4U;
    if (span > 0xFFFFFFFFULL || !env->check(env->ctx, addr, (uint32_t)span, write))
        return SYS_EFAULT;
    return SYS_OK;
}

static int32_t sys_gfx_write_pixels(SysGfxSurface* s, const SysGfxRect* r, uint32_t addr,
                                    uint32_t stride, const SysGfxEnv* env) {
    int32_t result = sys_gfx_transfer_check(s, r, addr, &stride, false, env);
    if (result < 0)
        return result;

    for (int32_t row = 0; row < r->h; row++) {
        uint32_t* dest = gfx_surface_pixel(&s->surface, r->x, r->y + row);
        if (!env->copy_in(env->ctx, dest, addr + (uint32_t)row * stride * 4U, (uint32_t)r->w * 4U))
            return SYS_EFAULT;
    }
    return SYS_OK;
}

static int32_t sys_gfx_read_pixels(SysGfxSurface* s, const SysGfxRect* r, uint32_t addr,
                                   uint32_t stride, const SysGfxEnv* env) {
    int32_t result = sys_gfx_transfer_check(s, r, addr, &stride, true, env);
    if (result < 0)
        return result;

    for (int32_t row = 0; row < r->h; row++) {
        const uint32_t* src = gfx_surface_pixel(&s->surface, r->x, r->y + row);
        if (!env->copy_out(env->ctx, addr + (uint32_t)row * stride * 4U, src, (uint32_t)r->w * 4U))
            return SYS_EFAULT;
    }
    return SYS_OK;
}

//
// Commands
//

// PIXELS: user pixels drawn through the context (origin, clip, blend, key),
// a strip of rows at a time
static int32_t sys_gfx_cmd_pixels(SysGfxSurface* s, const int32_t* a, const SysGfxEnv* env) {
    uint32_t addr   = (uint32_t)a[0];
    int32_t  width  = a[1], height = a[2];
    uint32_t stride = (uint32_t)a[3];
    int32_t  x = a[4], y = a[5];
    uint32_t flags  = (uint32_t)a[6];
    gfx_color_t key = (gfx_color_t)a[7];

    if (width <= 0 || height <= 0)
        return SYS_OK;
    if (width > SYS_GFX_MAX_DIM || height > SYS_GFX_MAX_DIM || !sys_gfx_coord_ok(x) || !sys_gfx_coord_ok(y))
        return SYS_EINVAL;
    if (stride == 0)
        stride = (uint32_t)width;
    if (stride < (uint32_t)width || stride > SYS_GFX_STRIDE_MAX)
        return SYS_EINVAL;

    uint64_t span = ((uint64_t)(height - 1) * stride + (uint64_t)width) * 4U;
    if (span > 0xFFFFFFFFULL || !env->check(env->ctx, addr, (uint32_t)span, false))
        return SYS_EFAULT;

    uint32_t row_bytes = (uint32_t)width * 4U;
    int32_t rows_per = (int32_t)(SYS_GFX_BOUNCE_BYTES / row_bytes);
    if (rows_per < 1) rows_per = 1;
    if (rows_per > height) rows_per = height;

    uint32_t* bounce = (uint32_t*)malloc(row_bytes * (uint32_t)rows_per);
    if (bounce == NULL)
        return SYS_ENOMEM;

    int32_t result = SYS_OK;
    for (int32_t row = 0; row < height; row += rows_per) {
        int32_t n = (height - row < rows_per) ? height - row : rows_per;
        for (int32_t i = 0; i < n; i++) {
            if (!env->copy_in(env->ctx, bounce + (uint32_t)i * (uint32_t)width,
                              addr + (uint32_t)(row + i) * stride * 4U, row_bytes)) {
                result = SYS_EFAULT;
                break;
            }
        }
        if (result < 0)
            break;
        gfx_draw_pixels(&s->gc, bounce, width, n, x, y + row, key, (flags & SYS_GFX_F_KEYED) != 0);
    }

    free(bounce);
    return result;
}

static int32_t sys_gfx_cmd_blit(SysGfxSurface* s, const int32_t* a, const SysGfxEnv* env) {
    SysGfxSurface* src = env->lookup(env->ctx, (uint32_t)a[0]);
    if (src == NULL)
        return SYS_EBADF;
    if (src == s)
        return SYS_EINVAL;      // Overlapping copy within one surface: not supported
    if (!sys_gfx_rect_ok(&a[1]) || !sys_gfx_coord_ok(a[5]) || !sys_gfx_coord_ok(a[6]))
        return SYS_EINVAL;

    uint32_t flags = (uint32_t)a[7];
    GfxRect rect = gfx_rect(a[1], a[2], a[3], a[4]);
    const GfxRect* src_rect = (flags & SYS_GFX_F_WHOLE) ? NULL : &rect;

    if (flags & SYS_GFX_F_KEYED)
        gfx_blit_keyed(&s->gc, &src->surface, src_rect, a[5], a[6], (gfx_color_t)a[8]);
    else
        gfx_blit(&s->gc, &src->surface, src_rect, a[5], a[6]);
    return SYS_OK;
}

static int32_t sys_gfx_cmd_resource(SysGfxSurface* s, const int32_t* a, const SysGfxEnv* env) {
    const SysGfxResource* res = env->resource ? env->resource(env->ctx, (uint32_t)a[0]) : NULL;
    if (res == NULL)
        return SYS_EBADF;
    if (!sys_gfx_rect_ok(&a[1]) || !sys_gfx_coord_ok(a[5]) || !sys_gfx_coord_ok(a[6]))
        return SYS_EINVAL;

    uint32_t flags = (uint32_t)a[7];
    GfxRect rect = gfx_rect(a[1], a[2], a[3], a[4]);
    const GfxRect* src_rect = (flags & SYS_GFX_F_WHOLE) ? NULL : &rect;

    // ALPHA applies to this draw only
    GfxBlend previous = s->gc.state.blend;
    if (flags & SYS_GFX_F_ALPHA)
        gfx_set_blend(&s->gc, GFX_BLEND_ALPHA);

    if (flags & SYS_GFX_F_KEYED)
        gfx_blit_keyed(&s->gc, &res->surface, src_rect, a[5], a[6], (gfx_color_t)a[8]);
    else
        gfx_blit(&s->gc, &res->surface, src_rect, a[5], a[6]);

    gfx_set_blend(&s->gc, previous);
    return SYS_OK;
}

static int32_t sys_gfx_cmd_text(SysGfxSurface* s, const int32_t* a, uint32_t argc) {
    uint32_t length = (uint32_t)a[5];
    if (length > SYS_GFX_TEXT_MAX || argc != 6 + (length + 3) / 4)
        return SYS_EINVAL;
    if (!sys_gfx_coord_ok(a[0]) || !sys_gfx_coord_ok(a[1]))
        return SYS_EINVAL;

    char text[SYS_GFX_TEXT_MAX + 1];
    memcpy(text, &a[6], length);
    text[length] = '\0';

    if ((uint32_t)a[4] & SYS_GFX_F_OPAQUE)
        gfx_draw_text_opaque(&s->gc, a[0], a[1], text, (gfx_color_t)a[2], (gfx_color_t)a[3]);
    else
        gfx_draw_text_n(&s->gc, a[0], a[1], text, length, (gfx_color_t)a[2]);
    return SYS_OK;
}

static int32_t sys_gfx_cmd_pie(SysGfxSurface* s, const int32_t* a, uint32_t argc) {
    uint32_t count = (uint32_t)a[5];
    if (count == 0 || count > SYS_GFX_PIE_MAX || argc != 6 + 2 * count)
        return SYS_EINVAL;
    if (!sys_gfx_coord_ok(a[0]) || !sys_gfx_coord_ok(a[1]) ||
        a[2] < 0 || a[2] > SYS_GFX_RADIUS_MAX || a[3] < 0 || a[3] > SYS_GFX_DEPTH_MAX)
        return SYS_EINVAL;

    const uint32_t*    values = (const uint32_t*)&a[6];
    const gfx_color_t* colors = (const gfx_color_t*)&a[6 + count];

    if ((uint32_t)a[4] & SYS_GFX_F_ISOMETRIC)
        gfx_draw_pie_isometric(&s->gc, a[0], a[1], a[2], a[3], values, colors, count);
    else
        gfx_draw_pie(&s->gc, a[0], a[1], a[2], values, colors, count);
    return SYS_OK;
}

// One command; a = its arguments, argc = how many. The header length has
// been checked against the batch already.
static int32_t sys_gfx_command(SysGfxSurface* s, uint32_t op, const int32_t* a, uint32_t argc,
                               const SysGfxEnv* env) {
    GfxContext* gc = &s->gc;

    // Fixed argument counts (0 = variable, checked by the command itself)
    static const uint8_t args_needed[] = {
        [SYS_GFX_CMD_CLEAR]       = 1, [SYS_GFX_CMD_PIXEL]       = 3, [SYS_GFX_CMD_LINE]        = 5,
        [SYS_GFX_CMD_RECT]        = 5, [SYS_GFX_CMD_FILL_RECT]   = 5, [SYS_GFX_CMD_GRADIENT]    = 7,
        [SYS_GFX_CMD_CIRCLE]      = 4, [SYS_GFX_CMD_FILL_CIRCLE] = 4, [SYS_GFX_CMD_TEXT]        = 6,
        [SYS_GFX_CMD_CHAR]        = 6, [SYS_GFX_CMD_PIE]         = 6, [SYS_GFX_CMD_BLIT]        = 9,
        [SYS_GFX_CMD_RESOURCE]    = 9,
        [SYS_GFX_CMD_PIXELS]      = 8, [SYS_GFX_CMD_SET_CLIP]    = 5, [SYS_GFX_CMD_INTERSECT_CLIP] = 4,
        [SYS_GFX_CMD_SET_ORIGIN]  = 2, [SYS_GFX_CMD_TRANSLATE]   = 2, [SYS_GFX_CMD_SET_BLEND]   = 1,
        [SYS_GFX_CMD_TEXT_SCALE]  = 1, [SYS_GFX_CMD_SAVE]        = 0, [SYS_GFX_CMD_RESTORE]     = 0,
    };
    if (op == 0 || op >= sizeof(args_needed))
        return SYS_EINVAL;
    bool variable = (op == SYS_GFX_CMD_TEXT || op == SYS_GFX_CMD_PIE);
    if (variable ? argc < args_needed[op] : argc != args_needed[op])
        return SYS_EINVAL;

    switch (op) {
        case SYS_GFX_CMD_CLEAR:
            gfx_clear(gc, (gfx_color_t)a[0]);
            return SYS_OK;

        case SYS_GFX_CMD_PIXEL:
            if (!sys_gfx_coord_ok(a[0]) || !sys_gfx_coord_ok(a[1])) return SYS_EINVAL;
            gfx_set_pixel(gc, a[0], a[1], (gfx_color_t)a[2]);
            return SYS_OK;

        case SYS_GFX_CMD_LINE:
            for (int i = 0; i < 4; i++)
                if (!sys_gfx_coord_ok(a[i])) return SYS_EINVAL;
            gfx_draw_line(gc, a[0], a[1], a[2], a[3], (gfx_color_t)a[4]);
            return SYS_OK;

        case SYS_GFX_CMD_RECT:
        case SYS_GFX_CMD_FILL_RECT:
            if (!sys_gfx_rect_ok(a)) return SYS_EINVAL;
            if (op == SYS_GFX_CMD_RECT) gfx_draw_rect(gc, a[0], a[1], a[2], a[3], (gfx_color_t)a[4]);
            else                        gfx_fill_rect(gc, a[0], a[1], a[2], a[3], (gfx_color_t)a[4]);
            return SYS_OK;

        case SYS_GFX_CMD_GRADIENT:
            if (!sys_gfx_rect_ok(a)) return SYS_EINVAL;
            if (a[6] != SYS_GFX_GRADIENT_VERTICAL && a[6] != SYS_GFX_GRADIENT_HORIZONTAL) return SYS_EINVAL;
            gfx_fill_gradient(gc, a[0], a[1], a[2], a[3], (gfx_color_t)a[4], (gfx_color_t)a[5], (GfxGradient)a[6]);
            return SYS_OK;

        case SYS_GFX_CMD_CIRCLE:
        case SYS_GFX_CMD_FILL_CIRCLE:
            if (!sys_gfx_coord_ok(a[0]) || !sys_gfx_coord_ok(a[1]) || a[2] < 0 || a[2] > SYS_GFX_RADIUS_MAX)
                return SYS_EINVAL;
            if (op == SYS_GFX_CMD_CIRCLE) gfx_draw_circle(gc, a[0], a[1], a[2], (gfx_color_t)a[3]);
            else                          gfx_fill_circle(gc, a[0], a[1], a[2], (gfx_color_t)a[3]);
            return SYS_OK;

        case SYS_GFX_CMD_TEXT:
            return sys_gfx_cmd_text(s, a, argc);

        case SYS_GFX_CMD_CHAR:
            if (!sys_gfx_coord_ok(a[0]) || !sys_gfx_coord_ok(a[1]) || (uint32_t)a[2] > 0xFF) return SYS_EINVAL;
            if ((uint32_t)a[5] & SYS_GFX_F_OPAQUE)
                gfx_draw_char_opaque(gc, a[0], a[1], (uint8_t)a[2], (gfx_color_t)a[3], (gfx_color_t)a[4]);
            else
                gfx_draw_char(gc, a[0], a[1], (uint8_t)a[2], (gfx_color_t)a[3]);
            return SYS_OK;

        case SYS_GFX_CMD_PIE:
            return sys_gfx_cmd_pie(s, a, argc);

        case SYS_GFX_CMD_BLIT:
            return sys_gfx_cmd_blit(s, a, env);

        case SYS_GFX_CMD_PIXELS:
            return sys_gfx_cmd_pixels(s, a, env);

        case SYS_GFX_CMD_RESOURCE:
            return sys_gfx_cmd_resource(s, a, env);

        case SYS_GFX_CMD_SET_CLIP: {
            if ((uint32_t)a[4] & SYS_GFX_F_NO_CLIP) {
                gfx_set_clip(gc, NULL);
                return SYS_OK;
            }
            if (!sys_gfx_rect_ok(a)) return SYS_EINVAL;
            GfxRect r = gfx_rect(a[0], a[1], a[2], a[3]);
            gfx_set_clip(gc, &r);
            return SYS_OK;
        }

        case SYS_GFX_CMD_INTERSECT_CLIP: {
            if (!sys_gfx_rect_ok(a)) return SYS_EINVAL;
            GfxRect r = gfx_rect(a[0], a[1], a[2], a[3]);
            gfx_intersect_clip(gc, &r);
            return SYS_OK;
        }

        case SYS_GFX_CMD_SET_ORIGIN:
            if (!sys_gfx_coord_ok(a[0]) || !sys_gfx_coord_ok(a[1])) return SYS_EINVAL;
            gfx_set_origin(gc, a[0], a[1]);
            return SYS_OK;

        case SYS_GFX_CMD_TRANSLATE: {
            if (!sys_gfx_size_ok(a[0]) || !sys_gfx_size_ok(a[1])) return SYS_EINVAL;
            int32_t ox, oy;
            gfx_get_origin(gc, &ox, &oy);
            if (!sys_gfx_coord_ok(ox + a[0]) || !sys_gfx_coord_ok(oy + a[1])) return SYS_EINVAL;
            gfx_translate(gc, a[0], a[1]);
            return SYS_OK;
        }

        case SYS_GFX_CMD_SET_BLEND:
            if (a[0] != SYS_GFX_BLEND_COPY && a[0] != SYS_GFX_BLEND_ALPHA) return SYS_EINVAL;
            gfx_set_blend(gc, (GfxBlend)a[0]);
            return SYS_OK;

        case SYS_GFX_CMD_TEXT_SCALE:
            if (a[0] < 1 || a[0] > 8) return SYS_EINVAL;
            gfx_set_text_scale(gc, (uint8_t)a[0]);
            return SYS_OK;

        case SYS_GFX_CMD_SAVE:
            return gfx_save(gc) ? SYS_OK : SYS_ERANGE;

        case SYS_GFX_CMD_RESTORE:
            return gfx_restore(gc) ? SYS_OK : SYS_ERANGE;
    }
    return SYS_EINVAL;
}

// Run a batch of `words` command words (already in kernel memory). Returns
// the number of commands run, or the first error (see syscall.h).
static int32_t sys_gfx_execute(SysGfxSurface* s, const uint32_t* cmds, uint32_t words, const SysGfxEnv* env) {
    int32_t first_error = SYS_OK;
    int32_t ran = 0;
    uint32_t pos = 0;

    while (pos < words) {
        uint32_t head = cmds[pos];
        uint32_t op = head & 0xFFFFU;
        uint32_t length = head >> 16;
        if (length == 0 || length > words - pos)
            return (first_error < 0) ? first_error : SYS_EINVAL;    // The rest cannot be parsed

        int32_t result = sys_gfx_command(s, op, (const int32_t*)&cmds[pos + 1], length - 1, env);
        if (result < 0) {
            if (first_error == SYS_OK) first_error = result;
        } else {
            ran++;
        }
        pos += length;
    }
    return (first_error < 0) ? first_error : ran;
}

#endif

