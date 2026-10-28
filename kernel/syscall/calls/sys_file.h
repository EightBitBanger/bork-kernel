
#ifndef _KERNEL_SYS_FILE_H_
#define _KERNEL_SYS_FILE_H_

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_procfile.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/syscall/calls/sys_print.h>
#include <kernel/syscall/calls/sys_security.h>
#include <kernel/vfs/vfs.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/scheduler/process.h>
#include <kernel/malloc.h>
#include <kernel/util/string.h>

//
// Descriptor calls: open, close, read, write, seek, fstat
//
// Data moves through a kernel bounce buffer, SYS_IO_CHUNK bytes at a time.
// The user buffer is checked once up front; if another thread unmaps part
// of it mid-call, the copy stops there and the call returns what it moved.
//

#define SYS_IO_CHUNK  4096

#define SYS_O_ALL     (SYS_O_READ | SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC | SYS_O_APPEND | SYS_O_EXCL)

// The VFS works in 64-bit sizes and positions; the syscall ABI stops at
// SYS_OFFSET_MAX. A larger file looks SYS_OFFSET_MAX long from user space.
static inline uint32_t sys_clamp_offset(uint64_t value) {
    return (value > SYS_OFFSET_MAX) ? SYS_OFFSET_MAX : (uint32_t)value;
}

// Resolve, create and open `path`. Caller holds the VFS lock, so the
// existence checks and the open are one step.
static int32_t sys_open_path(char* path, uint32_t flags, File* out) {
    uint16_t vfs_flags = sys_vfs_open_flags(flags);
    if (flags & SYS_O_TRUNC)
        vfs_flags |= VFS_OPEN_TRUNCATE;     // The VFS empties it as it opens
    bool exists = vfs_exists(path);

    if (exists) {
        if (flags & SYS_O_EXCL)
            return SYS_EEXIST;
        if (vfs_directory_check(path))
            return SYS_EISDIR;

        // vfs_open would quietly fall back to read-only; say no instead
        uint8_t perm = 0;
        if (!vfs_get_permissions(path, &perm))
            return SYS_EIO;
        if ((flags & SYS_O_READ) && !(perm & VFS_PERMISSION_READ))
            return SYS_EACCES;
        if ((flags & SYS_O_WRITE) && !(perm & VFS_PERMISSION_WRITE))
            return SYS_EACCES;
        if (!vfs_check_access(path))
            return SYS_EACCES;      // Needs clearance this process lacks
    } else {
        if (!(flags & SYS_O_CREATE))
            return SYS_ENOENT;
        if (!sys_parent_is_directory(path))
            return SYS_ENOENT;
        if (!vfs_check_parent_access(path))
            return SYS_EACCES;
        vfs_flags |= VFS_OPEN_CREATE;
    }

    File file = vfs_open(path, vfs_flags);
    if (file == VFS_INVALID_FILE) {
        // A refused create: kernel folder, read-only directory, bad name, full
        // device. An existing file: TRUNC/APPEND on a kernel node (fixed
        // size), or the truncate itself failed.
        return exists ? SYS_EIO : SYS_EACCES;
    }

    *out = file;
    return SYS_OK;
}

// vfs_write does not reliably extend a file past the space it owns, so a
// write past the end sizes the file first (the vfs_copy pattern: close,
// resize, reopen). The VFS lock keeps everyone else from seeing the file
// closed in between. Returns false if the resize failed (old size kept).
// If the reopen fails, f->file is VFS_INVALID_FILE and the descriptor
// reports SYS_EIO until it is closed.
static bool sys_file_grow(SysFile* f, uint32_t new_size) {
    vfs_lock();
    // Never shrink: a file past SYS_OFFSET_MAX only looks that short here
    if (vfs_get_size(f->file) >= new_size) {
        vfs_unlock();
        return true;
    }
    vfs_close(f->file);
    bool grown = vfs_truncate(f->path, new_size);
    f->file = vfs_open(f->path, sys_vfs_open_flags(f->flags));
    vfs_unlock();
    return grown;
}

//
// open(path, flags) -> descriptor
//

static int32_t sys_open_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t flags = args->arg2;

    if ((flags & ~SYS_O_ALL) != 0 || !(flags & (SYS_O_READ | SYS_O_WRITE)))
        return SYS_EINVAL;
    if ((flags & (SYS_O_TRUNC | SYS_O_APPEND)) && !(flags & SYS_O_WRITE))
        return SYS_EINVAL;
    if ((flags & SYS_O_EXCL) && !(flags & SYS_O_CREATE))
        return SYS_EINVAL;

    // Reading needs FILE_READ; writing, creating, truncating or appending
    // needs FILE_WRITE (TRUNC/APPEND already imply WRITE)
    uint32_t needs = 0;
    if (flags & SYS_O_READ)
        needs |= SYS_SEC_FILE_READ;
    if (flags & (SYS_O_WRITE | SYS_O_CREATE))
        needs |= SYS_SEC_FILE_WRITE;
    if (!sys_security_allows(needs))
        return SYS_EPERM;

    char path[SYS_PATH_MAX];
    int32_t result = sys_user_path(process_current_space(), t->cwd, args->arg1, path);
    if (result < 0)
        return result;

    int32_t fd = sys_fd_alloc(t);
    if (fd < 0)
        return fd;

    size_t length = strlen(path);
    char* saved = (char*)malloc(length + 1);
    if (saved == NULL)
        return SYS_ENOMEM;
    memcpy(saved, path, length + 1);

    File file = VFS_INVALID_FILE;
    vfs_lock();
    result = sys_open_path(path, flags, &file);
    vfs_unlock();

    if (result < 0) {
        free(saved);
        return result;
    }

    SysFile* f = &t->files[fd];
    f->path   = saved;
    f->file   = file;
    f->flags  = flags;
    f->offset = (flags & SYS_O_APPEND) ? sys_clamp_offset(vfs_get_size(file)) : 0;
    return fd;
}

static int32_t sys_open(const SyscallArgs* args) {
    return sys_files_call(sys_open_locked, args);
}

//
// close(fd)
//

static int32_t sys_close_locked(SysProcessFiles* t, const SyscallArgs* args) {
    SysFile* f = sys_fd_get(t, args->arg1);
    if (f == NULL)
        return SYS_EBADF;

    if (f->file != VFS_INVALID_FILE)
        vfs_close(f->file);
    free(f->path);
    memset(f, 0, sizeof(SysFile));
    return SYS_OK;
}

static int32_t sys_close(const SyscallArgs* args) {
    return sys_files_call(sys_close_locked, args);
}

//
// read(fd, buf, length) -> bytes read, 0 at end of file
//

static int32_t sys_read_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t user_buffer = args->arg2;
    uint32_t length      = args->arg3;

    SysFile* f = sys_fd_get(t, args->arg1);
    if (f == NULL || !(f->flags & SYS_O_READ))
        return SYS_EBADF;
    if (f->file == VFS_INVALID_FILE)
        return SYS_EIO;
    if (length == 0)
        return 0;
    if (length > SYS_RW_MAX)
        length = SYS_RW_MAX;

    AddressSpace* space = process_current_space();
    if (!vmm_user_check(space, user_buffer, length, true))
        return SYS_EFAULT;

    // Clamp here rather than trust each device's end-of-file behaviour
    uint32_t size = sys_clamp_offset(vfs_get_size(f->file));
    if (f->offset >= size)
        return 0;
    if (length > size - f->offset)
        length = size - f->offset;

    uint8_t* bounce = (uint8_t*)malloc(SYS_IO_CHUNK);
    if (bounce == NULL)
        return SYS_ENOMEM;

    vfs_seek(f->file, f->offset);

    uint32_t done  = 0;
    int32_t  error = SYS_OK;
    while (done < length) {
        uint32_t want = length - done;
        if (want > SYS_IO_CHUNK)
            want = SYS_IO_CHUNK;

        int32_t got = vfs_read(f->file, bounce, want);
        if (got <= 0) {
            if (got < 0) error = SYS_EIO;
            break;
        }
        if ((uint32_t)got > want)
            got = (int32_t)want;

        // Not delivered, so not consumed: the next call seeks back to offset
        if (!vmm_user_copy_to(space, user_buffer + done, bounce, (uint32_t)got)) {
            error = SYS_EFAULT;
            break;
        }

        done      += (uint32_t)got;
        f->offset += (uint32_t)got;
        if ((uint32_t)got < want)
            break;
    }

    free(bounce);
    return (done > 0) ? (int32_t)done : error;
}

static int32_t sys_read(const SyscallArgs* args) {
    if (args->arg1 < SYS_FD_FIRST) {
        // The terminal, if any (0 only). Not under the table's lock: it
        // blocks. Free, like writing to it: the host handed it over.
        if (args->arg1 != SYS_FD_STDIN)
            return SYS_EBADF;
        IpcCaller c = {
            .pid      = (uint32_t)process_current_id(),
            .space    = process_current_space(),
            .security = process_current_security(),
        };
        if (c.space == NULL)
            return SYS_EFAULT;
        return ipc_stdio_read(&c, args->arg2, args->arg3);
    }
    // Called with no requirement in the table: descriptor 0 needs none
    if (!sys_security_allows(SYS_SEC_FILE_READ))
        return SYS_EPERM;
    return sys_files_call(sys_read_locked, args);
}

//
// write(fd, buf, length) -> bytes written
//

static int32_t sys_write_locked(SysProcessFiles* t, const SyscallArgs* args) {
    uint32_t user_buffer = args->arg2;
    uint32_t length      = args->arg3;

    SysFile* f = sys_fd_get(t, args->arg1);
    if (f == NULL || !(f->flags & SYS_O_WRITE))
        return SYS_EBADF;
    if (f->file == VFS_INVALID_FILE)
        return SYS_EIO;
    if (length == 0)
        return 0;
    if (length > SYS_RW_MAX)
        length = SYS_RW_MAX;

    AddressSpace* space = process_current_space();
    if (!vmm_user_check(space, user_buffer, length, false))
        return SYS_EFAULT;

    uint32_t size = sys_clamp_offset(vfs_get_size(f->file));
    bool append = (f->flags & SYS_O_APPEND) != 0;

    if (append) {
        // The handle was opened VFS_OPEN_APPEND: every vfs_write lands at
        // the end of the file and grows it there, so nothing is sized up
        // front (a pre-grow would move "the end" past the new zeros)
        f->offset = size;
        if (size >= SYS_OFFSET_MAX)
            return SYS_ENOSPC;
        if (length > SYS_OFFSET_MAX - size)
            length = SYS_OFFSET_MAX - size;
    } else {
        // offset <= SYS_OFFSET_MAX and length <= SYS_RW_MAX: cannot wrap
        uint32_t end = f->offset + length;
        if (end > size) {
            sys_file_grow(f, end);      // On failure the write stops at the old end
            if (f->file == VFS_INVALID_FILE)
                return SYS_EIO;
            size = sys_clamp_offset(vfs_get_size(f->file));
        }

        if (f->offset >= size)
            return SYS_ENOSPC;
        if (length > size - f->offset)
            length = size - f->offset;
    }

    uint8_t* bounce = (uint8_t*)malloc(SYS_IO_CHUNK);
    if (bounce == NULL)
        return SYS_ENOMEM;

    vfs_seek(f->file, f->offset);

    uint32_t done  = 0;
    int32_t  error = SYS_OK;
    while (done < length) {
        uint32_t want = length - done;
        if (want > SYS_IO_CHUNK)
            want = SYS_IO_CHUNK;

        if (!vmm_user_copy_from(space, bounce, user_buffer + done, want)) {
            error = SYS_EFAULT;
            break;
        }

        int32_t wrote = vfs_write(f->file, bounce, want);
        if (wrote <= 0) {
            if (wrote < 0) error = SYS_EIO;
            else if (append) error = SYS_ENOSPC;    // The file could not grow
            break;
        }

        done      += (uint32_t)wrote;
        f->offset += (uint32_t)wrote;
        if ((uint32_t)wrote < want)
            break;
    }

    free(bounce);

    // Append: other descriptors on the file may have written in between,
    // so the position is wherever this handle's last write ended
    if (append && done > 0)
        f->offset = sys_clamp_offset(vfs_tell(f->file));
    return (done > 0) ? (int32_t)done : error;
}

static int32_t sys_write(const SyscallArgs* args) {
    uint32_t fd = args->arg1;

    if (fd == SYS_FD_STDOUT || fd == SYS_FD_STDERR) {
        // The terminal, or the console (sys_print checks SYS_SEC_CONSOLE for that)
        SyscallArgs print_args = { .arg1 = args->arg2, .arg2 = args->arg3 };
        return sys_print(&print_args);
    }
    if (fd < SYS_FD_FIRST)
        return SYS_EBADF;
    if (!sys_security_allows(SYS_SEC_FILE_WRITE))
        return SYS_EPERM;
    return sys_files_call(sys_write_locked, args);
}

//
// seek(fd, offset, whence) -> new position
//

static int32_t sys_seek_locked(SysProcessFiles* t, const SyscallArgs* args) {
    SysFile* f = sys_fd_get(t, args->arg1);
    if (f == NULL)
        return SYS_EBADF;

    int64_t base;
    switch (args->arg3) {
        case SYS_SEEK_SET: base = 0;                 break;
        case SYS_SEEK_CUR: base = (int64_t)f->offset; break;
        case SYS_SEEK_END:
            if (f->file == VFS_INVALID_FILE)
                return SYS_EIO;
            base = (int64_t)vfs_get_size(f->file);
            break;
        default:
            return SYS_EINVAL;
    }

    // Past the end is allowed: reads there return 0, a write grows the file
    int64_t position = base + (int64_t)(int32_t)args->arg2;
    if (position < 0 || position > (int64_t)SYS_OFFSET_MAX)
        return SYS_EINVAL;

    f->offset = (uint32_t)position;
    return (int32_t)position;
}

static int32_t sys_seek(const SyscallArgs* args) {
    return sys_files_call(sys_seek_locked, args);
}

//
// fstat(fd, SysStat*)
//

static int32_t sys_fstat_locked(SysProcessFiles* t, const SyscallArgs* args) {
    SysFile* f = sys_fd_get(t, args->arg1);
    if (f == NULL)
        return SYS_EBADF;

    FSFileStats st;
    if (!vfs_stat(f->path, &st)) {
        // Renamed by another process: only the handle is left to ask
        memset(&st, 0, sizeof(st));
        st.type = VFS_TYPE_FILE;
    }
    if (f->file != VFS_INVALID_FILE)
        st.size = vfs_get_size(f->file);

    return sys_stat_to_user(process_current_space(), args->arg2, &st);
}

static int32_t sys_fstat(const SyscallArgs* args) {
    return sys_files_call(sys_fstat_locked, args);
}

#endif



