#include <errno.h>
#include <string.h>
#include "internal.h"

int errno = 0;

int __errno_from_sys(int32_t e) {
    switch (e) {
        case SYS_ENOSYS:       return ENOSYS;
        case SYS_EFAULT:       return EFAULT;
        case SYS_EINVAL:       return EINVAL;
        case SYS_ENOMEM:       return ENOMEM;
        case SYS_ENOENT:       return ENOENT;
        case SYS_EEXIST:       return EEXIST;
        case SYS_EACCES:       return EACCES;
        case SYS_EBADF:        return EBADF;
        case SYS_EMFILE:       return EMFILE;
        case SYS_EISDIR:       return EISDIR;
        case SYS_ENOTDIR:      return ENOTDIR;
        case SYS_ENOTEMPTY:    return ENOTEMPTY;
        case SYS_EBUSY:        return EBUSY;
        case SYS_ENAMETOOLONG: return ENAMETOOLONG;
        case SYS_ERANGE:       return ERANGE;
        case SYS_ENOSPC:       return ENOSPC;
        case SYS_EIO:          return EIO;
        case SYS_EPERM:        return EPERM;
        case SYS_ESRCH:        return ESRCH;
        case SYS_EAGAIN:       return EAGAIN;
        case SYS_EPIPE:        return EPIPE;
        case SYS_EBADFMT:      return ENOEXEC;      // Not a loadable resource file / executable
        case SYS_E2BIG:        return E2BIG;
        case SYS_ETIMEDOUT:    return ETIMEDOUT;
        case SYS_ENOTTY:       return ENOTTY;
        case SYS_ENETDOWN:     return ENETDOWN;
        case SYS_ENETUNREACH:  return ENETUNREACH;
        case SYS_EADDRINUSE:   return EADDRINUSE;
        case SYS_ECONNREFUSED: return ECONNREFUSED;
        case SYS_ECONNRESET:   return ECONNRESET;
        case SYS_ENOTCONN:     return ENOTCONN;
        case SYS_EXDEV:        return EXDEV;
        case SYS_ECANCELED:    return ECANCELED;
        default:               return EIO;
    }
}

char* strerror(int e) {
    switch (e) {
        case 0:             return "Success";
        case EPERM:         return "Operation not permitted";
        case ENOENT:        return "No such file or directory";
        case ESRCH:         return "No such process";
        case EINTR:         return "Interrupted system call";
        case EIO:           return "Input/output error";
        case E2BIG:         return "Argument list too long";
        case ENOEXEC:       return "Exec format error";
        case EBADF:         return "Bad file descriptor";
        case EAGAIN:        return "Resource temporarily unavailable";
        case ENOMEM:        return "Out of memory";
        case EACCES:        return "Permission denied";
        case EFAULT:        return "Bad address";
        case EBUSY:         return "Device or resource busy";
        case EEXIST:        return "File exists";
        case EXDEV:         return "Invalid cross-directory link";
        case ENODEV:        return "No such device";
        case ENOTDIR:       return "Not a directory";
        case EISDIR:        return "Is a directory";
        case EINVAL:        return "Invalid argument";
        case ENFILE:        return "Too many open files in system";
        case EMFILE:        return "Too many open files";
        case ENOTTY:        return "Inappropriate ioctl for device";
        case EFBIG:         return "File too large";
        case ENOSPC:        return "No space left on device";
        case ESPIPE:        return "Illegal seek";
        case EROFS:         return "Read-only file system";
        case EPIPE:         return "Broken pipe";
        case EDOM:          return "Numerical argument out of domain";
        case ERANGE:        return "Numerical result out of range";
        case ENAMETOOLONG:  return "File name too long";
        case ENOSYS:        return "Function not implemented";
        case ENOTEMPTY:     return "Directory not empty";
        case EOVERFLOW:     return "Value too large for defined data type";
        case EILSEQ:        return "Invalid or incomplete multibyte character";
        case ENOTSUP:       return "Operation not supported";
        case ETIMEDOUT:     return "Timed out";
        case ECANCELED:     return "Canceled";
        case EADDRINUSE:    return "Address already in use";
        case ENETDOWN:      return "Network is down";
        case ENETUNREACH:   return "Network is unreachable";
        case ECONNRESET:    return "Connection reset by peer";
        case ENOTCONN:      return "Transport endpoint is not connected";
        case ECONNREFUSED:  return "Connection refused";
        default:            return "Unknown error";
    }
}



