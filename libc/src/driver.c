/*
 * Loadable drivers (os/driver.h) on SYS_DRV_LOAD / SYS_DRV_UNLOAD.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <os/driver.h>
#include "internal.h"

int LoadLibrary(const char* path, drv_info_t* info) {
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall2(SYS_DRV_LOAD, (uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)info));
}

int FreeLibrary(const char* name) {
    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall1(SYS_DRV_UNLOAD, (uint32_t)(uintptr_t)name));
}

/* The fixed part of kernel/driver/kdrv.h's KdrvHeader this needs */
#define DRV_HEADER_SIZE     64
#define DRV_HEADER_NAME     48

int drv_check(const char* path, char* name) {
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    unsigned char header[DRV_HEADER_SIZE];
    ssize_t got = 0;
    while (got < (ssize_t)sizeof(header)) {
        ssize_t n = read(fd, header + got, sizeof(header) - (size_t)got);
        if (n < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        if (n == 0)
            break;
        got += n;
    }
    close(fd);

    if (got < (ssize_t)sizeof(header) || memcmp(header, "kdrv", 4) != 0)
        return 0;
    /* version 1, a 64-byte header, and a name that ends inside its field */
    if (header[4] != 1 || header[5] != 0 || header[6] != DRV_HEADER_SIZE || header[7] != 0)
        return 0;
    if (header[DRV_HEADER_NAME] == 0 || memchr(header + DRV_HEADER_NAME, 0, DRV_NAME_MAX) == NULL)
        return 0;

    if (name != NULL)
        memcpy(name, header + DRV_HEADER_NAME, DRV_NAME_MAX);
    return 1;
}

/*
 * Library drivers on SYS_DRV_CALL / SYS_DRV_EXPORTS
 */

int drv_call(const char* driver, const char* function, const drv_arg_t* args, unsigned argc,
             int32_t* result, char* out, size_t out_size) {
    if (driver == NULL || function == NULL || (argc != 0 && args == NULL) || (out == NULL && out_size != 0)) {
        errno = EINVAL;
        return -1;
    }
    SysDrvCall call;
    memset(&call, 0, sizeof(call));
    call.driver   = (uint32_t)(uintptr_t)driver;
    call.function = (uint32_t)(uintptr_t)function;
    call.args     = (uint32_t)(uintptr_t)args;
    call.argc     = argc;
    call.out      = (uint32_t)(uintptr_t)out;
    call.out_size = (out_size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)out_size;
    if (out_size != 0)
        out[0] = '\0';

    if (__sys_ret(syscall1(SYS_DRV_CALL, (uint32_t)(uintptr_t)&call)) < 0)
        return -1;
    if (result != NULL)
        *result = call.result;
    return 0;
}

int drv_call_ints(const char* driver, const char* function, int32_t* result, unsigned count, ...) {
    if (count > DRV_ARGS_MAX) {
        errno = EINVAL;
        return -1;
    }
    drv_arg_t args[DRV_ARGS_MAX];
    va_list ap;
    va_start(ap, count);
    for (unsigned i = 0; i < count; i++)
        args[i] = drv_arg_int(va_arg(ap, int32_t));
    va_end(ap);
    return drv_call(driver, function, args, count, result, NULL, 0);
}

int drv_exports(const char* driver, unsigned index, drv_export_t* out) {
    if (driver == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    int32_t r = syscall3(SYS_DRV_EXPORTS, (uint32_t)(uintptr_t)driver, index, (uint32_t)(uintptr_t)out);
    if (r == SYS_ENOENT) {
        /* Past the last, or no such driver: index 0 tells them apart */
        if (index == 0) {
            errno = ENOENT;
            return -1;
        }
        return 0;
    }
    if (__sys_ret(r) < 0)
        return -1;
    out->name[sizeof(out->name) - 1] = '\0';
    out->params[sizeof(out->params) - 1] = '\0';
    out->help[sizeof(out->help) - 1] = '\0';
    return 1;
}

int drv_find_export(const char* driver, const char* function, drv_export_t* out) {
    if (function == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned i = 0; ; i++) {
        int got = drv_exports(driver, i, out);
        if (got <= 0)
            return got;
        if (strcmp(out->name, function) == 0)
            return 1;
    }
}

/* A whole int32_t: decimal, 0x hex, 0 octal, signed; nothing after it */
static int parse_int(const char* text, int32_t* value) {
    if (text == NULL || *text == '\0')
        return -1;
    char* end;
    errno = 0;
    long long v = strtoll(text, &end, 0);
    if (errno != 0 || *end != '\0')
        return -1;
    /* Signed, or an unsigned 32-bit pattern such as 0xFFFFFFFF */
    if (v < -2147483648LL || v > 4294967295LL)
        return -1;
    *value = (int32_t)(uint32_t)v;
    return 0;
}

int drv_call_strings(const char* driver, const char* function, int argc, char** argv,
                     int32_t* result, char* out, size_t out_size) {
    drv_export_t e;
    int found = drv_find_export(driver, function, &e);
    if (found < 0)
        return -1;
    if (found == 0) {
        errno = ENOENT;
        return -1;
    }
    if (argc < 0 || (size_t)argc != strlen(e.params)) {
        errno = EINVAL;
        return -1;
    }

    drv_arg_t args[DRV_ARGS_MAX];
    for (int i = 0; i < argc; i++) {
        if (e.params[i] == SYS_DRV_ARG_INT) {
            int32_t v;
            if (parse_int(argv[i], &v) < 0) {
                errno = EINVAL;
                return -1;
            }
            args[i] = drv_arg_int(v);
        } else {
            args[i] = drv_arg_string(argv[i]);
        }
    }
    return drv_call(driver, function, args, (unsigned)argc, result, out, out_size);
}
