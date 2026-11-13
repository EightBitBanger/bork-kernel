
#ifndef _OS_SYSCALL_H
#define _OS_SYSCALL_H

/*
 * Kernel system call ABI, user side.
 *
 * Mirrors kernel/syscall/syscall.h: numbers, error codes, flags and SysStat
 * must stay identical to the kernel's. Everything else in the libc is built
 * on the syscallN() stubs at the bottom of this file.
 *
 *   int 0x81
 *   EAX                  syscall number
 *   EBX ECX EDX ESI EDI  arguments 1-5
 *   EAX                  result: [-4095, -1] is an error (SYS_E*), anything
 *                        else is success. Addresses (sbrk) can exceed
 *                        0x7FFFFFFF, so test errors with SYS_IS_ERROR().
 *
 * The kernel preserves every register except EAX (and the FPU/SSE state).
 */

#include <stdint.h>

#define SYSCALL_VECTOR      0x81

#define SYS_EXIT        0   // (int code)               never returns
#define SYS_PRINT       1   // (buf, length)            bytes printed
#define SYS_YIELD       2   // ()                       0
#define SYS_SLEEP       3   // (ms)                     0
#define SYS_GETPID      4   // ()                       process id
#define SYS_SBRK        5   // (int increment)          previous break
#define SYS_OPEN        6   // (path, flags)            descriptor
#define SYS_CLOSE       7   // (fd)                     0
#define SYS_READ        8   // (fd, buf, length)        bytes read, 0 at EOF
#define SYS_WRITE       9   // (fd, buf, length)        bytes written
#define SYS_SEEK       10   // (fd, int offset, whence) new offset
#define SYS_FSTAT      11   // (fd, SysStat*)           0
#define SYS_STAT       12   // (path, SysStat*)         0
#define SYS_MKDIR      13   // (path)                   0
#define SYS_REMOVE     14   // (path)                   0 (file or EMPTY dir)
#define SYS_RENAME     15   // (path, new_name)         0 (same directory)
#define SYS_TRUNCATE   16   // (path, size)             0
#define SYS_READDIR    17   // (path, index, buf, size) name length, 0 at end
#define SYS_CHDIR      18   // (path)                   0
#define SYS_GETCWD     19   // (buf, size)              path length
#define SYS_COPY       20   // (src, dest)              0
#define SYS_GFX_INFO   21   // (SysGfxInfo*)            0
#define SYS_GFX_CREATE 22   // (width, height)          surface handle (>= 1)
#define SYS_GFX_DESTROY 23  // (handle)                 0
#define SYS_GFX_SUBMIT 24   // (handle, cmds, bytes)    commands run
#define SYS_GFX_WRITE  25   // (handle, SysGfxRect*, pixels, stride)  0
#define SYS_GFX_READ   26   // (handle, SysGfxRect*, pixels, stride)  0
#define SYS_GFX_PRESENT 27  // (handle, SysGfxRect* or 0, x, y)       0
#define SYS_SEC_GET    28   // (pid or 0)               security mask
#define SYS_SEC_SET    29   // (pid or 0, mask)         previous mask (needs SYS_SEC_ADMIN, except to drop own bits)
#define SYS_IPC_CLOSE  30   // (handle)                 0
#define SYS_IPC_PIPE   31   // (int32_t out[2])         0
#define SYS_IPC_PORT_CREATE 32 // (name)                port handle
#define SYS_IPC_CONNECT 33  // (name)                   pipe handle
#define SYS_IPC_ACCEPT 34   // (port)                   pipe handle, SYS_EAGAIN if none
#define SYS_IPC_SEND   35   // (pipe, buf, length, attach or 0)        0
#define SYS_IPC_RECV   36   // (pipe, buf, size, SysIpcMsgInfo* or 0)  message length
#define SYS_IPC_WAIT   37   // (SysIpcWaitItem*, count, timeout_ms)    items ready
#define SYS_SHM_CREATE 38   // (size)                   shm handle
#define SYS_SHM_MAP    39   // (handle)                 address
#define SYS_SHM_UNMAP  40   // (handle)                 0
#define SYS_IPC_INFO   41   // (handle, SysIpcInfo*)    0
#define SYS_INPUT_OPEN 42   // ()                       input handle
#define SYS_INPUT_READ 43   // (handle, SysInputEvent*, max)           events read
// 44 .. 46 unused (were res_load / res_get / res_info, the RSRC image
// resources; images are decoded in the program now, os/image.h)
#define SYS_GETHOME    47   // (buf, size)              home path length
#define SYS_TIME       48   // (uint64_t* ms)           0, milliseconds since boot
#define SYS_INPUT_WARP 49   // (handle, x, y, SysInputEvent* scratch)  0, pointer moved
#define SYS_SPAWN      50   // (path, argv or 0, cwd or 0, mask)       process id (needs SYS_SEC_EXEC)
#define SYS_FINFO      51   // (path, SysFileInfo*)     0
#define SYS_CHMOD      52   // (path, permissions)      0
#define SYS_SYNC       53   // ()                       0, every file system written out
#define SYS_ELEVATE    54   // (reason or 0, bits or 0) 0, the bits (0: SEC_ADMIN) granted (needs SYS_SEC_ELEVATE)
#define SYS_ELEVATE_AGENT 55 // (op, id, arg or buf)    by op (the desktop only)
#define SYS_FORMAT     56   // (device, SysFormatSpec*) 0 (FILE_READ to query; ADMIN + FILE_WRITE to format)
#define SYS_SPAWN_TTY  57   // (path, argv or 0, cwd or 0, mask, pipe) process id (needs EXEC + IPC)
#define SYS_IOCTL      58   // (fd, request, arg)       by request (terminals only)
#define SYS_TTY_RESIZE 59   // (pipe, columns, rows)    0 (needs SYS_SEC_IPC)
#define SYS_REG_GET    60   // (path, name, buf, size)  value length (needs FILE_READ)
#define SYS_REG_KEYS   61   // (path, index, buf, size) subkey name length, 0 past the last (needs FILE_READ)
#define SYS_SOCK_LISTEN 62  // (port, backlog)         socket handle (needs NET; + ADMIN below port 1024)
#define SYS_SOCK_CONNECT 63 // (SysSockAddr*)           socket handle, connecting (needs NET)
#define SYS_SOCK_ACCEPT 64  // (listener, SysSockAddr* or 0)  socket handle, SYS_EAGAIN if none
#define SYS_SOCK_SEND  65   // (handle, buf, length)    bytes queued
#define SYS_SOCK_RECV  66   // (handle, buf, size)      bytes read, 0 once the peer closed
#define SYS_SOCK_CLOSE 67   // (handle)                 0 (always allowed)
#define SYS_SOCK_WAIT  68   // (SysSockWaitItem*, count, timeout_ms)   items ready
#define SYS_SOCK_INFO  69   // (handle, SysSockInfo*)   0
#define SYS_DRV_LOAD   70   // (path, SysDrvInfo* or 0) 0, the driver is running   (needs ADMIN + FILE_READ)
#define SYS_DRV_UNLOAD 71   // (name)                   0                           (needs ADMIN)
#define SYS_REG_SET    72   // (path, name, data, size) 0 (needs FILE_WRITE; + ADMIN for hkey_root)
#define SYS_REG_VALUES 73   // (path, index, buf, size) value name length, 0 past the last (needs FILE_READ)
#define SYS_REG_DELETE 74   // (path, name or 0)        0: a value, or the key and all below it (as REG_SET)
#define SYS_REG_MKKEY  75   // (path)                   0 (as REG_SET)
#define SYS_REG_RENAME 76   // (path, name or 0, new_name)  0: a value, or the key itself (as REG_SET)
#define SYS_DRV_CALL   77   // (SysDrvCall*)            0, the function ran (its result is in the struct)
#define SYS_DRV_EXPORTS 78  // (driver, index, SysDrvExport*)  0, SYS_ENOENT past the last
#define SYS_MOVE       79   // (src, dest)              0, SYS_EXDEV if not on one volume
#define SYS_UNMOUNT    80   // (device)                 0, synced and gone from /mnt (needs FILE_WRITE)
#define SYS_SETPOLICY  81   // (path, policy)           0               (needs ADMIN + FILE_WRITE)
#define SYS_SETSECURITY 82  // (path, mask)             0               (needs ADMIN + FILE_WRITE)
#define SYS_CLOCK      83   // (uint64_t* ms)           0, milliseconds since 1970 (wall clock); SYS_ENOENT without one
#define SYS_CLOCK_SET  84   // (const uint64_t* ms)     0, the clock chip and wall clock set (needs ADMIN)
#define SYS_NET_RESOLVE 85 // (name, uint32_t* ip, timeout_ms)  0, the host's IPv4 address (needs NET)
#define SYS_SETWRITESEC 86  // (path, mask)             0, the extra bits writing needs  (needs ADMIN + FILE_WRITE)
#define SYS_RANDOM     87   // (buf, length)            bytes written (at most SYS_RANDOM_MAX), random bytes for keys
#define SYS_SETEXECPERM 88  // (path, allow, deny)      0, a program's per-program permissions  (needs ADMIN + FILE_WRITE)
#define SYS_POWER      89   // (op)                     does not return: SYS_POWER_OFF / SYS_POWER_RESTART (needs ADMIN)
#define SYS_COUNT      90

#define SYS_RANDOM_MAX 256  // Bytes one SYS_RANDOM call writes at most

#define SYS_POWER_OFF       0   // SYS_POWER: file systems written out, then soft off
#define SYS_POWER_RESTART   1   // SYS_POWER: file systems written out, then reset

#define SYS_REG_VALUE_MAX       4096        // SYS_REG_SET: bytes in one value, at most

#define SYS_OK               0
#define SYS_ENOSYS          -1
#define SYS_EFAULT          -2
#define SYS_EINVAL          -3
#define SYS_ENOMEM          -4
#define SYS_ENOENT          -5
#define SYS_EEXIST          -6
#define SYS_EACCES          -7
#define SYS_EBADF           -8
#define SYS_EMFILE          -9
#define SYS_EISDIR         -10
#define SYS_ENOTDIR        -11
#define SYS_ENOTEMPTY      -12
#define SYS_EBUSY          -13
#define SYS_ENAMETOOLONG   -14
#define SYS_ERANGE         -15
#define SYS_ENOSPC         -16
#define SYS_EIO            -17
#define SYS_EPERM          -18  // Not allowed by the process's security mask
#define SYS_ESRCH          -19  // No such process
#define SYS_EAGAIN         -20  // Not now (nothing queued, queue full)
#define SYS_EPIPE          -21  // The other end of the pipe is closed
#define SYS_EBADFMT        -22  // Not an executable (or other file) this kernel can load
#define SYS_E2BIG          -23  // Argument list too long (SYS_SPAWN)
#define SYS_ETIMEDOUT      -24  // Nobody answered in time (SYS_ELEVATE)
#define SYS_ENOTTY         -25  // Not a terminal, or a request it doesn't know (SYS_IOCTL)
#define SYS_ENETDOWN       -26  // No interface is up with an address
#define SYS_ENETUNREACH    -27  // No route to that address
#define SYS_EADDRINUSE     -28  // That port is taken
#define SYS_ECONNREFUSED   -29  // The peer refused the connection
#define SYS_ECONNRESET     -30  // The peer reset the connection
#define SYS_ENOTCONN       -31  // Not connected (any more), or a listener
#define SYS_EXDEV          -32  // Not on one volume: copy and remove instead (SYS_MOVE)
#define SYS_ECANCELED      -33  // The user said no to the prompt (SYS_SPAWN with SYS_SPAWN_ELEVATE)

#define SYS_ERROR_MIN       -4095
#define SYS_IS_ERROR(r)     ((uint32_t)(r) >= (uint32_t)SYS_ERROR_MIN)

#define SYS_PRINT_MAX       4096
#define SYS_PATH_MAX        512
#define SYS_NAME_MAX        255
#define SYS_FD_MAX          32
#define SYS_FD_FIRST        3
#define SYS_RW_MAX          (1024U * 1024U)
#define SYS_OFFSET_MAX      0x7FFFFFFFU

/* SYS_SPAWN (see the "Processes" section of the kernel's syscall.h). Every
   program starts with argc, argv[], NULL, envp NULL on its stack; crt0 hands
   that to __libc_start. */
#define SYS_SPAWN_INHERIT   0xFFFFFFFFu     // mask: the caller's own
#define SYS_SPAWN_ELEVATE   0x80000000u     // mask flag: add what the file needs, asking the user
#define SYS_ARG_MAX         64              // Strings in argv
#define SYS_ARG_BYTES       4096            // Bytes of them, terminators included

#define SYS_FD_STDIN        0
#define SYS_FD_STDOUT       1
#define SYS_FD_STDERR       2

/* Terminals (see the "Terminals" section of the kernel's syscall.h): a
   program started with SYS_SPAWN_TTY reads descriptor 0 from, and writes 1
   and 2 to, a pipe whose other end its host holds. */
#define SYS_TIOCGWINSZ          0x5413
#define SYS_FIONREAD            0x541B
#define SYS_TTY_DEFAULT_COLUMNS 80
#define SYS_TTY_DEFAULT_ROWS    25

typedef struct {
    uint16_t rows;
    uint16_t columns;
    uint16_t x_pixels;
    uint16_t y_pixels;
} SysWinSize;

#define SYS_O_READ          0x01
#define SYS_O_WRITE         0x02
#define SYS_O_CREATE        0x04
#define SYS_O_TRUNC         0x08    // Empty the file as it opens (needs WRITE): VFS_OPEN_TRUNCATE
#define SYS_O_APPEND        0x10    // Every write lands at the end (needs WRITE): VFS_OPEN_APPEND
#define SYS_O_EXCL          0x20

#define SYS_SEEK_SET        0
#define SYS_SEEK_CUR        1
#define SYS_SEEK_END        2

#define SYS_TYPE_FILE       0
#define SYS_TYPE_DIRECTORY  1
#define SYS_TYPE_MOUNT      2

#define SYS_PERM_EXECUTE    0x01
#define SYS_PERM_READ       0x02
#define SYS_PERM_WRITE      0x04

/* Security clearance bits (see os/security.h and the kernel's syscall.h) */
#define SYS_SEC_FILE_READ   0x00000001u
#define SYS_SEC_FILE_WRITE  0x00000002u
#define SYS_SEC_CONSOLE     0x00000004u
#define SYS_SEC_GFX         0x00000008u
#define SYS_SEC_GFX_SCREEN  0x00000010u
#define SYS_SEC_IPC         0x00000020u
#define SYS_SEC_INPUT       0x00000040u
#define SYS_SEC_EXEC        0x00000080u
#define SYS_SEC_ADMIN       0x00000100u
#define SYS_SEC_ELEVATE     0x00000200u
#define SYS_SEC_NET         0x00000400u
#define SYS_SEC_NET_CONFIG  0x00000800u     // Join / leave Wi-Fi networks (with SYS_SEC_NET)
#define SYS_SEC_MICROPHONE  0x00001000u     // Record from a microphone (/dev/micN)
#define SYS_SEC_NONE        0x00000000u
#define SYS_SEC_VALID       (SYS_SEC_FILE_READ | SYS_SEC_FILE_WRITE | SYS_SEC_CONSOLE | \
                             SYS_SEC_GFX | SYS_SEC_GFX_SCREEN | SYS_SEC_IPC | \
                             SYS_SEC_INPUT | SYS_SEC_EXEC | SYS_SEC_ADMIN | SYS_SEC_ELEVATE | \
                             SYS_SEC_NET | SYS_SEC_NET_CONFIG | SYS_SEC_MICROPHONE)
/* Per-program permissions: the bits a program file can be given, or kept from (SYS_SETEXECPERM) */
#define SYS_SEC_PROGRAM_GATED   (SYS_SEC_NET | SYS_SEC_MICROPHONE | SYS_SEC_INPUT)

/* SYS_ELEVATE / SYS_ELEVATE_AGENT: see "Elevation" in the kernel's syscall.h */
#define SYS_ELEVATE_OP_REGISTER  1
#define SYS_ELEVATE_OP_NEXT      2
#define SYS_ELEVATE_OP_STATUS    3
#define SYS_ELEVATE_OP_ANSWER    4

#define SYS_ELEVATE_REASON_MAX   128
#define SYS_ELEVATE_NAME_MAX     16
#define SYS_ELEVATE_PATH_MAX     256
#define SYS_ELEVATE_TIMEOUT_MS   120000
#define SYS_ELEVATE_COOLDOWN_MS  5000
#define SYS_ELEVATE_ARM_MS       750

#define SYS_ELEVATE_LAUNCH       0x0001     // flags: name/path are the program to be started, `by` the caller

typedef struct {
    uint32_t id;
    int32_t  pid;
    uint32_t security;
    uint32_t waiting;
    char     name[SYS_ELEVATE_NAME_MAX];
    char     path[SYS_ELEVATE_PATH_MAX];
    char     reason[SYS_ELEVATE_REASON_MAX];
    uint32_t flags;
    char     by[SYS_ELEVATE_NAME_MAX];
    uint32_t requested;                     // SYS_SEC_* bits the user is asked for
} SysElevateRequest;

_Static_assert(sizeof(SysElevateRequest) == 16 + 16 + 256 + 128 + 4 + 16 + 4, "SysElevateRequest is part of the kernel ABI");

/* SYS_FORMAT: see "Formatting" in the kernel's syscall.h, and os/disk.h */
#define SYS_FORMAT_QUERY        0x0001
#define SYS_FORMAT_QUICK        0x0002
#define SYS_FORMAT_FLAGS        (SYS_FORMAT_QUERY | SYS_FORMAT_QUICK)

#define SYS_FORMAT_INFO_WHOLE_DISK  0x0001
#define SYS_FORMAT_INFO_FORMATTED   0x0002
#define SYS_FORMAT_INFO_SYSTEM      0x0004

#define SYS_FORMAT_BLOCK_MIN    512
#define SYS_FORMAT_BLOCK_MAX    4096
#define SYS_FORMAT_FS_MAX       16

typedef struct {
    uint32_t size;                  /* sizeof(SysFormatSpec) */
    uint32_t flags;                 /* SYS_FORMAT_QUERY / SYS_FORMAT_QUICK */
    uint32_t block_size;            /* in: 0 = current (else 512); out: used */
    uint32_t reserved;              /* 0 */
    uint64_t capacity;              /* in: 0 = default; out: used */
    uint64_t max_capacity;          /* out */
    uint64_t device_bytes;          /* out */
    uint32_t device_sector_size;    /* out */
    uint32_t info;                  /* out: SYS_FORMAT_INFO_* */
    char     filesystem[SYS_FORMAT_FS_MAX];  /* in: "basefs" or ""; out: used */
} SysFormatSpec;

//
// Drivers
//
// SYS_DRV_LOAD(path, SysDrvInfo* info or 0) loads a driver file and starts
// it. A driver file is known by its first four bytes, "kdrv", not by its
// name (the format is kernel/driver/kdrv.h; drivers that come with the
// system are in sys/drivers/ under home). Its code runs in the kernel with
// full access to the machine, so the call needs SYS_SEC_ADMIN, and
// SYS_SEC_FILE_READ to read the file; a user program asks for admin first
// (SYS_ELEVATE). The file is read once, with the caller's clearance, and
// only that copy is checked and run.
//
// The driver is registered like a built-in one: it is offered every device
// nothing holds yet, and is listed in /drv with origin "loaded". info (may
// be 0) gets its name and version and how many devices it took.
//
// SYS_DRV_UNLOAD(name) stops a loaded driver: remove() runs on every device
// it holds, then its code is freed. Needs SYS_SEC_ADMIN.
//
// One load or unload runs at a time, system-wide. Every load and unload is
// logged on COM1 with the caller's pid and name.
//
// Returns SYS_OK or:
//   SYS_EPERM      no SYS_SEC_ADMIN (load: or no SYS_SEC_FILE_READ)
//   SYS_ENOENT     no such file / no loaded driver of that name
//   SYS_EACCES     the file is not readable with the caller's clearance;
//                  unload: a built-in driver
//   SYS_EBADFMT    not a driver file, or a damaged one (header, tables or
//                  checksum), or one built for another CPU, DRIVER_ABI or a
//                  newer kernel API, or larger than SYS_DRV_FILE_MAX
//   SYS_EEXIST     a driver of that name is registered already
//   SYS_ENOSPC     SYS_DRV_LOADED_MAX drivers loaded, or the driver table is full
//   SYS_ENOMEM     out of kernel memory
//   SYS_EIO        the file could not be read, or the driver would not start
//                  (its entry point said no or returned nonsense)
//   SYS_EFAULT     bad path, name or info pointer
//   SYS_ENAMETOOLONG

#define SYS_DRV_NAME_MAX        16                  // == DRIVER_NAME_MAX
#define SYS_DRV_VERSION_MAX     16
#define SYS_DRV_FILE_MAX        (1024u * 1024u)     // == KDRV_FILE_MAX
#define SYS_DRV_LOADED_MAX      8                   // == KDRV_LOADED_MAX

// Filled by SYS_DRV_LOAD. 48 bytes.
typedef struct {
    char     name[SYS_DRV_NAME_MAX];        // "hello"
    char     version[SYS_DRV_VERSION_MAX];  // "1.0", "" if it gives none
    uint32_t image_size;                    // Kernel memory it occupies, bytes
    uint32_t devices;                       // Devices it took when it started
    uint32_t reserved[2];
} SysDrvInfo;

// Library drivers: SYS_DRV_CALL / SYS_DRV_EXPORTS (kernel syscall.h, "Library drivers")
#define SYS_DRV_ARGS_MAX        8
#define SYS_DRV_STRING_MAX      256     // Terminator included
#define SYS_DRV_OUTPUT_MAX      1024    // Terminator included
#define SYS_DRV_FUNC_MAX        24      // Terminator included
#define SYS_DRV_HELP_MAX        64      // Terminator included

#define SYS_DRV_ARG_INT         'i'
#define SYS_DRV_ARG_STRING      's'

typedef struct {
    uint32_t type;                          // SYS_DRV_ARG_*
    uint32_t value;                         // INT: the number; STRING: address of the string
} SysDrvArg;

typedef struct {
    uint32_t driver;                        // in:  address of the library's name
    uint32_t function;                      // in:  address of the function's name
    uint32_t args;                          // in:  address of SysDrvArg[argc]
    uint32_t argc;                          // in
    uint32_t out;                           // in:  address of a buffer for the text answer, or 0
    uint32_t out_size;                      // in:  its size (0: no text wanted)
    int32_t  result;                        // out: what the function returned
    uint32_t out_length;                    // out: bytes of text, terminator not counted
    uint32_t reserved[2];
} SysDrvCall;

typedef struct {
    char     name[SYS_DRV_FUNC_MAX];        // "add"
    char     params[SYS_DRV_ARGS_MAX + 4];  // "ii", "" for none
    char     help[SYS_DRV_HELP_MAX];
    uint32_t security;                      // SYS_SEC_* bits a caller needs
} SysDrvExport;

_Static_assert(sizeof(SysDrvArg) == 8 && sizeof(SysDrvCall) == 40 && sizeof(SysDrvExport) == 104,
               "library driver structures are part of the kernel ABI");

_Static_assert(sizeof(SysFormatSpec) == 64, "SysFormatSpec is part of the kernel ABI");

typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t permissions;
    uint32_t created;
    uint32_t modified;
} SysStat;

_Static_assert(sizeof(SysStat) == 20, "SysStat is part of the kernel ABI");

/* SYS_FINFO / SYS_CHMOD: see "File details" in the kernel's syscall.h */
#define SYS_FINFO_DEVICE    0x0001  // A storage device's root: device_* are filled in
#define SYS_FINFO_ACCESS    0x0002  // The caller's clearance covers `security`
#define SYS_FINFO_CHMOD     0x0004  // SYS_CHMOD on it would be allowed for the caller
#define SYS_FINFO_COUNTING  0x0008  // DEVICE: still counting the used space; device_used is 0
                                    // and device_percent how far it got. Ask again in a moment.
#define SYS_FINFO_CONSOLE   0x0010  // No ACCESS, an executable: the program kind its marks say,
#define SYS_FINFO_WRITE     0x0040  // The caller's clearance covers writing it (security | write_security)
#define SYS_FINFO_WINDOWED  0x0020  // read by the kernel (program_kind() falls back on these)

typedef struct {
    uint32_t security;          // SYS_SEC_* bits the file requires (0: anyone)
    uint32_t flags;             // SYS_FINFO_*
    uint32_t device_used;       // DEVICE: bytes in use and in all, low 32 bits
    uint32_t device_capacity;
    uint32_t device_used_hi;    // DEVICE: high 32 bits of the two above
    uint32_t device_capacity_hi;
    uint32_t device_percent;    // DEVICE + COUNTING: how much has been counted (0..99)
    uint32_t policy;            // SYS_POLICY_*: the group policy it belongs to (0: none)
    uint32_t write_security;    // SYS_SEC_* bits writing needs on top of `security` (0: the same as reading)
    uint32_t exec_allow;        // Program file: SYS_SEC_PROGRAM_GATED bits it is given when started (SYS_SETEXECPERM)
    uint32_t exec_deny;         // ... and never given
    uint32_t reserved;          // 0
} SysFileInfo;

_Static_assert(sizeof(SysFileInfo) == 48, "SysFileInfo is part of the kernel ABI");

// Group policies (SysFileInfo.policy, SYS_SETPOLICY). The kernel's table,
// listed read-only in the registry under hkey_root/system/policy/<name>
// (id, permissions, clearance, description).
#define SYS_POLICY_NONE     0   // In no group: only the security mask counts
#define SYS_POLICY_USER     1   // What user programs start with; any process may use it
#define SYS_POLICY_ADMIN    2   // user + SYS_SEC_ADMIN; needs admin
#define SYS_POLICY_SYSTEM   3   // Every permission; needs every bit user programs lack
#define SYS_POLICY_NETWORK  4   // user + SYS_SEC_NET; needs SYS_SEC_NET

/* Graphics surfaces and the SYS_GFX_SUBMIT command format: see the
   "Graphics" section of kernel/syscall/syscall.h, and os/gfx.h for the
   library on top of them. */
#define SYS_GFX_SURFACE_MAX     256             // Surfaces per process (the table grows to this)
#define SYS_GFX_MAX_DIM         4096            // Largest surface width / height
#define SYS_GFX_PROCESS_BYTES   (32U << 20)     // Pixel memory per process
#define SYS_GFX_SUBMIT_MAX      65536           // Bytes per SYS_GFX_SUBMIT
#define SYS_GFX_COORD_MAX       8191
#define SYS_GFX_SIZE_MAX        16384
#define SYS_GFX_RADIUS_MAX      8191
#define SYS_GFX_DEPTH_MAX       4096            // Isometric pie depth
#define SYS_GFX_TEXT_MAX        255             // Bytes per text command
#define SYS_GFX_PIE_MAX         32              // Slices per pie
#define SYS_GFX_STRIDE_MAX      65536           // Pixels per row in user buffers

//      Op                          Arguments
#define SYS_GFX_CMD_CLEAR        1  // color
#define SYS_GFX_CMD_PIXEL        2  // x y color
#define SYS_GFX_CMD_LINE         3  // x0 y0 x1 y1 color
#define SYS_GFX_CMD_RECT         4  // x y w h color                     (outline)
#define SYS_GFX_CMD_FILL_RECT    5  // x y w h color
#define SYS_GFX_CMD_GRADIENT     6  // x y w h from to direction (SYS_GFX_GRADIENT_*)
#define SYS_GFX_CMD_CIRCLE       7  // cx cy radius color
#define SYS_GFX_CMD_FILL_CIRCLE  8  // cx cy radius color
#define SYS_GFX_CMD_TEXT         9  // x y fg bg flags length, then length bytes (padded to a word)
#define SYS_GFX_CMD_CHAR        10  // x y char fg bg flags
#define SYS_GFX_CMD_PIE         11  // cx cy radius depth flags count, values[count], colors[count]
#define SYS_GFX_CMD_BLIT        12  // src_handle sx sy sw sh x y flags key
#define SYS_GFX_CMD_PIXELS      13  // user_address width height stride x y flags key
#define SYS_GFX_CMD_SET_CLIP    14  // x y w h flags
#define SYS_GFX_CMD_INTERSECT_CLIP 15 // x y w h
#define SYS_GFX_CMD_SET_ORIGIN  16  // x y                               (result within COORD_MAX)
#define SYS_GFX_CMD_TRANSLATE   17  // dx dy                             (result within COORD_MAX)
#define SYS_GFX_CMD_SET_BLEND   18  // SYS_GFX_BLEND_*
#define SYS_GFX_CMD_TEXT_SCALE  19  // 1 .. 8
#define SYS_GFX_CMD_SAVE        20  //
#define SYS_GFX_CMD_RESTORE     21  //
// 22 unused (was RESOURCE)

#define SYS_GFX_CMD_HEADER(op, words)   ((uint32_t)(op) | ((uint32_t)(words) << 16))

// Command flags
#define SYS_GFX_F_OPAQUE        0x01    // TEXT, CHAR: fill each cell with bg first
#define SYS_GFX_F_ISOMETRIC     0x01    // PIE: tilted, with a `depth` pixel side
#define SYS_GFX_F_WHOLE         0x01    // BLIT: the whole source (sx sy sw sh ignored)
#define SYS_GFX_F_KEYED         0x02    // BLIT, PIXELS: skip pixels equal to key
#define SYS_GFX_F_NO_CLIP       0x01    // SET_CLIP: clip to the whole surface

#define SYS_GFX_BLEND_COPY      0
#define SYS_GFX_BLEND_ALPHA     1
#define SYS_GFX_GRADIENT_VERTICAL   0
#define SYS_GFX_GRADIENT_HORIZONTAL 1

typedef struct {
    int32_t x, y, w, h;
} SysGfxRect;

// Filled by SYS_GFX_INFO. 32 bytes.
typedef struct {
    uint32_t width;         // Screen size in pixels (0 when there is no display)
    uint32_t height;
    uint32_t bpp;           // 32
    uint32_t char_width;    // Built-in font cell at text scale 1
    uint32_t line_height;
    uint32_t max_dim;       // SYS_GFX_MAX_DIM
    uint32_t max_surfaces;  // SYS_GFX_SURFACE_MAX
    uint32_t max_bytes;     // SYS_GFX_PROCESS_BYTES
} SysGfxInfo;

_Static_assert(sizeof(SysGfxRect) == 16 && sizeof(SysGfxInfo) == 32, "gfx structures are part of the kernel ABI");

//
// IPC: pipes, ports and shared memory
//
// IPC objects are reached through HANDLES, private to the process, numbered
// 1 .. SYS_IPC_HANDLE_MAX (their own numbering, apart from file descriptors
// and gfx surfaces). Every handle is closed when the process exits.
//
// PIPE: two connected ends. Each end sends whole messages (1 ..
// SYS_IPC_MSG_MAX bytes) to the other and receives the ones sent to it, in
// order. Messages are never split or merged. A message may carry one handle
// (a pipe end or shared memory, not a port or input): the receiver gets a
// new handle to the same object, the sender keeps its own. An end cannot be
// sent through its own pipe (SYS_EINVAL), nor while messages queued for it
// carry pipe ends themselves (SYS_EBUSY: receive those first). That keeps
// ends from holding each other in a loop that could never be freed.
//   send: SYS_EAGAIN when the peer's queue is full (SYS_IPC_QUEUE_MAX
//         messages), SYS_EPIPE once the peer end is closed.
//   recv: the message length; SYS_EAGAIN when nothing is queued, SYS_EPIPE
//         when nothing is queued and the peer is closed (end of stream).
//         A buffer too small is SYS_ERANGE and the message STAYS queued;
//         its length is in SysIpcMsgInfo.length when info was given. If the
//         message carries a handle and this process has no free handle,
//         SYS_EMFILE, and the message stays queued too.
// Nothing blocks: wait for a handle to become ready with SYS_IPC_WAIT.
//
// PORT: a named rendezvous ("dwm", "audio.mixer"). Names are 1 ..
// SYS_IPC_NAME_MAX - 1 characters of [A-Za-z0-9._-], unique system-wide
// (SYS_EEXIST); names starting "sys." need SYS_SEC_ADMIN. SYS_IPC_CONNECT
// makes a new pipe, queues one end on the port (up to SYS_IPC_BACKLOG,
// SYS_EAGAIN beyond, SYS_ENOENT if no such port) and returns the other.
// The port's owner takes queued ends with SYS_IPC_ACCEPT. Closing the port
// handle removes the name; queued ends are closed (their clients see
// SYS_EPIPE).
//
// SHARED MEMORY: SYS_SHM_CREATE(size) makes a zeroed block of whole pages
// (at most SYS_SHM_MAX bytes; SYS_SHM_TOTAL_MAX for all blocks together).
// SYS_SHM_MAP maps it into this process, read-write, and returns the
// address (above 0x7FFFFFFF: test results with SYS_IS_ERROR()). Mapping a handle that is already mapped returns the same
// address. Pass the handle to another process in a message; it maps its own
// copy of the handle. Closing a handle unmaps it. The memory is freed when
// the last handle to it is closed.
//
// SYS_IPC_WAIT: block until at least one item is ready, the timeout passes
// (milliseconds; 0 = just poll, SYS_IPC_WAIT_FOREVER = no timeout) or the
// process is ending. Each item names a handle and the events wanted; the
// kernel fills `ready` for every item and returns how many items have a
// non-zero `ready` (0 on timeout). SYS_EBADF if any handle is not open.
//   PIPE   READABLE: a message is queued, or the peer is closed
//          WRITABLE: send would not fail with SYS_EAGAIN
//          HANGUP:   the peer is closed (always reported, even unasked)
//   PORT   READABLE: a connection is waiting for SYS_IPC_ACCEPT
//   INPUT  READABLE: an input event is waiting
//   SHM    never ready
// Waiting on a pipe, port or shm needs SYS_SEC_IPC, on input SYS_SEC_INPUT.
//

#define SYS_IPC_HANDLE_MAX      1024            // Handles per process (the table grows to this)
#define SYS_IPC_MSG_MAX         512             // Bytes per message
#define SYS_IPC_QUEUE_MAX       64              // Messages queued per direction
#define SYS_IPC_GLOBAL_BYTES    (4U << 20)      // Message bytes queued system-wide
#define SYS_IPC_NAME_MAX        32              // Port name bytes, terminator included
#define SYS_IPC_BACKLOG         16              // Connections waiting per port
#define SYS_IPC_WAIT_MAX        128             // Items per SYS_IPC_WAIT
#define SYS_IPC_WAIT_FOREVER    0xFFFFFFFFu
#define SYS_SHM_MAX             (16U << 20)     // Bytes per shared block
#define SYS_SHM_TOTAL_MAX       (64U << 20)     // Bytes in all shared blocks together

// SysIpcInfo.type
#define SYS_IPC_TYPE_PIPE       1
#define SYS_IPC_TYPE_PORT       2
#define SYS_IPC_TYPE_SHM        3
#define SYS_IPC_TYPE_INPUT      4

// SysIpcWaitItem.events / .ready
#define SYS_IPC_READABLE        0x0001
#define SYS_IPC_WRITABLE        0x0002
#define SYS_IPC_HANGUP          0x0004

// SysIpcInfo.flags
#define SYS_IPC_INFO_PEER_CLOSED 0x0001         // Pipe: the other end is closed
#define SYS_IPC_INFO_MAPPED      0x0002         // Shm: this handle is mapped

// Filled by SYS_IPC_RECV when given. 16 bytes.
typedef struct {
    uint32_t length;        // Message length (also on SYS_ERANGE)
    int32_t  handle;        // Handle that came with the message, 0 if none
    uint32_t sender;        // Process id that sent it
    uint32_t reserved;
} SysIpcMsgInfo;

// One SYS_IPC_WAIT item. 8 bytes.
typedef struct {
    int32_t  handle;
    uint16_t events;        // SYS_IPC_READABLE | SYS_IPC_WRITABLE
    uint16_t ready;         // Set by the kernel
} SysIpcWaitItem;

// Filled by SYS_IPC_INFO. 32 bytes.
typedef struct {
    uint32_t type;          // SYS_IPC_TYPE_*
    uint32_t flags;         // SYS_IPC_INFO_*
    uint32_t size;          // Shm: bytes. Pipe: bytes queued to read.
    uint32_t address;       // Shm: where this handle is mapped, 0 if not
    uint32_t pending;       // Pipe: messages queued to read. Port: connections waiting.
    uint32_t reserved[3];
} SysIpcInfo;

//
// Network: TCP sockets (see "Network" in the kernel's syscall.h)
//

#define SYS_SOCK_PER_PROCESS        8
#define SYS_SOCK_BACKLOG_MAX        8
#define SYS_SOCK_IO_MAX             4096
#define SYS_SOCK_WAIT_MAX           16
#define SYS_SOCK_WAIT_FOREVER       0xFFFFFFFFu
#define SYS_SOCK_PORT_PRIVILEGED    1024

// SYS_NET_RESOLVE(name, uint32_t* ip, timeout_ms): the IPv4 address of a
// host name ("www.example.com", at most SYS_NET_NAME_MAX bytes), asked of
// the DNS server DHCP gave (else the gateway). A dotted address and
// "localhost" are answered at once. Blocks up to timeout_ms (0 or more than
// SYS_NET_RESOLVE_TIMEOUT_MAX: that maximum). Needs SYS_SEC_NET.
// SYS_ENOENT no such host (or no IPv4 address for it), SYS_ETIMEDOUT no
// answer, SYS_ENETDOWN no interface with an address, SYS_EINVAL not a
// host name, SYS_EIO the server failed the query.
#define SYS_NET_NAME_MAX            253
#define SYS_NET_RESOLVE_TIMEOUT_MAX 15000

#define SYS_SOCK_READABLE       0x0001
#define SYS_SOCK_WRITABLE       0x0002
#define SYS_SOCK_ACCEPTABLE     0x0004
#define SYS_SOCK_ERROR          0x0008
#define SYS_SOCK_CONNECTED      0x0010
#define SYS_SOCK_LISTENING      0x0020

// An IPv4 address and port, host order. 8 bytes.
typedef struct {
    uint32_t ip;
    uint16_t port;
    uint16_t reserved;      // 0
} SysSockAddr;

// One SYS_SOCK_WAIT item. 8 bytes.
typedef struct {
    int32_t  handle;
    uint16_t events;
    uint16_t ready;         // Set by the kernel
} SysSockWaitItem;

// Filled by SYS_SOCK_INFO. 32 bytes.
typedef struct {
    SysSockAddr local;
    SysSockAddr remote;
    uint32_t flags;         // SYS_SOCK_*
    int32_t  error;         // SYS_E* once the connection failed, else 0
    uint32_t reserved[2];
} SysSockInfo;

_Static_assert(sizeof(SysSockAddr) == 8 && sizeof(SysSockWaitItem) == 8 && sizeof(SysSockInfo) == 32,
               "socket structures are part of the kernel ABI");

//
// Input: keyboard and mouse
//
// SYS_INPUT_OPEN takes the keyboard and mouse for this process: from then
// on every key press and mouse change goes to it instead of the window
// manager, which stops drawing until the handle is closed (or the process
// ends) and then repaints the whole screen. One owner at a time: a second
// SYS_INPUT_OPEN is SYS_EBUSY. The first event read is always a mouse event
// with the current position and buttons.
//
// SYS_INPUT_READ copies up to `max` (at most SYS_INPUT_READ_MAX) events and
// returns how many; 0 when none are waiting. Wait for input with
// SYS_IPC_WAIT (READABLE). Mouse and key events are in order among
// themselves; a burst read in one call returns the mouse events first.
//
// Keys: presses only (releases are not reported, except SYS_KEY_ALT_UP when
// the last Alt key is let go, so a program can end an Alt+Tab); `key` is
// the character (ASCII, Shift applied) or one of SYS_KEY_*. `mods` holds the
// modifier keys at the time the event is read, `x` `y` `buttons` the mouse state.
//
// Mouse wheel: a MOUSE event with `wheel` != 0 (notches turned since the
// last event; + toward the user, i.e. scroll down). It may carry a move too.
//

#define SYS_INPUT_READ_MAX      32

#define SYS_INPUT_MOUSE         1
#define SYS_INPUT_KEY           2

#define SYS_INPUT_BTN_LEFT      0x01
#define SYS_INPUT_BTN_RIGHT     0x02
#define SYS_INPUT_BTN_MIDDLE    0x04

#define SYS_INPUT_MOD_SHIFT     0x01
#define SYS_INPUT_MOD_CTRL      0x02
#define SYS_INPUT_MOD_ALT       0x04

#define SYS_KEY_BACKSPACE       0x0001
#define SYS_KEY_ENTER           0x0002
#define SYS_KEY_TAB             0x0009
#define SYS_KEY_ESCAPE          0x001B
#define SYS_KEY_HOME            0x4700
#define SYS_KEY_UP              0x4800
#define SYS_KEY_PAGE_UP         0x4900
#define SYS_KEY_LEFT            0x4B00
#define SYS_KEY_RIGHT           0x4D00
#define SYS_KEY_END             0x4F00
#define SYS_KEY_DOWN            0x5000
#define SYS_KEY_PAGE_DOWN       0x5100
#define SYS_KEY_DELETE          0x5300
#define SYS_KEY_F1              0x3B00
#define SYS_KEY_F2              0x3C00
#define SYS_KEY_F3              0x3D00
#define SYS_KEY_F4              0x3E00
#define SYS_KEY_F5              0x3F00
#define SYS_KEY_F6              0x4000
#define SYS_KEY_F7              0x4100
#define SYS_KEY_F8              0x4200
#define SYS_KEY_F9              0x4300
#define SYS_KEY_F10             0x4400
#define SYS_KEY_F11             0x5700
#define SYS_KEY_F12             0x5800
#define SYS_KEY_ALT_UP          0x3800  // Not a press: both Alt keys were let go

// One input event. 16 bytes.
typedef struct {
    uint16_t type;          // SYS_INPUT_MOUSE / SYS_INPUT_KEY
    uint16_t key;           // KEY: the key (see above), MOUSE: 0
    int16_t  x, y;          // Mouse position, screen coordinates
    uint8_t  buttons;       // SYS_INPUT_BTN_*
    uint8_t  mods;          // SYS_INPUT_MOD_*
    int16_t  wheel;         // MOUSE: wheel notches, + toward the user (scroll
                            // down), - away (up); 0 when it didn't turn. KEY: 0
    uint32_t time_ms;       // Milliseconds since boot (mouse); read time (keys)
} SysInputEvent;

_Static_assert(sizeof(SysIpcMsgInfo) == 16 && sizeof(SysIpcWaitItem) == 8 &&
               sizeof(SysIpcInfo) == 32 && sizeof(SysInputEvent) == 16,
               "IPC structures are part of the kernel ABI");

//
// See-through color
//
// The window manager's images (and the programs that draw the same way)
// treat opaque black as see-through: draw them keyed on it, alpha
// blended. Black that must show is drawn as 0xFF010101.
//

#define SYS_GFX_SPRITE_KEY      0xFF000000u

#ifdef LIBC_SYSCALL_HOOK
/* Test builds route every call through a C function instead of the gate */
int32_t LIBC_SYSCALL_HOOK(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
#define __SYSCALL(n, a1, a2, a3, a4, a5) LIBC_SYSCALL_HOOK(n, a1, a2, a3, a4, a5)
#else
static inline int32_t __syscall_trap(uint32_t n, uint32_t a1, uint32_t a2,
                                     uint32_t a3, uint32_t a4, uint32_t a5) {
    int32_t result;
    __asm__ volatile("int $0x81"
                     : "=a"(result)
                     : "a"(n), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
                     : "memory");
    return result;
}
#define __SYSCALL(n, a1, a2, a3, a4, a5) __syscall_trap(n, a1, a2, a3, a4, a5)
#endif

static inline int32_t syscall0(uint32_t n) { return __SYSCALL(n, 0, 0, 0, 0, 0); }
static inline int32_t syscall1(uint32_t n, uint32_t a1) { return __SYSCALL(n, a1, 0, 0, 0, 0); }
static inline int32_t syscall2(uint32_t n, uint32_t a1, uint32_t a2) { return __SYSCALL(n, a1, a2, 0, 0, 0); }
static inline int32_t syscall3(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3) { return __SYSCALL(n, a1, a2, a3, 0, 0); }
static inline int32_t syscall4(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) { return __SYSCALL(n, a1, a2, a3, a4, 0); }
static inline int32_t syscall5(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) { return __SYSCALL(n, a1, a2, a3, a4, a5); }

#endif




