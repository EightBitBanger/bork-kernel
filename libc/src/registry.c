/*
 * The system registry (os/registry.h).
 */

#include <errno.h>
#include <string.h>
#include <os/registry.h>
#include <os/syscall.h>
#include "internal.h"

ssize_t os_reg_get(const char* path, const char* name, void* buf, size_t size) {
    return __sys_ret(syscall4(SYS_REG_GET, (uint32_t)path, (uint32_t)name, (uint32_t)buf, (uint32_t)size));
}

int os_reg_get_string(const char* path, const char* name, char* out, size_t size) {
    if (out == NULL || size == 0) {
        errno = EINVAL;
        return -1;
    }
    out[0] = '\0';
    // One byte kept back, for a string stored without its terminator
    ssize_t n = os_reg_get(path, name, out, size - 1);
    if (n < 0) {
        out[0] = '\0';
        return -1;
    }
    out[n] = '\0';
    return 0;
}

int os_reg_key(const char* path, unsigned index, char* out, size_t size) {
    return __sys_ret(syscall4(SYS_REG_KEYS, (uint32_t)path, (uint32_t)index, (uint32_t)out, (uint32_t)size));
}

int os_reg_set(const char* path, const char* name, const void* data, size_t size) {
    return __sys_ret(syscall4(SYS_REG_SET, (uint32_t)path, (uint32_t)name, (uint32_t)data, (uint32_t)size));
}

int os_reg_set_string(const char* path, const char* name, const char* text) {
    if (text == NULL)
        text = "";
    return os_reg_set(path, name, text, strlen(text) + 1);
}

int os_reg_value(const char* path, unsigned index, char* out, size_t size) {
    return __sys_ret(syscall4(SYS_REG_VALUES, (uint32_t)path, (uint32_t)index, (uint32_t)out, (uint32_t)size));
}

int os_reg_delete(const char* path, const char* name) {
    return __sys_ret(syscall2(SYS_REG_DELETE, (uint32_t)path, (uint32_t)name));
}

int os_reg_mkkey(const char* path) {
    return __sys_ret(syscall1(SYS_REG_MKKEY, (uint32_t)path));
}

int os_reg_rename(const char* path, const char* name, const char* new_name) {
    return __sys_ret(syscall3(SYS_REG_RENAME, (uint32_t)path, (uint32_t)name, (uint32_t)new_name));
}
