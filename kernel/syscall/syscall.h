#ifndef _KERNEL_SYSCALL_H_
#define _KERNEL_SYSCALL_H_

#include <stdint.h>

// System call ABI (i386, int 0x81)
//
//   EAX                  syscall number
//   EBX ECX EDX ESI EDI  arguments 1-5
//   EAX                  result
//
// Every other register, and the FPU/SSE state, is preserved.
//
// Results: an error is a small negative number, SYS_E* below, always in
// [-4095, -1] (0xFFFFF001 - 0xFFFFFFFF unsigned). Anything else is success.
// Calls that return an address (sbrk) can return values above 0x7FFFFFFF,
// so test for errors with an unsigned compare: result >= (uint32_t)-4095.
//
// Why 0x81 and not 0x80: vector 0x80 is the scheduler's internal yield
// (thread_yield, int $0x80). Its gate is DPL 0, so ring 3 cannot reach it;
// a user `int 0x80` raises #GP and kills the process. The syscall gate is a
// DPL 3 trap gate, so interrupts stay enabled while a syscall runs.
//
// This file is meant to be shared with user programs: the numbers, error
// codes, flags and structures below are the ABI. Each call is implemented
// in syscall/calls/; SYSCALLS.md describes every call in detail.

#define SYSCALL_VECTOR      0x81

//      Number              Arguments                   Result
#define SYS_EXIT        0   // (int code)               never returns
#define SYS_PRINT       1   // (buf, length)            bytes printed (console + COM1)
#define SYS_YIELD       2   // ()                       0
#define SYS_SLEEP       3   // (ms)                     0
#define SYS_GETPID      4   // ()                       process id
#define SYS_SBRK        5   // (int increment)          previous heap break (address)
#define SYS_OPEN        6   // (path, flags)            file descriptor
#define SYS_CLOSE       7   // (fd)                     0
#define SYS_READ        8   // (fd, buf, length)        bytes read, 0 at end of file
#define SYS_WRITE       9   // (fd, buf, length)        bytes written
#define SYS_SEEK       10   // (fd, int offset, whence) new offset
#define SYS_FSTAT      11   // (fd, SysStat*)           0
#define SYS_STAT       12   // (path, SysStat*)         0
#define SYS_MKDIR      13   // (path)                   0
#define SYS_REMOVE     14   // (path)                   0
#define SYS_RENAME     15   // (path, new_name)         0
#define SYS_TRUNCATE   16   // (path, size)             0
#define SYS_READDIR    17   // (path, index, buf, size) name length, 0 past the last entry
#define SYS_CHDIR      18   // (path)                   0
#define SYS_GETCWD     19   // (buf, size)              path length
#define SYS_COPY       20   // (src, dest)              0
#define SYS_COUNT      21

#define SYS_OK               0
#define SYS_ENOSYS          -1  // Unknown syscall number
#define SYS_EFAULT          -2  // Bad user pointer (unmapped, kernel, or not writable)
#define SYS_EINVAL          -3  // Bad argument
#define SYS_ENOMEM          -4  // Out of memory or over the process's limit
#define SYS_ENOENT          -5  // No such file or directory (or a missing parent)
#define SYS_EEXIST          -6  // Already exists
#define SYS_EACCES          -7  // Permission denied (or refused by the file system)
#define SYS_EBADF           -8  // Not an open descriptor, or not opened for this
#define SYS_EMFILE          -9  // Too many open files in this process
#define SYS_EISDIR         -10  // Is a directory
#define SYS_ENOTDIR        -11  // Not a directory
#define SYS_ENOTEMPTY      -12  // Directory not empty
#define SYS_EBUSY          -13  // Open, or a mounted device
#define SYS_ENAMETOOLONG   -14  // Path or name too long
#define SYS_ERANGE         -15  // Caller's buffer too small
#define SYS_ENOSPC         -16  // No space (or the file cannot grow)
#define SYS_EIO            -17  // The device or file system failed

#define SYS_ERROR_MIN       -4095

#define SYS_PRINT_MAX       4096    // Longer prints are truncated (the return value says how much went out)

//
// Files
//
// Paths are NUL-terminated, at most SYS_PATH_MAX bytes including the
// terminator. A path not starting with '/' is relative to the process's
// working directory (SYS_CHDIR; "/" when the process starts). "." and ".."
// are folded as text before the VFS sees the path.
//
// Descriptors are private to the process. 0, 1 and 2 are reserved: writing
// to 1 or 2 prints like SYS_PRINT, anything else on them is SYS_EBADF.
// SYS_OPEN hands out 3 and up. All are closed when the process exits.
//

#define SYS_PATH_MAX        512     // Bytes, terminator included (= VFS_PATH_MAX)
#define SYS_NAME_MAX        255     // One name, terminator excluded (= VFS_NAME_MAX)
#define SYS_FD_MAX          32      // Descriptor numbers 0 .. SYS_FD_MAX - 1
#define SYS_FD_FIRST        3       // First descriptor SYS_OPEN returns
#define SYS_RW_MAX          (1024U * 1024U)   // Longer reads/writes are cut short
#define SYS_OFFSET_MAX      0x7FFFFFFFU       // Largest file position

#define SYS_FD_STDIN        0
#define SYS_FD_STDOUT       1
#define SYS_FD_STDERR       2

// SYS_OPEN flags. At least one of READ / WRITE.
#define SYS_O_READ          0x01
#define SYS_O_WRITE         0x02
#define SYS_O_CREATE        0x04    // Create the file if it does not exist
#define SYS_O_TRUNC         0x08    // Empty an existing file (needs WRITE)
#define SYS_O_APPEND        0x10    // Every write goes to the end (needs WRITE)
#define SYS_O_EXCL          0x20    // With CREATE: fail with SYS_EEXIST if it exists

// SYS_SEEK whence
#define SYS_SEEK_SET        0
#define SYS_SEEK_CUR        1
#define SYS_SEEK_END        2

// SysStat.type
#define SYS_TYPE_FILE       0
#define SYS_TYPE_DIRECTORY  1
#define SYS_TYPE_MOUNT      2       // A device's root (or a mount that is unformatted)

// SysStat.permissions
#define SYS_PERM_EXECUTE    0x01
#define SYS_PERM_READ       0x02
#define SYS_PERM_WRITE      0x04

// Filled by SYS_STAT / SYS_FSTAT. Fixed-width fields only, 20 bytes.
typedef struct {
    uint32_t size;          // Bytes (files only, 0 for directories)
    uint32_t type;          // SYS_TYPE_*
    uint32_t permissions;   // SYS_PERM_*
    uint32_t created;       // Seconds since the Unix epoch, 0 = unknown
    uint32_t modified;
} SysStat;

// Kernel side: set up the serial output and locks the syscalls use. The IDT
// gate is installed by idt_init().
void syscall_init(void);

// Kernel side: close every file a process has open and drop its descriptor
// table. DestroyProcess() calls it before freeing the address space, so
// every way a process ends (SYS_EXIT, a fatal fault, a kill) is covered;
// nothing else needs to. Thread context with interrupts on: it may sleep.
void syscall_process_exit(uint32_t process_id);

#endif

