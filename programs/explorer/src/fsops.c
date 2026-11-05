/*
 * File system helpers: path text and free names. Copying, moving and
 * deleting are fileop's (<home>/sys/fileop).
 * Only libc calls (and fs_kind), so they run anywhere the libc does.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

bool fs_kind_is_dir(int kind) {
    return kind == SYS_TYPE_DIRECTORY || kind == SYS_TYPE_MOUNT;
}

const char* fs_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

int fs_join(const char* dir, const char* name, char* out, size_t size) {
    size_t dir_len = strlen(dir);
    bool slash = dir_len > 0 && dir[dir_len - 1] != '/';
    int n = snprintf(out, size, "%s%s%s", dir, slash ? "/" : "", name);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

void fs_parent(const char* path, char* out, size_t size) {
    strlcpy(out, path, size);
    char* slash = strrchr(out, '/');
    if (slash == NULL || slash == out)
        strlcpy(out, "/", size);
    else
        *slash = '\0';
}

bool fs_is_inside(const char* path, const char* dir) {
    if (strcmp(dir, "/") == 0)
        return path[0] == '/';
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

void fs_volume(const char* path, char* out, size_t size) {
    // The deepest device root at or above path ("/mnt/usb0/a/b" -> "/mnt/usb0")
    strlcpy(out, "/", size);
    char prefix[PATH_MAX];
    size_t len = strlen(path);
    for (size_t i = 2; i <= len && i < sizeof(prefix); i++) {
        if (path[i] != '/' && path[i] != '\0')
            continue;
        memcpy(prefix, path, i);
        prefix[i] = '\0';
        if (fs_kind(prefix, NULL) == SYS_TYPE_MOUNT)
            strlcpy(out, prefix, size);
    }
}

bool fs_same_volume(const char* a, const char* b) {
    char va[PATH_MAX], vb[PATH_MAX];
    fs_volume(a, va, sizeof(va));
    fs_volume(b, vb, sizeof(vb));
    return strcmp(va, vb) == 0;
}

static bool path_free(const char* path) {
    return fs_kind(path, NULL) < 0 && errno == ENOENT;
}

int fs_unique_path(const char* dir, const char* name, char* out, size_t size) {
    if (fs_join(dir, name, out, size) < 0)
        return -1;
    if (path_free(out))
        return 0;

    // "notes.txt" -> "notes (1).txt"; a leading dot is not an extension
    const char* dot = strrchr(name, '.');
    size_t stem = (dot != NULL && dot != name) ? (size_t)(dot - name) : strlen(name);
    const char* ext = name + stem;

    char candidate[NAME_MAX + 1];
    for (int n = 1; n < 10000; n++) {
        int len = snprintf(candidate, sizeof(candidate), "%.*s (%d)%s", (int)stem, name, n, ext);
        if (len < 0 || (size_t)len >= sizeof(candidate)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (fs_join(dir, candidate, out, size) < 0)
            return -1;
        if (path_free(out))
            return 0;
    }
    errno = EEXIST;
    return -1;
}

