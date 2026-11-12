
#ifndef _FCNTL_H
#define _FCNTL_H

#include <sys/types.h>

/* Linux values; open() translates them to SYS_O_* (O_TRUNC -> the VFS's
   VFS_OPEN_TRUNCATE, O_APPEND -> VFS_OPEN_APPEND; both need write access).
   os_open() in os/os.h takes the kernel's flags directly. */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);

#endif


