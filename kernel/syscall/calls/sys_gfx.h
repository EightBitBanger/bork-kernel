#ifndef _KERNEL_SYS_GFX_H_
#define _KERNEL_SYS_GFX_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_gfx_core.h>
#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_display.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/process.h>
#include <kernel/kernel_lock.h>
#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

//
// Graphics calls: gfx_info, gfx_create, gfx_destroy, gfx_submit, gfx_write,
// gfx_read, gfx_present (ABI in syscall.h, interpreter in sys_gfx_core.h)
//
// One SysGfxProcess per process that has used a graphics call, on a list
// keyed by process id, created on the first call and dropped by
// syscall_process_exit(). Same shape and rules as the descriptor tables in
// sys_files.h:
//
//   - the list and each table's `users` count: interrupts off
//   - everything else in a table: its mutex, held for a whole call
//   - order: table mutex -> kernel lock (presenting). Never the other way.
//
// Memory: surfaces come from the kernel heap, whose large allocations take
// pages from the kernel VM window (256 MB, shared by everything). So each
// process gets SYS_GFX_PROCESS_BYTES and all processes together
// SYS_GFX_TOTAL_BYTES; past that, gfx_create fails with SYS_ENOMEM instead
// of the kernel running out of address space.
//

#define SYS_GFX_TOTAL_BYTES     (64U << 20)
#define SYS_GFX_EXIT_WAIT_MS    1000

typedef struct SysGfxProcess {
    uint32_t       process_id;
    AddressSpace*  space;           // Spots a reused process id
    uint32_t       users;           // Calls in progress (interrupts off to touch)
    mutex_t        lock;
    SysGfxSurface* surfaces[SYS_GFX_SURFACE_MAX];   // Handle h is surfaces[h - 1]
    SysGfxResource* resources[SYS_RES_MAX];         // Resource handle h is resources[h - 1]
    uint32_t       bytes;           // Pixel memory charged to this process (surfaces and resources)
    uint32_t*      cmd_buffer;      // SYS_GFX_SUBMIT_MAX bytes, allocated on first submit
    struct SysGfxProcess* next;
} SysGfxProcess;

static SysGfxProcess* sys_gfx_list = NULL;
static uint32_t       sys_gfx_total = 0;      // Interrupts off to touch

//
// Tables
//

// Interrupts off
static SysGfxProcess* sys_gfx_find(uint32_t process_id) {
    for (SysGfxProcess* t = sys_gfx_list; t != NULL; t = t->next) {
        if (t->process_id == process_id)
            return t;
    }
    return NULL;
}

// Take `bytes` of pixel budget (process and system-wide). False when over.
static bool sys_gfx_charge(SysGfxProcess* t, uint32_t bytes) {
    if (bytes > SYS_GFX_PROCESS_BYTES - t->bytes)
        return false;
    uint32_t irq_flags = irq_save();
    bool fits = bytes <= SYS_GFX_TOTAL_BYTES - sys_gfx_total;
    if (fits)
        sys_gfx_total += bytes;
    irq_restore(irq_flags);
    if (fits)
        t->bytes += bytes;
    return fits;
}

// Give back the budget a surface held
static void sys_gfx_uncharge(SysGfxProcess* t, uint32_t bytes) {
    t->bytes -= bytes;
    uint32_t irq_flags = irq_save();
    sys_gfx_total -= bytes;
    irq_restore(irq_flags);
}

// Caller holds the table mutex, or no other thread can reach the table
static void sys_gfx_free_handle(SysGfxProcess* t, uint32_t index) {
    SysGfxSurface* s = t->surfaces[index];
    if (s == NULL)
        return;
    uint32_t bytes = sys_gfx_surface_bytes(s->surface.width, s->surface.height);
    sys_gfx_surface_free(s);
    free(s);
    t->surfaces[index] = NULL;
    sys_gfx_uncharge(t, bytes);
}

static void sys_gfx_free_resource(SysGfxProcess* t, uint32_t index) {
    SysGfxResource* r = t->resources[index];
    if (r == NULL)
        return;
    uint32_t bytes = sys_gfx_surface_bytes(r->surface.width, r->surface.height);
    sys_gfx_resource_free(r);
    free(r);
    t->resources[index] = NULL;
    sys_gfx_uncharge(t, bytes);
}

static void sys_gfx_free_all(SysGfxProcess* t) {
    for (uint32_t i = 0; i < SYS_GFX_SURFACE_MAX; i++)
        sys_gfx_free_handle(t, i);
    for (uint32_t i = 0; i < SYS_RES_MAX; i++)
        sys_gfx_free_resource(t, i);
    free(t->cmd_buffer);
    t->cmd_buffer = NULL;
}

// The calling process's table, created if needed, returned LOCKED.
// NULL when out of memory.
static SysGfxProcess* sys_gfx_acquire(void) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return NULL;
    uint32_t process_id = (uint32_t)process_current_id();

    uint32_t irq_flags = irq_save();
    SysGfxProcess* t = sys_gfx_find(process_id);
    if (t == NULL) {
        t = (SysGfxProcess*)malloc(sizeof(SysGfxProcess));
        if (t != NULL) {
            memset(t, 0, sizeof(SysGfxProcess));
            t->process_id = process_id;
            t->space      = space;
            mutex_init(&t->lock);
            t->next = sys_gfx_list;
            sys_gfx_list = t;
        }
    }
    if (t != NULL)
        t->users++;
    irq_restore(irq_flags);

    if (t == NULL)
        return NULL;

    mutex_lock(&t->lock);

    // Left behind by an earlier process with this id that ended without
    // syscall_process_exit(): its surfaces are not ours
    if (t->space != space) {
        sys_gfx_free_all(t);
        t->space = space;
    }
    return t;
}

static void sys_gfx_release(SysGfxProcess* t) {
    mutex_unlock(&t->lock);

    uint32_t irq_flags = irq_save();
    t->users--;
    irq_restore(irq_flags);
}

typedef int32_t (*SysGfxFn)(SysGfxProcess* t, const SyscallArgs* args);

static int32_t sys_gfx_call(SysGfxFn fn, const SyscallArgs* args) {
    SysGfxProcess* t = sys_gfx_acquire();
    if (t == NULL)
        return SYS_ENOMEM;
    int32_t result = fn(t, args);
    sys_gfx_release(t);
    return result;
}

// Free an unlinked table once the calls still inside it have left
static void sys_gfx_destroy_table(SysGfxProcess* t) {
    uint64_t start = timer_get_ms();
    while (1) {
        uint32_t irq_flags = irq_save();
        uint32_t users = t->users;
        irq_restore(irq_flags);
        if (users == 0)
            break;

        if (timer_get_ms() - start >= SYS_GFX_EXIT_WAIT_MS) {
            serial_puts("syscall: process ");
            serial_put_dec(t->process_id);
            serial_puts(" ended inside a graphics call; its surfaces are leaked\n");
            return;
        }
        thread_sleep(1);
    }

    sys_gfx_free_all(t);
    free(t);
}

// Called from syscall_process_exit(). Thread context, interrupts on.
static void sys_gfx_process_exit(uint32_t process_id) {
    uint32_t irq_flags = irq_save();
    SysGfxProcess* t = NULL;
    for (SysGfxProcess** link = &sys_gfx_list; *link != NULL; link = &(*link)->next) {
        if ((*link)->process_id == process_id) {
            t = *link;
            *link = t->next;
            break;
        }
    }
    irq_restore(irq_flags);

    if (t != NULL)
        sys_gfx_destroy_table(t);
}

static SysGfxSurface* sys_gfx_get(SysGfxProcess* t, uint32_t handle) {
    if (handle == 0 || handle > SYS_GFX_SURFACE_MAX)
        return NULL;
    return t->surfaces[handle - 1];
}

//
// User memory, for the interpreter. ctx is the table.
//

static bool sys_gfx_env_check(void* ctx, uint32_t addr, uint32_t len, bool write) {
    return vmm_user_check(((SysGfxProcess*)ctx)->space, addr, len, write);
}

static bool sys_gfx_env_copy_in(void* ctx, void* dst, uint32_t addr, uint32_t len) {
    return vmm_user_copy_from(((SysGfxProcess*)ctx)->space, dst, addr, len);
}

static bool sys_gfx_env_copy_out(void* ctx, uint32_t addr, const void* src, uint32_t len) {
    return vmm_user_copy_to(((SysGfxProcess*)ctx)->space, addr, src, len);
}

static SysGfxSurface* sys_gfx_env_lookup(void* ctx, uint32_t handle) {
    return sys_gfx_get((SysGfxProcess*)ctx, handle);
}

static SysGfxResource* sys_gfx_get_resource(SysGfxProcess* t, uint32_t handle) {
    if (handle == 0 || handle > SYS_RES_MAX)
        return NULL;
    return t->resources[handle - 1];
}

static const SysGfxResource* sys_gfx_env_resource(void* ctx, uint32_t handle) {
    return sys_gfx_get_resource((SysGfxProcess*)ctx, handle);
}

static SysGfxEnv sys_gfx_env(SysGfxProcess* t) {
    SysGfxEnv env = {
        .ctx      = t,
        .check    = sys_gfx_env_check,
        .copy_in  = sys_gfx_env_copy_in,
        .copy_out = sys_gfx_env_copy_out,
        .lookup   = sys_gfx_env_lookup,
        .resource = sys_gfx_env_resource,
    };
    return env;
}

// A SysGfxRect from user memory
static int32_t sys_gfx_user_rect(SysGfxProcess* t, uint32_t addr, SysGfxRect* out) {
    if (!vmm_user_check(t->space, addr, sizeof(SysGfxRect), false) ||
        !vmm_user_copy_from(t->space, out, addr, sizeof(SysGfxRect)))
        return SYS_EFAULT;
    return SYS_OK;
}

//
// gfx_info(SysGfxInfo*)
//

static int32_t sys_gfx_info(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    const GfxFont* font = gfx_font_default();

    SysGfxInfo info = {
        .width        = gfx_display_ready() ? (uint32_t)gfx_display_width()  : 0,
        .height       = gfx_display_ready() ? (uint32_t)gfx_display_height() : 0,
        .bpp          = 32,
        .char_width   = font->advance,
        .line_height  = font->line_height,
        .max_dim      = SYS_GFX_MAX_DIM,
        .max_surfaces = SYS_GFX_SURFACE_MAX,
        .max_bytes    = SYS_GFX_PROCESS_BYTES,
    };

    if (space == NULL || !vmm_user_check(space, args->arg1, sizeof(info), true) ||
        !vmm_user_copy_to(space, args->arg1, &info, sizeof(info)))
        return SYS_EFAULT;
    return SYS_OK;
}

//
// gfx_create(width, height) -> handle
//

static int32_t sys_gfx_create_locked(SysGfxProcess* t, const SyscallArgs* args) {
    int32_t width  = (int32_t)args->arg1;
    int32_t height = (int32_t)args->arg2;
    uint32_t bytes = sys_gfx_surface_bytes(width, height);
    if (bytes == 0)
        return SYS_EINVAL;

    uint32_t index = 0;
    while (index < SYS_GFX_SURFACE_MAX && t->surfaces[index] != NULL)
        index++;
    if (index == SYS_GFX_SURFACE_MAX)
        return SYS_EMFILE;

    // Reserve the budget before allocating anything
    if (!sys_gfx_charge(t, bytes))
        return SYS_ENOMEM;

    SysGfxSurface* s = (SysGfxSurface*)malloc(sizeof(SysGfxSurface));
    int32_t result = (s != NULL) ? sys_gfx_surface_init(s, width, height) : SYS_ENOMEM;
    if (result < 0) {
        free(s);
        sys_gfx_uncharge(t, bytes);
        return result;
    }

    t->surfaces[index] = s;
    return (int32_t)(index + 1);
}

static int32_t sys_gfx_create(const SyscallArgs* args) {
    return sys_gfx_call(sys_gfx_create_locked, args);
}

//
// gfx_destroy(handle)
//

static int32_t sys_gfx_destroy_locked(SysGfxProcess* t, const SyscallArgs* args) {
    if (sys_gfx_get(t, args->arg1) == NULL)
        return SYS_EBADF;
    sys_gfx_free_handle(t, args->arg1 - 1);
    return SYS_OK;
}

static int32_t sys_gfx_destroy(const SyscallArgs* args) {
    return sys_gfx_call(sys_gfx_destroy_locked, args);
}

//
// gfx_submit(handle, cmds, bytes) -> commands run
//

static int32_t sys_gfx_submit_locked(SysGfxProcess* t, const SyscallArgs* args) {
    SysGfxSurface* s = sys_gfx_get(t, args->arg1);
    uint32_t addr  = args->arg2;
    uint32_t bytes = args->arg3;

    if (s == NULL)
        return SYS_EBADF;
    if (bytes == 0)
        return 0;
    if ((bytes & 3U) != 0 || bytes > SYS_GFX_SUBMIT_MAX)
        return SYS_EINVAL;

    // Copied in whole first: the interpreter then reads kernel memory only,
    // and another thread cannot change a command after it was checked
    if (t->cmd_buffer == NULL) {
        t->cmd_buffer = (uint32_t*)malloc(SYS_GFX_SUBMIT_MAX);
        if (t->cmd_buffer == NULL)
            return SYS_ENOMEM;
    }
    if (!vmm_user_check(t->space, addr, bytes, false) ||
        !vmm_user_copy_from(t->space, t->cmd_buffer, addr, bytes))
        return SYS_EFAULT;

    SysGfxEnv env = sys_gfx_env(t);
    return sys_gfx_execute(s, t->cmd_buffer, bytes / 4U, &env);
}

static int32_t sys_gfx_submit(const SyscallArgs* args) {
    return sys_gfx_call(sys_gfx_submit_locked, args);
}

//
// gfx_write / gfx_read(handle, SysGfxRect*, pixels, stride)
//

static int32_t sys_gfx_transfer_locked(SysGfxProcess* t, const SyscallArgs* args, bool write) {
    SysGfxSurface* s = sys_gfx_get(t, args->arg1);
    if (s == NULL)
        return SYS_EBADF;

    SysGfxRect r;
    int32_t result = sys_gfx_user_rect(t, args->arg2, &r);
    if (result < 0)
        return result;

    SysGfxEnv env = sys_gfx_env(t);
    return write ? sys_gfx_write_pixels(s, &r, args->arg3, args->arg4, &env)
                 : sys_gfx_read_pixels(s, &r, args->arg3, args->arg4, &env);
}

static int32_t sys_gfx_write_locked(SysGfxProcess* t, const SyscallArgs* args) {
    return sys_gfx_transfer_locked(t, args, true);
}

static int32_t sys_gfx_read_locked(SysGfxProcess* t, const SyscallArgs* args) {
    return sys_gfx_transfer_locked(t, args, false);
}

static int32_t sys_gfx_write(const SyscallArgs* args) {
    return sys_gfx_call(sys_gfx_write_locked, args);
}

static int32_t sys_gfx_read(const SyscallArgs* args) {
    return sys_gfx_call(sys_gfx_read_locked, args);
}

//
// gfx_present(handle, SysGfxRect* src or 0, x, y)
//
// Needs SYS_SEC_GFX and SYS_SEC_GFX_SCREEN (the table checks both): the
// surface is drawn straight onto the display, over whatever is there.
//

static int32_t sys_gfx_present_locked(SysGfxProcess* t, const SyscallArgs* args) {
    SysGfxSurface* s = sys_gfx_get(t, args->arg1);
    int32_t x = (int32_t)args->arg3;
    int32_t y = (int32_t)args->arg4;
    if (s == NULL)
        return SYS_EBADF;
    if (!sys_gfx_coord_ok(x) || !sys_gfx_coord_ok(y))
        return SYS_EINVAL;

    // Source area, cut to the surface
    GfxRect src = gfx_surface_bounds(&s->surface);
    if (args->arg2 != 0) {
        SysGfxRect r;
        int32_t result = sys_gfx_user_rect(t, args->arg2, &r);
        if (result < 0)
            return result;
        if (!sys_gfx_size_ok(r.w) || !sys_gfx_size_ok(r.h) || !sys_gfx_coord_ok(r.x) || !sys_gfx_coord_ok(r.y))
            return SYS_EINVAL;
        GfxRect want = gfx_rect(r.x, r.y, r.w, r.h);
        if (!gfx_rect_intersect(&want, &src, &src))
            return SYS_OK;      // Nothing to show
        x += src.x - r.x;       // Keep the area's corner where the caller put it
        y += src.y - r.y;
    }

    if (!gfx_display_ready())
        return SYS_EIO;

    // Straight to the screen: into the back buffer, then the changed part to
    // VRAM. The kernel lock keeps this from interleaving with the kernel's
    // own event handling (and anything else drawing under it).
    kernel_lock_acquire();

    GfxContext gc;
    gfx_context_init(&gc, gfx_display_back());
    gfx_blit(&gc, &s->surface, &src, x, y);

    GfxRect shown = gfx_rect(x, y, src.w, src.h);
    GfxRect screen = gfx_surface_bounds(gfx_display_back());
    if (gfx_rect_intersect(&shown, &screen, &shown))
        gfx_display_present(&shown);

    kernel_lock_release();
    return SYS_OK;
}

static int32_t sys_gfx_present(const SyscallArgs* args) {
    return sys_gfx_call(sys_gfx_present_locked, args);
}

#endif



