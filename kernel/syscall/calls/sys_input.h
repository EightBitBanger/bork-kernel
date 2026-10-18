#ifndef _KERNEL_SYS_INPUT_H_
#define _KERNEL_SYS_INPUT_H_

#include <stdint.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_ipc.h>
#include <kernel/scheduler/process.h>
#include <kernel/console/display.h>
#include <kernel/console/mouse.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/ipc/ipc.h>

//
// input_warp(handle, x, y, scratch) -> 0
//
// Move the mouse pointer to (x, y), clamped to the screen. Only for the
// process holding the input grab: `handle` must be its open input handle
// (SYS_INPUT_OPEN). Mouse events queued before the call keep their old
// positions; the next movement continues from (x, y).
//
// The handle is checked by the IPC layer itself, with a zero-event
// SYS_INPUT_READ into `scratch` (any writable SysInputEvent of the caller;
// nothing is written or taken from the queue). That keeps one owner check
// for every input call.
//
//   SYS_EBADF    not the caller's input handle
//   SYS_EFAULT   scratch is not writable user memory
//   SYS_EIO      no display to put the pointer on
//

static int32_t sys_input_warp(const SyscallArgs* args) {
    IpcCaller c = sys_ipc_caller();
    if (c.space == NULL)
        return SYS_EFAULT;

    int32_t r = ipc_sys_input_read(&c, args->arg1, args->arg4, 0);
    if (r < 0)
        return r;

    int32_t width  = (int32_t)display_get_width();
    int32_t height = (int32_t)display_get_height();
    if (width <= 0 || height <= 0)
        return SYS_EIO;

    int32_t x = (int32_t)args->arg2;
    int32_t y = (int32_t)args->arg3;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= width)  x = width - 1;
    if (y >= height) y = height - 1;

    // The mouse IRQ updates the same position: keep it out for the write
    uint32_t flags = irq_save();
    mouse_set_position((uint32_t)x, (uint32_t)y);
    irq_restore(flags);
    return SYS_OK;
}

#endif
