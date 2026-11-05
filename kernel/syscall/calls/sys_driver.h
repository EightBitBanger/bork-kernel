#ifndef _KERNEL_SYS_DRIVER_H_
#define _KERNEL_SYS_DRIVER_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/syscall/calls/sys_procfile.h>
#include <kernel/scheduler/process.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/driver/driver.h>
#include <kernel/driver/kdrv_loader.h>
#include <kernel/driver/drvcall.h>
#include <kernel/util/string.h>
#include <kernel/util/snprint.h>

//
// drv_load(path, SysDrvInfo* or 0) -> 0
// drv_unload(name) -> 0
// drv_call(SysDrvCall*) -> 0
// drv_exports(name, index, SysDrvExport*) -> 0
//
// ABI in syscall.h ("Drivers"), the loader in driver/kdrv_loader.c.
// syscall_dispatch() has already required SYS_SEC_ADMIN (and FILE_READ for
// a load). The path is copied and normalized under the caller's file table,
// which is let go before the load: a driver's probe may take a while and
// must not hold up the process's other file calls.
//

_Static_assert(sizeof(SysDrvInfo) == 48, "SysDrvInfo is part of the user ABI");
_Static_assert(SYS_DRV_NAME_MAX == DRIVER_NAME_MAX, "SYS_DRV_NAME_MAX must equal DRIVER_NAME_MAX");
_Static_assert(SYS_DRV_VERSION_MAX == sizeof(((KdrvInfo*)0)->version), "SYS_DRV_VERSION_MAX must match KdrvInfo");
_Static_assert(SYS_DRV_FILE_MAX == KDRV_FILE_MAX, "SYS_DRV_FILE_MAX must equal KDRV_FILE_MAX");
_Static_assert(SYS_DRV_LOADED_MAX == KDRV_LOADED_MAX, "SYS_DRV_LOADED_MAX must equal KDRV_LOADED_MAX");
_Static_assert(sizeof(SysDrvArg) == 8 && sizeof(SysDrvCall) == 40 && sizeof(SysDrvExport) == 104,
               "library driver structures are part of the user ABI");
_Static_assert(SYS_DRV_ARGS_MAX == DRIVER_CALL_ARGS_MAX && SYS_DRV_STRING_MAX == DRIVER_CALL_STRING_MAX &&
               SYS_DRV_OUTPUT_MAX == DRIVER_CALL_OUTPUT_MAX && SYS_DRV_FUNC_MAX == DRIVER_EXPORT_NAME_MAX &&
               SYS_DRV_HELP_MAX == DRIVER_EXPORT_HELP_MAX, "SYS_DRV_* limits must match driver.h");
_Static_assert(SYS_DRV_ARG_INT == DRIVER_ARG_INT && SYS_DRV_ARG_STRING == DRIVER_ARG_STRING,
               "SYS_DRV_ARG_* must match DRIVER_ARG_*");

static int32_t sys_drv_errno(KdrvResult result) {
    switch (result) {
        case KDRV_OK:               return SYS_OK;
        case KDRV_ERR_NOT_FOUND:    return SYS_ENOENT;
        case KDRV_ERR_ACCESS:
        case KDRV_ERR_BUILTIN:      return SYS_EACCES;
        case KDRV_ERR_NOT_DRIVER:
        case KDRV_ERR_BAD_FORMAT:
        case KDRV_ERR_CHECKSUM:
        case KDRV_ERR_TOO_BIG:
        case KDRV_ERR_MISMATCH:     return SYS_EBADFMT;
        case KDRV_ERR_EXISTS:       return SYS_EEXIST;
        case KDRV_ERR_BUSY:         return SYS_EBUSY;
        case KDRV_ERR_FULL:         return SYS_ENOSPC;
        case KDRV_ERR_NO_MEMORY:    return SYS_ENOMEM;
        case KDRV_ERR_IO:
        case KDRV_ERR_REFUSED:      return SYS_EIO;
    }
    return SYS_EIO;
}

// "drv: pid 7 (loadlib) <what>" on COM1, so every load says who asked
static void sys_drv_log(const char* what, const char* subject) {
    ProcessId pid = process_current_id();
    ProcessInfo info;
    bool known = ProcessGetInfo(pid, &info);

    char line[SYS_PATH_MAX + 64];
    int length = snprintf(line, sizeof(line), "drv: pid %d (%s) %s %s\n",
                          (int)pid, known ? info.name : "?", what, subject);
    if (length <= 0)
        return;
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
        line[length - 1] = '\n';
    }
    serial_write(line, (uint32_t)length);
}

static int32_t sys_drv_load(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;

    // The answer's buffer first, so a driver that starts can always say so
    if (args->arg2 != 0 && !vmm_user_check(space, args->arg2, sizeof(SysDrvInfo), true))
        return SYS_EFAULT;

    char* path = (char*)malloc(SYS_PATH_MAX);
    if (path == NULL)
        return SYS_ENOMEM;

    SysProcessFiles* files = sys_files_acquire();
    if (files == NULL) {
        free(path);
        return SYS_ENOMEM;
    }
    int32_t result = sys_user_path(space, files->cwd, args->arg1, path);
    sys_files_release(files);
    if (result != SYS_OK) {
        free(path);
        return result;
    }

    sys_drv_log("loads", path);

    KdrvInfo loaded;
    result = sys_drv_errno(kdrv_load(path, &loaded));
    free(path);
    if (result != SYS_OK || args->arg2 == 0)
        return result;

    SysDrvInfo info;
    memset(&info, 0, sizeof(info));
    memcpy(info.name, loaded.name, sizeof(info.name));
    memcpy(info.version, loaded.version, sizeof(info.version));
    info.name[sizeof(info.name) - 1] = '\0';
    info.version[sizeof(info.version) - 1] = '\0';
    info.image_size = loaded.image_size;
    info.devices    = loaded.devices;

    // The driver runs whatever happens here; a bad pointer only costs the answer
    if (!sys_user_put(space, args->arg2, &info, sizeof(info)))
        return SYS_EFAULT;
    return SYS_OK;
}

static int32_t sys_drv_unload(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;

    char name[SYS_DRV_NAME_MAX];
    int32_t length = sys_user_string(space, args->arg1, name, sizeof(name));
    if (length < 0)
        return length;
    if (length == 0)
        return SYS_ENOENT;

    sys_drv_log("unloads", name);
    return sys_drv_errno(kdrv_unload(name));
}

static int32_t sys_drvcall_errno(DrvCallResult result) {
    switch (result) {
        case DRVCALL_OK:            return SYS_OK;
        case DRVCALL_NO_DRIVER:
        case DRVCALL_NO_FUNCTION:   return SYS_ENOENT;
        case DRVCALL_NOT_LIBRARY:   return SYS_EBADFMT;
        case DRVCALL_BAD_ARGS:      return SYS_EINVAL;
        case DRVCALL_DENIED:        return SYS_EPERM;
    }
    return SYS_EIO;
}

// Everything a call copies into the kernel, in one allocation
typedef struct {
    char              driver[SYS_DRV_NAME_MAX];
    char              function[SYS_DRV_FUNC_MAX];
    SysDrvArg         args[SYS_DRV_ARGS_MAX];
    char              strings[SYS_DRV_ARGS_MAX][SYS_DRV_STRING_MAX];
    char              out[SYS_DRV_OUTPUT_MAX];
    struct DriverCall call;
} SysDrvCallState;

// A name argument: NAME_MAX or longer is SYS_ENAMETOOLONG, "" SYS_ENOENT
static int32_t sys_drv_name(AddressSpace* space, uint32_t address, char* out, uint32_t size) {
    int32_t length = sys_user_string(space, address, out, size);
    if (length < 0)
        return length;
    return (length == 0) ? SYS_ENOENT : SYS_OK;
}

static int32_t sys_drv_call_run(AddressSpace* space, uint32_t user_call, SysDrvCallState* st) {
    SysDrvCall request;
    if (!vmm_user_check(space, user_call, sizeof(request), true) ||
        !vmm_user_copy_from(space, &request, user_call, sizeof(request)))
        return SYS_EFAULT;

    int32_t result = sys_drv_name(space, request.driver, st->driver, sizeof(st->driver));
    if (result == SYS_OK)
        result = sys_drv_name(space, request.function, st->function, sizeof(st->function));
    if (result != SYS_OK)
        return result;

    if (request.argc > SYS_DRV_ARGS_MAX)
        return SYS_EINVAL;
    if (request.argc != 0 &&
        (!vmm_user_check(space, request.args, request.argc * sizeof(SysDrvArg), false) ||
         !vmm_user_copy_from(space, st->args, request.args, request.argc * sizeof(SysDrvArg))))
        return SYS_EFAULT;

    // The text answer's buffer first, so a function that runs can always answer
    uint32_t out_size = request.out_size;
    if (request.out == 0)
        out_size = 0;
    if (out_size > SYS_DRV_OUTPUT_MAX)
        out_size = SYS_DRV_OUTPUT_MAX;
    if (out_size != 0 && !vmm_user_check(space, request.out, out_size, true))
        return SYS_EFAULT;

    struct DriverCall* call = &st->call;
    call->argc = request.argc;
    for (uint32_t i = 0; i < request.argc; i++) {
        const SysDrvArg* arg = &st->args[i];
        if (arg->type == SYS_DRV_ARG_INT) {
            call->args[i].type = DRIVER_ARG_INT;
            call->args[i].i    = (int32_t)arg->value;
        } else if (arg->type == SYS_DRV_ARG_STRING) {
            int32_t length = sys_user_string(space, arg->value, st->strings[i], SYS_DRV_STRING_MAX);
            if (length < 0)
                return length;
            call->args[i].type = DRIVER_ARG_STRING;
            call->args[i].s    = st->strings[i];
        } else {
            return SYS_EINVAL;
        }
    }
    call->out      = (out_size != 0) ? st->out : NULL;
    call->out_size = out_size;

    int32_t value = 0;
    result = sys_drvcall_errno(driver_call(st->driver, st->function, call,
                                           process_current_security(), &value));
    if (result != SYS_OK)
        return result;

    // The function has run whatever happens here; a bad pointer only costs the answer
    if (call->out_length != 0 && !sys_user_put(space, request.out, st->out, call->out_length + 1))
        return SYS_EFAULT;
    if (out_size != 0 && call->out_length == 0 && !sys_user_put(space, request.out, "", 1))
        return SYS_EFAULT;

    request.result     = value;
    request.out_length = call->out_length;
    if (!sys_user_put(space, user_call + offsetof(SysDrvCall, result), &request.result,
                      sizeof(request.result) + sizeof(request.out_length)))
        return SYS_EFAULT;
    return SYS_OK;
}

static int32_t sys_drv_call(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;

    // About 4 KB: too much for a syscall's kernel stack
    SysDrvCallState* st = (SysDrvCallState*)malloc(sizeof(SysDrvCallState));
    if (st == NULL)
        return SYS_ENOMEM;
    memset(st, 0, sizeof(*st));
    int32_t result = sys_drv_call_run(space, args->arg1, st);
    free(st);
    return result;
}

static int32_t sys_drv_exports(const SyscallArgs* args) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return SYS_EFAULT;
    if (!vmm_user_check(space, args->arg3, sizeof(SysDrvExport), true))
        return SYS_EFAULT;

    char name[SYS_DRV_NAME_MAX];
    int32_t result = sys_drv_name(space, args->arg1, name, sizeof(name));
    if (result != SYS_OK)
        return result;

    DrvExportInfo info;
    result = sys_drvcall_errno(driver_export_info(name, args->arg2, &info));
    if (result != SYS_OK)
        return result;

    SysDrvExport out;
    memset(&out, 0, sizeof(out));
    memcpy(out.name, info.name, sizeof(out.name));
    memcpy(out.params, info.params, sizeof(info.params));
    memcpy(out.help, info.help, sizeof(out.help));
    out.security = info.security;
    return sys_user_put(space, args->arg3, &out, sizeof(out)) ? SYS_OK : SYS_EFAULT;
}

#endif
