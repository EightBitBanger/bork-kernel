/*
 * Test-only: the kernel's graphics syscalls on a development machine.
 *
 * Built from the kernel's own sources: gfx/gfx_draw.c, gfx_surface.c,
 * gfx_text.c and syscall/calls/sys_gfx_core.h (the command interpreter
 * SYS_GFX_SUBMIT runs), with the few kernel headers they need stubbed in
 * test/kstub. Only the process table, budgets and address-space checks of
 * sys_gfx.h are re-done here, in simplified form.
 *
 * The "screen" is a GFX_SHIM_W x GFX_SHIM_H surface that tests can read
 * back.
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/syscall/calls/sys_gfx_core.h>

#define GFX_SHIM_W  640
#define GFX_SHIM_H  480

static SysGfxSurface* surfaces[SYS_GFX_SURFACE_MAX];
static GfxSurface     screen;
static uint32_t       cmd_buffer[SYS_GFX_SUBMIT_MAX / 4];

// Host pointers are all "user memory" (Linux puts the stack near 4 GB);
// only the first page counts as unmapped
static bool env_check(void* ctx, uint32_t addr, uint32_t len, bool write) {
    (void)ctx; (void)write;
    return addr >= 4096 && (uint64_t)addr + len <= 0x100000000ULL;
}

static bool env_copy_in(void* ctx, void* dst, uint32_t addr, uint32_t len) {
    (void)ctx;
    memcpy(dst, (const void*)(uintptr_t)addr, len);
    return true;
}

static bool env_copy_out(void* ctx, uint32_t addr, const void* src, uint32_t len) {
    (void)ctx;
    memcpy((void*)(uintptr_t)addr, src, len);
    return true;
}

static SysGfxSurface* get(uint32_t handle) {
    if (handle == 0 || handle > SYS_GFX_SURFACE_MAX)
        return NULL;
    return surfaces[handle - 1];
}

static SysGfxSurface* env_lookup(void* ctx, uint32_t handle) {
    (void)ctx;
    return get(handle);
}

static const SysGfxEnv env = { NULL, env_check, env_copy_in, env_copy_out, env_lookup };

static int32_t user_rect(uint32_t addr, SysGfxRect* out) {
    if (!env_check(NULL, addr, sizeof(*out), false))
        return SYS_EFAULT;
    memcpy(out, (const void*)(uintptr_t)addr, sizeof(*out));
    return SYS_OK;
}

// What the test reads back from the "screen"
uint32_t __test_gfx_screen_pixel(int32_t x, int32_t y) {
    const uint32_t* p = screen.pixels ? gfx_surface_pixel(&screen, x, y) : NULL;
    return p ? *p : 0;
}

int32_t __test_gfx_syscall(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    if (screen.pixels == NULL) {
        if (!gfx_surface_create(&screen, GFX_SHIM_W, GFX_SHIM_H))
            return SYS_ENOMEM;
        memset(screen.pixels, 0, GFX_SHIM_W * GFX_SHIM_H * 4);
    }

    switch (n) {
        case SYS_GFX_INFO: {
            const GfxFont* font = gfx_font_default();
            SysGfxInfo info = { GFX_SHIM_W, GFX_SHIM_H, 32, font->advance, font->line_height,
                                SYS_GFX_MAX_DIM, SYS_GFX_SURFACE_MAX, SYS_GFX_PROCESS_BYTES };
            if (!env_check(NULL, a1, sizeof(info), true)) return SYS_EFAULT;
            memcpy((void*)(uintptr_t)a1, &info, sizeof(info));
            return SYS_OK;
        }

        case SYS_GFX_CREATE: {
            if (sys_gfx_surface_bytes((int32_t)a1, (int32_t)a2) == 0) return SYS_EINVAL;
            uint32_t i = 0;
            while (i < SYS_GFX_SURFACE_MAX && surfaces[i]) i++;
            if (i == SYS_GFX_SURFACE_MAX) return SYS_EMFILE;
            SysGfxSurface* s = (SysGfxSurface*)malloc(sizeof(SysGfxSurface));
            int32_t r = s ? sys_gfx_surface_init(s, (int32_t)a1, (int32_t)a2) : SYS_ENOMEM;
            if (r < 0) { free(s); return r; }
            surfaces[i] = s;
            return (int32_t)(i + 1);
        }

        case SYS_GFX_DESTROY: {
            SysGfxSurface* s = get(a1);
            if (!s) return SYS_EBADF;
            sys_gfx_surface_free(s);
            free(s);
            surfaces[a1 - 1] = NULL;
            return SYS_OK;
        }

        case SYS_GFX_SUBMIT: {
            SysGfxSurface* s = get(a1);
            if (!s) return SYS_EBADF;
            if (a3 == 0) return 0;
            if ((a3 & 3) || a3 > SYS_GFX_SUBMIT_MAX) return SYS_EINVAL;
            if (!env_check(NULL, a2, a3, false)) return SYS_EFAULT;
            memcpy(cmd_buffer, (const void*)(uintptr_t)a2, a3);
            return sys_gfx_execute(s, cmd_buffer, a3 / 4, &env);
        }

        case SYS_GFX_WRITE:
        case SYS_GFX_READ: {
            SysGfxSurface* s = get(a1);
            if (!s) return SYS_EBADF;
            SysGfxRect r;
            int32_t e = user_rect(a2, &r);
            if (e < 0) return e;
            return (n == SYS_GFX_WRITE) ? sys_gfx_write_pixels(s, &r, a3, a4, &env)
                                        : sys_gfx_read_pixels(s, &r, a3, a4, &env);
        }

        case SYS_GFX_PRESENT: {
            // Same arithmetic as sys_gfx_present_locked, onto the fake screen
            SysGfxSurface* s = get(a1);
            int32_t x = (int32_t)a3, y = (int32_t)a4;
            if (!s) return SYS_EBADF;
            if (!sys_gfx_coord_ok(x) || !sys_gfx_coord_ok(y)) return SYS_EINVAL;
            GfxRect src = gfx_surface_bounds(&s->surface);
            if (a2 != 0) {
                SysGfxRect r;
                int32_t e = user_rect(a2, &r);
                if (e < 0) return e;
                if (!sys_gfx_size_ok(r.w) || !sys_gfx_size_ok(r.h) || !sys_gfx_coord_ok(r.x) || !sys_gfx_coord_ok(r.y))
                    return SYS_EINVAL;
                GfxRect want = gfx_rect(r.x, r.y, r.w, r.h);
                if (!gfx_rect_intersect(&want, &src, &src)) return SYS_OK;
                x += src.x - r.x;
                y += src.y - r.y;
            }
            GfxContext gc;
            gfx_context_init(&gc, &screen);
            gfx_blit(&gc, &s->surface, &src, x, y);
            return SYS_OK;
        }
    }
    return SYS_ENOSYS;
}


