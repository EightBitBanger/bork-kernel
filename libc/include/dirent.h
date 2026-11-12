#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

#define DT_UNKNOWN  0
#define DT_DIR      4
#define DT_REG      8

struct dirent {
    ino_t         d_ino;        /* Entry index + 1 (the kernel has no inodes) */
    unsigned char d_type;       /* DT_DIR, DT_REG or DT_UNKNOWN */
    char          d_name[256];
};

typedef struct __DIR DIR;

DIR*           opendir(const char* path);
struct dirent* readdir(DIR* dir);
void           rewinddir(DIR* dir);
int            closedir(DIR* dir);

#endif

