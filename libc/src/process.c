/*
 * Starting programs (os/process.h) on SYS_SPAWN.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <os/os.h>
#include <os/process.h>
#include "internal.h"

pid_t spawn_ex(const char* path, char* const argv[], const char* cwd, sec_mask_t security) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    return __sys_ret(syscall4(SYS_SPAWN, (uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)argv,
                              (uint32_t)(uintptr_t)cwd, (uint32_t)security));
}

pid_t spawn_tty(const char* path, char* const argv[], const char* cwd, sec_mask_t security, int tty) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    return __sys_ret(syscall5(SYS_SPAWN_TTY, (uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)argv,
                              (uint32_t)(uintptr_t)cwd, (uint32_t)security, (uint32_t)tty));
}

int tty_resize(int pipe, unsigned columns, unsigned rows) {
    return __sys_ret(syscall3(SYS_TTY_RESIZE, (uint32_t)pipe, columns, rows));
}

pid_t spawn(const char* path, char* const argv[]) {
    return spawn_ex(path, argv, NULL, SPAWN_INHERIT);
}

pid_t spawnl(const char* path, const char* arg0, ...) {
    const char* args[SPAWN_ARG_MAX + 1];
    size_t count = 0;

    va_list ap;
    va_start(ap, arg0);
    for (const char* a = arg0; a != NULL; a = va_arg(ap, const char*)) {
        if (count == SPAWN_ARG_MAX) {
            va_end(ap);
            errno = E2BIG;
            return -1;
        }
        args[count++] = a;
    }
    va_end(ap);
    args[count] = NULL;

    return spawn(path, (char* const*)args);
}

//
// Program kinds
//

// The ELF32 header and section header fields needed here
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header_t;

typedef struct {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
} elf_section_t;

#define KIND_SECTION    ".os.kind"
#define KIND_MAX        4096        // Bytes of marks read (a few per object file)

static bool read_at(int fd, uint32_t offset, void* buf, size_t size) {
    if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset)
        return false;
    uint8_t* p = (uint8_t*)buf;
    while (size > 0) {
        ssize_t n = read(fd, p, size);
        if (n <= 0)
            return false;
        p += n;
        size -= (size_t)n;
    }
    return true;
}

// Read section `index`'s header
static bool read_section(int fd, const elf_header_t* h, uint32_t index, elf_section_t* out) {
    return read_at(fd, h->shoff + index * (uint32_t)h->shentsize, out, sizeof(*out));
}

static int kind_of(int fd) {
    elf_header_t h;
    if (!read_at(fd, 0, &h, sizeof(h)) || memcmp(h.ident, "\x7f" "ELF", 4) != 0 ||
        h.ident[4] != 1 || h.shentsize < sizeof(elf_section_t) || h.shstrndx >= h.shnum) {
        errno = ENOEXEC;
        return -1;
    }

    elf_section_t names;
    if (!read_section(fd, &h, h.shstrndx, &names)) {
        errno = ENOEXEC;
        return -1;
    }

    for (uint32_t i = 1; i < h.shnum; i++) {
        elf_section_t sec;
        char name[sizeof(KIND_SECTION)];
        if (!read_section(fd, &h, i, &sec) || sec.name >= names.size ||
            !read_at(fd, names.offset + sec.name, name, sizeof(name)) ||
            memcmp(name, KIND_SECTION, sizeof(name)) != 0)
            continue;

        // The marks, one string per object file that has one
        uint32_t size = sec.size < KIND_MAX ? sec.size : KIND_MAX;
        char* marks = (char*)malloc(size + 1);
        if (marks == NULL)
            return -1;
        if (!read_at(fd, sec.offset, marks, size)) {
            free(marks);
            break;
        }
        marks[size] = '\0';
        int kind = PROGRAM_CONSOLE;
        for (uint32_t at = 0; at < size; at += (uint32_t)strlen(marks + at) + 1) {
            if (strcmp(marks + at, "console") == 0) {
                kind = PROGRAM_CONSOLE;
                break;
            }
            if (strcmp(marks + at, "windowed") == 0)
                kind = PROGRAM_WINDOWED;
        }
        free(marks);
        return kind;
    }
    return PROGRAM_CONSOLE;     // No marks
}

int program_kind(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        // No clearance for it: the kernel tells the kind of an executable
        int err = errno;
        os_finfo_t info;
        if (err == EACCES && os_finfo(path, &info) == 0) {
            if (info.flags & SYS_FINFO_WINDOWED)
                return PROGRAM_WINDOWED;
            if (info.flags & SYS_FINFO_CONSOLE)
                return PROGRAM_CONSOLE;
        }
        errno = err;
        return -1;
    }
    int kind = kind_of(fd);
    int err = errno;
    close(fd);
    errno = err;
    return kind;
}

int program_needs_elevation(const char* path, sec_mask_t security) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    os_finfo_t info;
    if (os_finfo(path, &info) < 0)
        return -1;
    if (security == SPAWN_INHERIT)
        security = sec_get();
    security &= ~(sec_mask_t)SPAWN_ELEVATE;

    sec_mask_t missing = info.security & ~security;
    if (missing == 0)
        return 0;       // The child has all it needs already
    return sec_has(missing) || sec_has(SEC_ELEVATE);
}

pid_t spawn_program(const char* path, char* const argv[], const char* cwd, sec_mask_t security) {
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    // Unreadable: spawn_ex says why
    if (program_kind(path) != PROGRAM_CONSOLE)
        return spawn_ex(path, argv, cwd, security);

    // The console window asks for admin itself, for the program
    if (security != SPAWN_INHERIT)
        security &= ~(sec_mask_t)SPAWN_ELEVATE;

    // No console host (no home disk, or no <home>/bin/console): start it
    // bare, the way programs were started before there was one
    char console[SYS_PATH_MAX];
    if (os_programpath("console", console, sizeof(console)) == NULL)
        return spawn_ex(path, argv, cwd, security);

    // console --run <path> <argv[1]> ...
    size_t argc = 0;
    while (argv != NULL && argv[argc] != NULL)
        argc++;
    size_t extra = argc > 0 ? argc - 1 : 0;
    if (3 + extra > SPAWN_ARG_MAX) {
        errno = E2BIG;
        return -1;
    }
    char* args[SPAWN_ARG_MAX + 1];
    args[0] = console;
    args[1] = (char*)"--run";
    args[2] = (char*)path;
    for (size_t i = 0; i < extra; i++)
        args[3 + i] = argv[1 + i];
    args[3 + extra] = NULL;
    return spawn_ex(console, args, cwd, security);
}
