

#ifndef _KERNEL_SYS_FILES_H_
#define _KERNEL_SYS_FILES_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/vfs/vfs.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/arch/x86/drivers/serial.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/process.h>
#include <kernel/malloc.h>
#include <kernel/mutex.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>

//
// Per-process file state: descriptor table and working directory
//
// One SysProcessFiles per process, kept on a list keyed by process id (the
// process structure itself knows nothing about files). process_build()
// creates it through syscall_process_start() before the process's first
// thread exists, with the working directory the launcher chose. A process
// started without that hook gets one on its first file call, in "/".
// syscall_process_exit() drops it.
//
// Locking:
//   - the list and each table's `users` count: interrupts off (irq_save)
//   - everything else in a table: its mutex, held for a whole file call,
//     so the threads of one process make file calls one at a time
//   - order: table mutex -> VFS lock. Never the other way round.
//
// The VFS knows nothing about descriptors, offsets or relative paths: each
// SysFile is one VFS handle plus the position and the absolute path it was
// opened by (needed to resize the file, see sys_file_grow in sys_file.h).
//

// How long teardown waits for other threads of the exiting process to
// leave a file call. After that the table is leaked, never freed under them.
#define SYS_FILES_EXIT_WAIT_MS  1000

typedef struct {
    char*    path;      // Absolute path (malloc'd). NULL = descriptor free
    File     file;      // VFS handle; VFS_INVALID_FILE if a reopen failed
    uint32_t offset;    // Position, kept here (vfs_tell does not follow device files)
    uint32_t flags;     // SYS_O_* as opened
} SysFile;

typedef struct SysProcessFiles {
    uint32_t      process_id;
    AddressSpace* space;        // Owner's address space: spots a reused process id
    uint32_t      users;        // Calls in progress (interrupts off to touch)
    mutex_t       lock;
    SysFile       files[SYS_FD_MAX];
    char          cwd[SYS_PATH_MAX];
    struct SysProcessFiles* next;
} SysProcessFiles;

static SysProcessFiles* sys_files_list = NULL;

// Interrupts off
static SysProcessFiles* sys_files_find(uint32_t process_id) {
    for (SysProcessFiles* t = sys_files_list; t != NULL; t = t->next) {
        if (t->process_id == process_id)
            return t;
    }
    return NULL;
}

// Close every descriptor. Caller holds the table mutex, or no other thread
// can reach the table any more.
static void sys_files_close_all(SysProcessFiles* t) {
    for (uint32_t i = 0; i < SYS_FD_MAX; i++) {
        SysFile* f = &t->files[i];
        if (f->path == NULL)
            continue;
        if (f->file != VFS_INVALID_FILE)
            vfs_close(f->file);
        free(f->path);
        memset(f, 0, sizeof(SysFile));
    }
}

// A fresh, unlinked table for `process_id` in directory `cwd` (absolute,
// normalized, fits SYS_PATH_MAX). NULL when out of memory.
static SysProcessFiles* sys_files_new(uint32_t process_id, AddressSpace* space, const char* cwd) {
    SysProcessFiles* t = (SysProcessFiles*)malloc(sizeof(SysProcessFiles));
    if (t == NULL)
        return NULL;
    memset(t, 0, sizeof(SysProcessFiles));
    t->process_id = process_id;
    t->space      = space;
    memcpy(t->cwd, cwd, strlen(cwd) + 1);
    mutex_init(&t->lock);
    return t;
}

// The calling process's table, created if needed, returned LOCKED.
// NULL when out of memory.
static SysProcessFiles* sys_files_acquire(void) {
    AddressSpace* space = process_current_space();
    if (space == NULL)
        return NULL;
    uint32_t process_id = (uint32_t)process_current_id();

    // Normally process_build() made the table already (syscall_process_start)
    uint32_t irq_flags = irq_save();
    SysProcessFiles* t = sys_files_find(process_id);
    if (t == NULL) {
        t = sys_files_new(process_id, space, "/");
        if (t != NULL) {
            t->next = sys_files_list;
            sys_files_list = t;
        }
    }
    if (t != NULL)
        t->users++;
    irq_restore(irq_flags);

    if (t == NULL)
        return NULL;

    mutex_lock(&t->lock);

    // Left behind by an earlier process with the same id that ended without
    // syscall_process_exit(): its files are not ours
    if (t->space != space) {
        sys_files_close_all(t);
        memset(t->cwd, 0, sizeof(t->cwd));
        t->cwd[0] = '/';
        t->space  = space;
    }
    return t;
}

static void sys_files_release(SysProcessFiles* t) {
    mutex_unlock(&t->lock);

    uint32_t irq_flags = irq_save();
    t->users--;
    irq_restore(irq_flags);
}

typedef int32_t (*SysFilesFn)(SysProcessFiles* t, const SyscallArgs* args);

// Run one file call with the caller's table locked
static int32_t sys_files_call(SysFilesFn fn, const SyscallArgs* args) {
    SysProcessFiles* t = sys_files_acquire();
    if (t == NULL)
        return SYS_ENOMEM;
    int32_t result = fn(t, args);
    sys_files_release(t);
    return result;
}

// Wait for the calls still inside an unlinked table to leave, then close
// its files and free it. Thread context, interrupts on.
static void sys_files_destroy(SysProcessFiles* t) {
    uint64_t start = timer_get_ms();
    while (1) {
        uint32_t irq_flags = irq_save();
        uint32_t users = t->users;
        irq_restore(irq_flags);
        if (users == 0)
            break;

        if (timer_get_ms() - start >= SYS_FILES_EXIT_WAIT_MS) {
            serial_puts("syscall: process ");
            serial_put_dec(t->process_id);
            serial_puts(" ended inside a file call; its descriptor table is leaked\n");
            return;
        }
        thread_sleep(1);
    }

    sys_files_close_all(t);
    free(t);
}

static bool sys_files_process_start(uint32_t process_id, AddressSpace* space, const char* cwd) {
    // Settle the directory first: absolute, normalized, and a directory
    // that exists right now. Anything else starts the process in "/".
    char path[SYS_PATH_MAX];
    bool usable = false;

    if (cwd != NULL && cwd[0] == '/' && vfs_path_normalize("/", cwd, path, sizeof(path)))
        usable = (path[1] == '\0') || vfs_directory_check(path);

    if (!usable) {
        if (cwd != NULL) {
            serial_puts("syscall: process ");
            serial_put_dec(process_id);
            serial_puts(": working directory \"");
            serial_puts(cwd);
            serial_puts("\" is not usable, starting in /\n");
        }
        path[0] = '/';
        path[1] = '\0';
    }

    SysProcessFiles* t = sys_files_new(process_id, space, path);
    if (t == NULL)
        return false;

    // Swap in the new table. Process ids carry a generation, so an old table
    // under this id should not exist; drop one if it somehow does.
    SysProcessFiles* stale = NULL;
    uint32_t irq_flags = irq_save();
    for (SysProcessFiles** link = &sys_files_list; *link != NULL; link = &(*link)->next) {
        if ((*link)->process_id == process_id) {
            stale = *link;
            *link = stale->next;
            break;
        }
    }
    t->next = sys_files_list;
    sys_files_list = t;
    irq_restore(irq_flags);

    if (stale != NULL)
        sys_files_destroy(stale);
    return true;
}

static void sys_files_process_exit(uint32_t process_id) {
    uint32_t irq_flags = irq_save();
    SysProcessFiles* t = NULL;
    for (SysProcessFiles** link = &sys_files_list; *link != NULL; link = &(*link)->next) {
        if ((*link)->process_id == process_id) {
            t = *link;
            *link = t->next;
            break;
        }
    }
    irq_restore(irq_flags);

    // Unlinked: no new call can find it. Wait for the ones already inside.
    if (t != NULL)
        sys_files_destroy(t);
}

//
// Descriptors (caller holds the table mutex)
//

static int32_t sys_fd_alloc(SysProcessFiles* t) {
    for (uint32_t fd = SYS_FD_FIRST; fd < SYS_FD_MAX; fd++) {
        if (t->files[fd].path == NULL)
            return (int32_t)fd;
    }
    return SYS_EMFILE;
}

// The open descriptor `fd`, or NULL
static SysFile* sys_fd_get(SysProcessFiles* t, uint32_t fd) {
    if (fd < SYS_FD_FIRST || fd >= SYS_FD_MAX)
        return NULL;
    SysFile* f = &t->files[fd];
    return (f->path != NULL) ? f : NULL;
}

static uint16_t sys_vfs_open_flags(uint32_t flags) {
    uint16_t vfs_flags = 0;
    if (flags & SYS_O_READ)  vfs_flags |= VFS_OPEN_READ;
    if (flags & SYS_O_WRITE) vfs_flags |= VFS_OPEN_WRITE;
    return vfs_flags;
}

// True when `path` is `base` or lies below it. Both absolute and normalized.
static bool sys_path_within(const char* path, const char* base) {
    size_t length = strlen(base);
    if (length == 1)        // "/"
        return true;
    if (strncmp(path, base, length) != 0)
        return false;
    return path[length] == '\0' || path[length] == '/';
}

// True if this process has `path`, or anything below it, open
static bool sys_files_path_open(SysProcessFiles* t, const char* path) {
    for (uint32_t i = SYS_FD_FIRST; i < SYS_FD_MAX; i++) {
        if (t->files[i].path != NULL && sys_path_within(t->files[i].path, path))
            return true;
    }
    return false;
}

// After a rename: re-point this process's descriptors at the new path. One
// left stale (no memory) only loses the ability to grow its file.
static void sys_files_rename_paths(SysProcessFiles* t, const char* old_path, const char* new_path) {
    size_t old_length = strlen(old_path);
    size_t new_length = strlen(new_path);

    for (uint32_t i = SYS_FD_FIRST; i < SYS_FD_MAX; i++) {
        SysFile* f = &t->files[i];
        if (f->path == NULL || !sys_path_within(f->path, old_path))
            continue;

        size_t tail = strlen(f->path) - old_length;
        char* path = (char*)malloc(new_length + tail + 1);
        if (path == NULL)
            continue;
        memcpy(path, new_path, new_length);
        memcpy(path + new_length, f->path + old_length, tail + 1);

        free(f->path);
        f->path = path;
    }
}

#endif
