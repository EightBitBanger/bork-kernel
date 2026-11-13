
/*
 * POSIX file and process calls on top of the kernel's syscalls.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <os/os.h>
#include "internal.h"

static char* empty_environ[] = { NULL };
char** environ = empty_environ;

//
// Descriptors
//

int open(const char* path, int flags, ...) {
    uint32_t sf;
    switch (flags & O_ACCMODE) {
        case O_RDONLY: sf = SYS_O_READ;                break;
        case O_WRONLY: sf = SYS_O_WRITE;               break;
        case O_RDWR:   sf = SYS_O_READ | SYS_O_WRITE;  break;
        default:       errno = EINVAL; return -1;
    }

    // The kernel rejects TRUNC/APPEND without WRITE; POSIX leaves
    // O_RDONLY|O_TRUNC unspecified, so it is simply ignored here
    if (sf & SYS_O_WRITE) {
        if (flags & O_TRUNC)  sf |= SYS_O_TRUNC;
        if (flags & O_APPEND) sf |= SYS_O_APPEND;
    }
    if (flags & O_CREAT) {
        sf |= SYS_O_CREATE;
        if (flags & O_EXCL) sf |= SYS_O_EXCL;
    }
    // Permissions (the mode argument) are fixed by the kernel at read/write

    return __sys_ret(syscall2(SYS_OPEN, (uint32_t)path, sf));
}

int os_open(const char* path, uint32_t flags) {
    // The kernel checks the combinations (TRUNCATE / APPEND without WRITE,
    // EXCL without CREATE, unknown bits: EINVAL)
    return __sys_ret(syscall2(SYS_OPEN, (uint32_t)path, flags));
}

int creat(const char* path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int close(int fd) {
    return __sys_ret(syscall1(SYS_CLOSE, (uint32_t)fd));
}

ssize_t read(int fd, void* buf, size_t count) {
    if (count > SSIZE_MAX) count = SSIZE_MAX;
    return __sys_ret(syscall3(SYS_READ, (uint32_t)fd, (uint32_t)buf, (uint32_t)count));
}

ssize_t write(int fd, const void* buf, size_t count) {
    if (count > SSIZE_MAX) count = SSIZE_MAX;
    return __sys_ret(syscall3(SYS_WRITE, (uint32_t)fd, (uint32_t)buf, (uint32_t)count));
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd >= 0 && fd < SYS_FD_FIRST) {
        errno = ESPIPE;     // The console has no position
        return -1;
    }
    return __sys_ret(syscall3(SYS_SEEK, (uint32_t)fd, (uint32_t)offset, (uint32_t)whence));
}

int isatty(int fd) {
    if (fd >= 0 && fd < SYS_FD_FIRST)
        return 1;
    errno = (fd >= SYS_FD_FIRST && fd < SYS_FD_MAX) ? ENOTTY : EBADF;
    return 0;
}

// Every request the kernel knows takes one pointer
int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    return __sys_ret(syscall3(SYS_IOCTL, (uint32_t)fd, (uint32_t)request, (uint32_t)(uintptr_t)arg));
}

//
// Metadata
//

static void stat_from_sys(const SysStat* s, struct stat* out) {
    memset(out, 0, sizeof(*out));

    mode_t perm = 0;
    if (s->permissions & SYS_PERM_READ)    perm |= S_IRUSR | S_IRGRP | S_IROTH;
    if (s->permissions & SYS_PERM_WRITE)   perm |= S_IWUSR | S_IWGRP | S_IWOTH;
    if (s->permissions & SYS_PERM_EXECUTE) perm |= S_IXUSR | S_IXGRP | S_IXOTH;

    out->st_mode    = perm | ((s->type == SYS_TYPE_FILE) ? S_IFREG : S_IFDIR);
    out->st_nlink   = 1;
    out->st_size    = (off_t)s->size;
    out->st_blksize = 512;
    out->st_blocks  = (blkcnt_t)((s->size + 511) / 512);
    out->st_mtime   = s->modified;
    out->st_atime   = s->modified;
    out->st_ctime   = s->created;
}

int stat(const char* restrict path, struct stat* restrict out) {
    SysStat s;
    if (__sys_ret(syscall2(SYS_STAT, (uint32_t)path, (uint32_t)&s)) < 0)
        return -1;
    stat_from_sys(&s, out);
    return 0;
}

int lstat(const char* restrict path, struct stat* restrict out) {
    return stat(path, out);     // No symbolic links
}

int fstat(int fd, struct stat* out) {
    if (fd >= 0 && fd < SYS_FD_FIRST) {
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IRUSR | S_IWUSR;   // Character-device-like console
        return 0;
    }
    SysStat s;
    if (__sys_ret(syscall2(SYS_FSTAT, (uint32_t)fd, (uint32_t)&s)) < 0)
        return -1;
    stat_from_sys(&s, out);
    return 0;
}

int access(const char* path, int mode) {
    SysStat s;
    if (__sys_ret(syscall2(SYS_STAT, (uint32_t)path, (uint32_t)&s)) < 0)
        return -1;
    if (((mode & R_OK) && !(s.permissions & SYS_PERM_READ)) ||
        ((mode & W_OK) && !(s.permissions & SYS_PERM_WRITE)) ||
        ((mode & X_OK) && !(s.permissions & SYS_PERM_EXECUTE))) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

//
// Paths
//

int mkdir(const char* path, mode_t mode) {
    (void)mode;
    return __sys_ret(syscall1(SYS_MKDIR, (uint32_t)path));
}

static int path_type(const char* path) {
    SysStat s;
    if (__sys_ret(syscall2(SYS_STAT, (uint32_t)path, (uint32_t)&s)) < 0)
        return -1;
    return (int)s.type;
}

int unlink(const char* path) {
    int type = path_type(path);
    if (type < 0)
        return -1;
    if (type != SYS_TYPE_FILE) {
        errno = EISDIR;
        return -1;
    }
    return __sys_ret(syscall1(SYS_REMOVE, (uint32_t)path));
}

int rmdir(const char* path) {
    int type = path_type(path);
    if (type < 0)
        return -1;
    if (type == SYS_TYPE_FILE) {
        errno = ENOTDIR;
        return -1;
    }
    return __sys_ret(syscall1(SYS_REMOVE, (uint32_t)path));
}

int truncate(const char* path, off_t length) {
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    return __sys_ret(syscall2(SYS_TRUNCATE, (uint32_t)path, (uint32_t)length));
}

int chdir(const char* path) {
    return __sys_ret(syscall1(SYS_CHDIR, (uint32_t)path));
}

char* getcwd(char* buf, size_t size) {
    char* owned = NULL;
    if (buf == NULL) {
        // Common extension: allocate when no buffer is given
        if (size == 0) size = SYS_PATH_MAX;
        buf = owned = (char*)malloc(size);
        if (buf == NULL)
            return NULL;
    } else if (size == 0) {
        errno = EINVAL;
        return NULL;
    }

    if (__sys_ret(syscall2(SYS_GETCWD, (uint32_t)buf, (uint32_t)size)) < 0) {
        free(owned);
        return NULL;
    }
    return buf;
}

char* os_abspath(const char* path, char* out, size_t size) {
    char scratch[SYS_PATH_MAX * 2];
    size_t path_len = strlen(path);

    if (path_len == 0) {
        errno = ENOENT;
        return NULL;
    }

    if (path[0] == '/') {
        if (path_len >= sizeof(scratch)) { errno = ENAMETOOLONG; return NULL; }
        memcpy(scratch, path, path_len + 1);
    } else {
        if (getcwd(scratch, SYS_PATH_MAX) == NULL)
            return NULL;
        size_t base_len = strlen(scratch);
        if (base_len + 1 + path_len + 1 > sizeof(scratch)) { errno = ENAMETOOLONG; return NULL; }
        scratch[base_len] = '/';
        memcpy(scratch + base_len + 1, path, path_len + 1);
    }

    // Fold "." and ".." as text, like vfs_path_normalize (there are no links)
    if (size < 2) { errno = ERANGE; return NULL; }
    size_t length = 0;
    out[length++] = '/';

    char* save = NULL;
    for (char* name = strtok_r(scratch, "/", &save); name != NULL; name = strtok_r(NULL, "/", &save)) {
        if (strcmp(name, ".") == 0)
            continue;
        if (strcmp(name, "..") == 0) {
            while (length > 1 && out[length - 1] != '/') length--;
            if (length > 1) length--;
            continue;
        }
        size_t name_len = strlen(name);
        size_t slash = (length > 1) ? 1 : 0;
        if (length + slash + name_len + 1 > size) { errno = ENAMETOOLONG; return NULL; }
        if (slash) out[length++] = '/';
        memcpy(out + length, name, name_len);
        length += name_len;
    }
    out[length] = '\0';
    return out;
}

char* realpath(const char* restrict path, char* restrict resolved) {
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    char buf[PATH_MAX];
    if (os_abspath(path, buf, sizeof(buf)) == NULL)
        return NULL;

    struct stat st;
    if (stat(buf, &st) < 0)
        return NULL;

    if (resolved == NULL)
        return strdup(buf);
    memcpy(resolved, buf, strlen(buf) + 1);
    return resolved;
}

//
// Process
//

pid_t getpid(void) {
    return (pid_t)syscall0(SYS_GETPID);
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    (void)path; (void)argv; (void)envp;
    errno = ENOSYS;
    return -1;
}

int execv(const char* path, char* const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char* file, char* const argv[]) {
    return execve(file, argv, environ);
}

void _exit(int status) {
    syscall1(SYS_EXIT, (uint32_t)status);
    for (;;) { }    // SYS_EXIT does not return
}

void _Exit(int status) {
    _exit(status);
}

void* sbrk(intptr_t increment) {
    int32_t r = syscall1(SYS_SBRK, (uint32_t)increment);
    if (SYS_IS_ERROR(r)) {
        errno = __errno_from_sys(r);
        return (void*)-1;
    }
    return (void*)(uintptr_t)(uint32_t)r;
}

int brk(void* addr) {
    void* cur = sbrk(0);
    if (cur == (void*)-1)
        return -1;
    return (sbrk((intptr_t)((char*)addr - (char*)cur)) == (void*)-1) ? -1 : 0;
}

int sched_yield(void) {
    return __sys_ret(syscall0(SYS_YIELD));
}

int os_sleep_ms(uint32_t ms) {
    return __sys_ret(syscall1(SYS_SLEEP, ms));
}

unsigned int sleep(unsigned int seconds) {
    while (seconds > 0) {
        unsigned int chunk = (seconds > 1000000U) ? 1000000U : seconds;   // Keep ms in 32 bits
        syscall1(SYS_SLEEP, chunk * 1000U);
        seconds -= chunk;
    }
    return 0;
}

int usleep(useconds_t usec) {
    // The kernel sleeps in milliseconds: round up, never sleep less than asked
    return __sys_ret(syscall1(SYS_SLEEP, (usec + 999U) / 1000U));
}

//
// Kernel-specific
//

ssize_t os_print(const char* buf, size_t length) {
    return __sys_ret(syscall2(SYS_PRINT, (uint32_t)buf, (uint32_t)length));
}

int os_random(void* buf, size_t length) {
    unsigned char* p = (unsigned char*)buf;
    while (length > 0) {
        size_t n = length < SYS_RANDOM_MAX ? length : SYS_RANDOM_MAX;
        ssize_t got = __sys_ret(syscall2(SYS_RANDOM, (uint32_t)p, (uint32_t)n));
        if (got <= 0)
            return -1;
        p += got;
        length -= (size_t)got;
    }
    return 0;
}

int os_copy(const char* src, const char* dest) {
    return __sys_ret(syscall2(SYS_COPY, (uint32_t)src, (uint32_t)dest));
}

int os_finfo(const char* path, os_finfo_t* out) {
    if (path == NULL || out == NULL) {
        errno = EFAULT;
        return -1;
    }
    return __sys_ret(syscall2(SYS_FINFO, (uint32_t)path, (uint32_t)out));
}

int os_sync(void) {
    return __sys_ret(syscall0(SYS_SYNC));
}

void sync(void) {
    (void)os_sync();
}

int os_poweroff(void) {
    return __sys_ret(syscall1(SYS_POWER, SYS_POWER_OFF));
}

int os_restart(void) {
    return __sys_ret(syscall1(SYS_POWER, SYS_POWER_RESTART));
}

int os_set_permissions(const char* path, uint32_t permissions) {
    return __sys_ret(syscall2(SYS_CHMOD, (uint32_t)path, permissions));
}

int os_set_policy(const char* path, uint32_t policy) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    return __sys_ret(syscall2(SYS_SETPOLICY, (uint32_t)path, policy));
}

int os_set_security(const char* path, uint32_t mask) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    return __sys_ret(syscall2(SYS_SETSECURITY, (uint32_t)path, mask));
}

int os_set_write_security(const char* path, uint32_t mask) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    return __sys_ret(syscall2(SYS_SETWRITESEC, (uint32_t)path, mask));
}

int os_set_exec_permissions(const char* path, uint32_t allow, uint32_t deny) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    return __sys_ret(syscall3(SYS_SETEXECPERM, (uint32_t)path, allow, deny));
}

const char* os_policy_name(uint32_t policy) {
    switch (policy) {
    case SYS_POLICY_USER:    return "user";
    case SYS_POLICY_ADMIN:   return "admin";
    case SYS_POLICY_SYSTEM:  return "system";
    case SYS_POLICY_NETWORK: return "network";
    default:                 return NULL;
    }
}

int chmod(const char* path, mode_t mode) {
    uint32_t p = 0;
    if (mode & (S_IRUSR | S_IRGRP | S_IROTH)) p |= SYS_PERM_READ;
    if (mode & (S_IWUSR | S_IWGRP | S_IWOTH)) p |= SYS_PERM_WRITE;
    if (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) p |= SYS_PERM_EXECUTE;
    return os_set_permissions(path, p);
}

int os_rename(const char* path, const char* new_name) {
    return __sys_ret(syscall2(SYS_RENAME, (uint32_t)path, (uint32_t)new_name));
}

int os_move(const char* src, const char* dest) {
    return __sys_ret(syscall2(SYS_MOVE, (uint32_t)src, (uint32_t)dest));
}

char* os_gethome(char* buf, size_t size) {
    char* owned = NULL;
    if (buf == NULL) {
        if (size == 0) size = SYS_PATH_MAX;
        buf = owned = (char*)malloc(size);
        if (buf == NULL)
            return NULL;
    } else if (size == 0) {
        errno = EINVAL;
        return NULL;
    }

    if (__sys_ret(syscall2(SYS_GETHOME, (uint32_t)buf, (uint32_t)size)) < 0) {
        free(owned);
        return NULL;
    }
    return buf;
}

char* os_syspath(const char* sub, char* out, size_t size) {
    char home[SYS_PATH_MAX];
    if (os_gethome(home, sizeof(home)) == NULL)
        return NULL;

    while (sub != NULL && *sub == '/')
        sub++;

    int n = (sub == NULL || *sub == '\0')
          ? snprintf(out, size, "%s/sys", home)
          : snprintf(out, size, "%s/sys/%s", home, sub);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return out;
}

// Folders under home that hold programs, searched in this order
static const char* const program_dirs[] = { "sys", "bin" };

char* os_programpath(const char* name, char* out, size_t size) {
    char home[SYS_PATH_MAX];
    if (os_gethome(home, sizeof(home)) == NULL)
        return NULL;

    while (name != NULL && *name == '/')
        name++;
    if (name == NULL || *name == '\0') {
        errno = ENOENT;
        return NULL;
    }

    int err = ENOENT;
    for (size_t i = 0; i < sizeof(program_dirs) / sizeof(program_dirs[0]); i++) {
        char path[SYS_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s/%s", home, program_dirs[i], name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            err = ENAMETOOLONG;
            continue;
        }
        struct stat st;
        if (stat(path, &st) < 0 || S_ISDIR(st.st_mode))
            continue;
        if ((size_t)n >= size) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        memcpy(out, path, (size_t)n + 1);
        return out;
    }
    errno = err;
    return NULL;
}



