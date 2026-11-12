#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int             ssize_t;
typedef long            off_t;      /* The kernel's file positions are 31-bit */
typedef unsigned int    mode_t;
typedef int             pid_t;
typedef unsigned int    uid_t;
typedef unsigned int    gid_t;
typedef unsigned int    ino_t;
typedef unsigned int    dev_t;
typedef unsigned int    nlink_t;
typedef long            blksize_t;
typedef long            blkcnt_t;
typedef long long       time_t;
typedef long            suseconds_t;
typedef unsigned int    useconds_t;

#endif

