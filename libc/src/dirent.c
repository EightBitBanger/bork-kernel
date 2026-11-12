/*
 * Directory streams over SYS_READDIR, which hands out one entry name per
 * index. The directory is remembered by absolute path, so a chdir() while
 * it is open changes nothing. If the directory changes between readdir()
 * calls an entry may be skipped or repeated, as the kernel notes.
 */

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <os/os.h>
#include "internal.h"

struct __DIR {
    uint32_t      index;
    struct dirent entry;
    char          path[SYS_PATH_MAX];
};

DIR* opendir(const char* path) {
    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (d == NULL)
        return NULL;

    struct stat st;
    if (os_abspath(path, d->path, sizeof(d->path)) == NULL || stat(d->path, &st) < 0) {
        free(d);
        return NULL;
    }
    if (!S_ISDIR(st.st_mode)) {
        free(d);
        errno = ENOTDIR;
        return NULL;
    }
    d->index = 0;
    return d;
}

struct dirent* readdir(DIR* d) {
    for (;;) {
        uint32_t index = d->index++;
        int32_t r = syscall4(SYS_READDIR, (uint32_t)d->path, index,
                             (uint32_t)d->entry.d_name, sizeof(d->entry.d_name));
        if (r == 0)
            return NULL;            // Past the last entry; errno untouched
        if (r == SYS_EIO)
            continue;               // That one entry is unreadable: skip it
        if (SYS_IS_ERROR(r)) {
            errno = __errno_from_sys(r);
            return NULL;
        }

        d->entry.d_ino = index + 1;
        d->entry.d_type = DT_UNKNOWN;

        // Fill in the type: one stat per entry, but callers almost always want it
        char full[SYS_PATH_MAX];
        size_t dir_len = strlen(d->path);
        size_t name_len = (size_t)r;
        size_t slash = (dir_len > 1) ? 1 : 0;
        if (dir_len + slash + name_len + 1 <= sizeof(full)) {
            memcpy(full, d->path, dir_len);
            if (slash) full[dir_len] = '/';
            memcpy(full + dir_len + slash, d->entry.d_name, name_len + 1);

            SysStat s;
            if (!SYS_IS_ERROR(syscall2(SYS_STAT, (uint32_t)full, (uint32_t)&s)))
                d->entry.d_type = (s.type == SYS_TYPE_FILE) ? DT_REG : DT_DIR;
        }
        return &d->entry;
    }
}

void rewinddir(DIR* d) {
    d->index = 0;
}

int closedir(DIR* d) {
    free(d);
    return 0;
}

