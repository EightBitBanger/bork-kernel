#ifndef _LIBC_INTERNAL_H
#define _LIBC_INTERNAL_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <os/syscall.h>

/* Kernel SYS_E* (negative) -> POSIX errno value */
int __errno_from_sys(int32_t sys_error);

/* Turn a raw syscall result into the POSIX convention: -1 with errno set */
static inline int __sys_ret(int32_t r) {
    if (SYS_IS_ERROR(r)) {
        errno = __errno_from_sys(r);
        return -1;
    }
    return r;
}

/* printf engine: calls emit() with each piece of output, returns the total
   length (or -1 with errno EOVERFLOW past INT_MAX) */
typedef void (*__emit_fn)(void* ctx, const char* s, size_t n);
int __vformat(__emit_fn emit, void* ctx, const char* fmt, va_list ap);

/* gfx_char_width() at text scale 1, without a surface (ui_edit) */
int32_t __gfx_cell_width(void);
int32_t __gfx_line_height(void);

/* Record an error on a gfx surface, reported by its next flush (gfx.c) */
struct gfx_surface;
void __gfx_set_error(struct gfx_surface* s, int e);

/* stdio teardown, called by exit() */
void __stdio_exit(void);

#endif

