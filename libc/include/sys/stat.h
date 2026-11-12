#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

#define S_IFMT      0170000
#define S_IFDIR     0040000
#define S_IFREG     0100000

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

#define S_IRWXU     0700
#define S_IRUSR     0400
#define S_IWUSR     0200
#define S_IXUSR     0100
#define S_IRWXG     0070
#define S_IRGRP     0040
#define S_IWGRP     0020
#define S_IXGRP     0010
#define S_IRWXO     0007
#define S_IROTH     0004
#define S_IWOTH     0002
#define S_IXOTH     0001

/*
 * Filled from the kernel's SysStat. The kernel keeps one read/write/execute
 * set per file, so it shows up in all three permission groups. A device root
 * (SYS_TYPE_MOUNT) reports as a directory; st_ino is always 0.
 */
struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    off_t     st_size;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    time_t    st_atime;     /* = st_mtime (no access times) */
    time_t    st_mtime;
    time_t    st_ctime;     /* Creation time */
};

int stat(const char* restrict path, struct stat* restrict out);
int lstat(const char* restrict path, struct stat* restrict out);
int fstat(int fd, struct stat* out);
int mkdir(const char* path, mode_t mode);

/* The kernel keeps one read/write/execute set per file: any of the three
   groups' r, w or x bits in `mode` sets it. See os_set_permissions. */
int chmod(const char* path, mode_t mode);

#endif


