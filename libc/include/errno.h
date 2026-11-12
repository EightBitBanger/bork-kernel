#ifndef _ERRNO_H
#define _ERRNO_H

/* Single-threaded processes: errno is a plain global */
extern int errno;

/* Linux/POSIX numbering, so ported code that switches on values still works */
#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define EWOULDBLOCK     EAGAIN
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define ENAMETOOLONG    36
#define ENOSYS          38
#define ENOTEMPTY       39
#define EOVERFLOW       75
#define EILSEQ          84
#define ENOTSUP         95
#define EADDRINUSE      98
#define ENETDOWN       100
#define ENETUNREACH    101
#define ECONNRESET     104
#define ENOTCONN       107
#define ECONNREFUSED   111
#define ETIMEDOUT       110
#define ECANCELED      125
#define EOPNOTSUPP      ENOTSUP

#endif

