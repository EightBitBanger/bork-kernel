
#ifndef _KERNEL_SYSCALL_H_
#define _KERNEL_SYSCALL_H_

#include <stdint.h>
#include <stdbool.h>

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
#define SYS_GFX_INFO   21   // (SysGfxInfo*)            0
#define SYS_GFX_CREATE 22   // (width, height)          surface handle (>= 1)
#define SYS_GFX_DESTROY 23  // (handle)                 0
#define SYS_GFX_SUBMIT 24   // (handle, cmds, bytes)    commands run
#define SYS_GFX_WRITE  25   // (handle, SysGfxRect*, pixels, stride)  0
#define SYS_GFX_READ   26   // (handle, SysGfxRect*, pixels, stride)  0
#define SYS_GFX_PRESENT 27  // (handle, SysGfxRect* or 0, x, y)       0
#define SYS_SEC_GET    28   // (pid or 0)               security mask
#define SYS_SEC_SET    29   // (pid or 0, mask)         previous mask   (needs SYS_SEC_ADMIN)
#define SYS_IPC_CLOSE  30   // (handle)                 0
#define SYS_IPC_PIPE   31   // (int32_t out[2])         0, two connected pipe ends in out
#define SYS_IPC_PORT_CREATE 32 // (name)                port handle
#define SYS_IPC_CONNECT 33  // (name)                   pipe handle (client end)
#define SYS_IPC_ACCEPT 34   // (port)                   pipe handle (server end), SYS_EAGAIN if none
#define SYS_IPC_SEND   35   // (pipe, buf, length, attach or 0)        0
#define SYS_IPC_RECV   36   // (pipe, buf, size, SysIpcMsgInfo* or 0)  message length
#define SYS_IPC_WAIT   37   // (SysIpcWaitItem*, count, timeout_ms)    items ready, 0 on timeout
#define SYS_SHM_CREATE 38   // (size)                   shared memory handle
#define SYS_SHM_MAP    39   // (handle)                 user address
#define SYS_SHM_UNMAP  40   // (handle)                 0
#define SYS_IPC_INFO   41   // (handle, SysIpcInfo*)    0
#define SYS_INPUT_OPEN 42   // ()                       input handle (exclusive)
#define SYS_INPUT_READ 43   // (handle, SysInputEvent*, max)           events read, 0 if none
#define SYS_RES_LOAD   44   // (path)                   0
#define SYS_RES_GET    45   // (name)                   resource handle (>= 1)
#define SYS_RES_INFO   46   // (handle, SysResInfo*)    0
#define SYS_GETHOME    47   // (buf, size)              home path length (system device root)
#define SYS_TIME       48   // (uint64_t* ms)           0, milliseconds since boot
#define SYS_INPUT_WARP 49   // (handle, x, y, SysInputEvent* scratch)  0, pointer moved
#define SYS_SPAWN      50   // (path, argv or 0, cwd or 0, mask)       process id    (needs SYS_SEC_EXEC)
#define SYS_FINFO      51   // (path, SysFileInfo*)     0
#define SYS_CHMOD      52   // (path, permissions)      0
#define SYS_SYNC       53   // ()                       0, every file system written out
#define SYS_COUNT      54

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
#define SYS_EPERM          -18  // The process's security mask does not allow this call
#define SYS_ESRCH          -19  // No such process
#define SYS_EAGAIN         -20  // Not now: nothing to receive / queue full / try again
#define SYS_EPIPE          -21  // The other end of the pipe is closed
#define SYS_EBADFMT        -22  // Not a resource file / executable this kernel can load
#define SYS_E2BIG          -23  // Argument list too long (SYS_SPAWN)

#define SYS_ERROR_MIN       -4095

#define SYS_PRINT_MAX       4096    // Longer prints are truncated (the return value says how much went out)

//
// Security clearance
//
// Every process carries a 32-bit security mask. Each bit is one clearance;
// a syscall that needs a clearance the caller lacks fails with SYS_EPERM
// before it does anything. The mask is checked on every call, so taking a
// bit away also stops descriptors and surfaces that are already open.
//
//   SYS_SEC_FILE_READ    open for reading, read, stat, fstat, readdir, chdir,
//                        res_load (with SYS_SEC_GFX)
//   SYS_SEC_FILE_WRITE   open for writing / create / trunc / append, write
//                        (to a file), mkdir, remove, rename, truncate
//                        (copy needs both file bits)
//   SYS_SEC_CONSOLE      print, and write to descriptors 1 and 2
//   SYS_SEC_GFX          graphics surfaces: info, create, destroy, submit,
//                        write, read
//   SYS_SEC_GFX_SCREEN   present a surface onto the display. Needs
//                        SYS_SEC_GFX too.
//   SYS_SEC_IPC          pipes, ports and shared memory: pipe, port_create,
//                        connect, accept, send, recv, shm_create, shm_map,
//                        shm_unmap, and ipc_wait on those handles
//   SYS_SEC_INPUT        take the keyboard and mouse: input_open,
//                        input_read, and ipc_wait on an input handle
//   SYS_SEC_EXEC         start other programs: spawn
//   SYS_SEC_ADMIN        configure security masks: SYS_SEC_SET on any
//                        process (itself included) and SYS_SEC_GET on other
//                        processes; create ports named "sys.*"
//
// Always allowed: exit, yield, sleep, getpid, sbrk, close, seek, getcwd, gethome, time,
// ipc_close, ipc_info and SYS_SEC_GET on the caller itself.
//
// Files: every file and directory on a storage device carries a mask of
// these same bits, the clearance it requires. A process may open, list,
// change or remove it only when its own mask holds every bit of the file's
// (SYS_EACCES otherwise). Files marked with SYS_SEC_ADMIN are administrator
// files: a user program (SYS_SEC_ADMIN clear) cannot touch them. stat still
// works on them. A directory's mask also guards the way through it, as Unix
// search permission does: nothing inside a directory the process is not
// cleared for can be reached, whatever its own mask (SYS_EACCES from the
// path checks; a lookup through it reports SYS_ENOENT). New files and
// directories inherit their directory's mask.
//
// The bits are independent: SYS_SEC_ADMIN does not imply the others, but an
// administrative process can grant itself whatever it needs. An admin that
// clears its own SYS_SEC_ADMIN cannot get it back by itself.
//
// SYS_SEC_SET(pid, mask) replaces the whole mask of `pid` (0 = the caller)
// and returns the previous one. Unknown bits are SYS_EINVAL, a pid that is
// not a running process SYS_ESRCH. The change applies from the target's next
// syscall; a call already in progress finishes under the old mask.
//

#define SYS_SEC_FILE_READ   0x00000001u
#define SYS_SEC_FILE_WRITE  0x00000002u
#define SYS_SEC_FILE_RW     (SYS_SEC_FILE_READ | SYS_SEC_FILE_WRITE)
#define SYS_SEC_CONSOLE     0x00000004u
#define SYS_SEC_GFX         0x00000008u
#define SYS_SEC_GFX_SCREEN  0x00000010u
#define SYS_SEC_IPC         0x00000020u
#define SYS_SEC_INPUT       0x00000040u
#define SYS_SEC_EXEC        0x00000080u
#define SYS_SEC_ADMIN       0x00000100u

#define SYS_SEC_NONE        0x00000000u
#define SYS_SEC_VALID       (SYS_SEC_FILE_RW | SYS_SEC_CONSOLE | \
                             SYS_SEC_GFX | SYS_SEC_GFX_SCREEN | SYS_SEC_IPC | \
                             SYS_SEC_INPUT | SYS_SEC_EXEC | SYS_SEC_ADMIN)

//
// Files
//
// Paths are NUL-terminated, at most SYS_PATH_MAX bytes including the
// terminator. A path not starting with '/' is relative to the process's
// working directory (SYS_CHDIR). The launcher chooses the starting
// directory: by default the directory holding the executable, "/" for a
// process with no executable file or when the chosen directory is not
// usable. "." and ".." are folded as text before the VFS sees the path.
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
#define SYS_O_TRUNC         0x08    // Empty an existing file (needs WRITE)       = VFS_OPEN_TRUNCATE
#define SYS_O_APPEND        0x10    // Every write goes to the end (needs WRITE)  = VFS_OPEN_APPEND
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

//
// Graphics
//
// A process draws into SURFACES: blocks of 32 bpp 0xAARRGGBB pixels the
// kernel owns, each with its own drawing state (clip, origin, blend mode,
// text scale, a save/restore stack), the same as a kernel GfxContext.
// Handles are private to the process, 1 .. SYS_GFX_SURFACE_MAX, and every
// surface is freed when the process exits. A new surface is opaque black.
//
// Drawing is batched: the process writes commands into a buffer and hands
// it over with SYS_GFX_SUBMIT, which runs them in order with the kernel's
// gfx layer (one syscall per frame instead of one per rectangle).
// SYS_GFX_WRITE / SYS_GFX_READ move raw pixels in and out of a surface
// (no clip, no blending). SYS_GFX_PRESENT puts (part of) a surface on the
// screen with its top-left corner at (x, y), in screen coordinates.
//
// Command format: a header word, op in bits 0-15 and the command's total
// length in 32-bit words (header included) in bits 16-31, then that many
// minus one argument words. Arguments are int32 unless noted; colors are
// 0xAARRGGBB. Coordinates are logical (the surface's origin is added) and
// must lie in [-SYS_GFX_COORD_MAX, SYS_GFX_COORD_MAX]; widths and heights
// in [-SYS_GFX_SIZE_MAX, SYS_GFX_SIZE_MAX] (zero or negative draws nothing).
//
// A command with a bad length ends the batch. A command with a bad value is
// skipped and the rest still run. Either way SYS_GFX_SUBMIT then returns
// the first error; otherwise the number of commands run.
//

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
#define SYS_GFX_CMD_RESOURCE    22  // resource_handle sx sy sw sh x y flags key   (see "Resources")

#define SYS_GFX_CMD_HEADER(op, words)   ((uint32_t)(op) | ((uint32_t)(words) << 16))

// Command flags
#define SYS_GFX_F_OPAQUE        0x01    // TEXT, CHAR: fill each cell with bg first
#define SYS_GFX_F_ISOMETRIC     0x01    // PIE: tilted, with a `depth` pixel side
#define SYS_GFX_F_WHOLE         0x01    // BLIT: the whole source (sx sy sw sh ignored)
#define SYS_GFX_F_KEYED         0x02    // BLIT, PIXELS: skip pixels equal to key
#define SYS_GFX_F_NO_CLIP       0x01    // SET_CLIP: clip to the whole surface
#define SYS_GFX_F_ALPHA         0x04    // RESOURCE: blend by alpha, whatever the blend mode

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
// PORT: a named rendezvous ("shell", "audio.mixer"). Names are 1 ..
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
// address (above 0x7FFFFFFF: test results with SYS_IS_ERROR style unsigned
// compares). Mapping a handle that is already mapped returns the same
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
// Input: keyboard and mouse
//
// SYS_INPUT_OPEN takes the keyboard and mouse for this process: from then
// on every key press and mouse change goes to it instead of the kernel
// console, until the handle is closed (or the process ends). One owner at a time: a second
// SYS_INPUT_OPEN is SYS_EBUSY. The first event read is always a mouse event
// with the current position and buttons.
//
// SYS_INPUT_READ copies up to `max` (at most SYS_INPUT_READ_MAX) events and
// returns how many; 0 when none are waiting. Wait for input with
// SYS_IPC_WAIT (READABLE). Mouse and key events are in order among
// themselves; a burst read in one call returns the mouse events first.
//
// Keys: presses only (releases are not reported); `key` is the character
// (ASCII, Shift applied) or one of SYS_KEY_*. `mods` holds the modifier
// keys at the time the event is read, `x` `y` `buttons` the mouse state.
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

// One input event. 16 bytes.
typedef struct {
    uint16_t type;          // SYS_INPUT_MOUSE / SYS_INPUT_KEY
    uint16_t key;           // KEY: the key (see above), MOUSE: 0
    int16_t  x, y;          // Mouse position, screen coordinates
    uint8_t  buttons;       // SYS_INPUT_BTN_*
    uint8_t  mods;          // SYS_INPUT_MOD_*
    uint16_t reserved;
    uint32_t time_ms;       // Milliseconds since boot (mouse); read time (keys)
} SysInputEvent;

//
// Resources
//
// SYS_RES_LOAD reads a resource file (the kernel's RSRC format, see
// resources/resource_manager.h) into this process and registers it under
// its NAME: the file's own name, the last part of the path
// ("/x/images/icon_folder" is "icon_folder"). SYS_RES_GET turns a name into a handle, 1 ..
// SYS_RES_MAX, which SYS_GFX_CMD_RESOURCE draws from and SYS_RES_INFO
// describes. Names and handles are private to the process; everything it
// loaded is freed when it exits.
//
// Loading the same file again is a no-op (0). Another file with the same
// name is SYS_EEXIST. A path that is not a file is SYS_ENOENT / SYS_EISDIR;
// a file that is not a resource, holds a type this kernel cannot load, is
// damaged, or is larger than SYS_GFX_MAX_DIM either way is SYS_EBADFMT.
// The decoded image counts against the same pixel budget as surfaces
// (SYS_GFX_PROCESS_BYTES), so SYS_ENOMEM past that.
//
// Supported: sprites (SYS_RES_TYPE_SPRITE), decoded to 0xAARRGGBB pixels.
// Sprites mark see-through pixels with opaque black, SYS_GFX_SPRITE_KEY;
// draw them keyed on it and alpha blended, as the libc's
// gfx_draw_resource* does (KEYED | ALPHA, key SYS_GFX_SPRITE_KEY).
//
// SYS_GFX_CMD_RESOURCE takes the same arguments as SYS_GFX_CMD_BLIT, with
// a resource handle as the source, plus SYS_GFX_F_ALPHA.
//
// Load needs SYS_SEC_FILE_READ and SYS_SEC_GFX; get and info SYS_SEC_GFX.
//

#define SYS_RES_MAX             64      // Resources per process
#define SYS_RES_NAME_MAX        64      // Name bytes, terminator included

#define SYS_RES_TYPE_SPRITE     1       // = RESOURCE_TYPE_SPRITE

#define SYS_GFX_SPRITE_KEY      0xFF000000u

// Filled by SYS_RES_INFO. 16 bytes.
typedef struct {
    uint32_t type;          // SYS_RES_TYPE_*
    uint32_t width;         // Pixels
    uint32_t height;
    uint32_t reserved;
} SysResInfo;

//
// File details: SYS_FINFO, SYS_CHMOD
//
// SYS_FINFO(path, SysFileInfo*) tells what SYS_STAT doesn't: the clearance
// a file requires (its security mask, the same SYS_SEC_* bits a process
// holds), whether the caller's clearance covers it, whether the caller could
// change its permissions, and for a storage device's root how full the
// device is. Works on files the caller may not open (like stat). Needs
// SYS_SEC_FILE_READ.
//
// SYS_CHMOD(path, permissions) replaces a file's or folder's SYS_PERM_* bits.
// Needs SYS_SEC_FILE_WRITE, clearance for the file and the folder holding
// it, and that folder must be writable (SYS_EACCES otherwise). A device's
// root is SYS_EBUSY; bits outside SYS_PERM_* are SYS_EINVAL.
//

#define SYS_FINFO_DEVICE    0x0001  // A storage device's root: device_* are filled in
#define SYS_FINFO_ACCESS    0x0002  // The caller's clearance covers `security`
#define SYS_FINFO_CHMOD     0x0004  // SYS_CHMOD on it would be allowed for the caller

// Filled by SYS_FINFO. 32 bytes.
typedef struct {
    uint32_t security;          // SYS_SEC_* bits the file requires (0: anyone)
    uint32_t flags;             // SYS_FINFO_*
    uint32_t device_used;       // DEVICE: as the device reports them (vfs_device_get_*)
    uint32_t device_capacity;
    uint32_t reserved[4];
} SysFileInfo;

//
// SYS_SYNC() writes what every mounted file system still holds in memory
// (cached blocks, directory entries, allocation tables) to its device, so a
// file just written, renamed or removed survives a reset. Needs
// SYS_SEC_FILE_WRITE; SYS_EIO if a device refused a write.
//

//
// Processes
//
// SYS_SPAWN(path, argv, cwd, mask) loads the ELF executable at `path`
// (relative paths start in the working directory) as a new process and
// returns its id. Needs SYS_SEC_EXEC. The caller does not wait: the new
// process may already be running when the call returns, and it lives on
// independently of the caller.
//
//   argv   user array of string pointers ending in NULL, or 0. Copied in
//          before anything else happens. At most SYS_ARG_MAX strings and
//          SYS_ARG_BYTES bytes of them, terminators included (SYS_E2BIG
//          beyond). 0 or an empty array gives argv = { resolved path }.
//          By convention argv[0] is the program's name.
//   cwd    the child's working directory: 0 = the caller's, "" = the
//          directory holding the executable, anything else a path
//          (relative to the caller's). One that is not a usable directory
//          starts the child in "/".
//   mask   the child's SYS_SEC_* mask, or SYS_SPAWN_INHERIT for the caller's
//          own. It must hold only bits the caller holds (SYS_EPERM): a
//          process can hand clearance down, never invent it.
//   The child runs at the caller's base priority.
//
// Errors: SYS_ENOENT, SYS_EISDIR, SYS_EACCES (the file's clearance, or not
// marked executable), SYS_EBADFMT (not an ELF this kernel can run),
// SYS_E2BIG, SYS_ENAMETOOLONG, SYS_EAGAIN (process table full), SYS_ENOMEM,
// SYS_EFAULT, SYS_EINVAL, SYS_EPERM.
//
// Program entry, for every process however it was started: ESP is 16-byte
// aligned and points at
//
//   [esp]          argc
//   [esp + 4]      argv[0] .. argv[argc - 1], then NULL
//   after that     envp: NULL (no environment is passed yet)
//
// with the strings themselves higher up on the same stack. A process the
// kernel starts itself gets argc 1, argv[0] = its image path (or its name
// for a flat image).
//

#define SYS_SPAWN_INHERIT       0xFFFFFFFFu     // mask: the caller's own
#define SYS_ARG_MAX             64              // Strings in argv
#define SYS_ARG_BYTES           4096            // Bytes of them, terminators included

// Kernel side: set up the serial output and locks the syscalls use. The IDT
// gate is installed by idt_init().
void syscall_init(void);

struct AddressSpace;

// Kernel side: give a new process its descriptor table and working
// directory. process_build() calls it after the address space exists and
// BEFORE the process's first thread is created, so no file call can ever
// run against a default table. `cwd` is an absolute path; NULL, or one that
// does not normalize or is not a directory, starts the process in "/".
// Thread context with interrupts on (it checks the directory through the
// VFS). Returns false only when out of memory. A process that fails to
// start after this succeeded must be cleaned up with syscall_process_exit().
bool syscall_process_start(uint32_t process_id, struct AddressSpace* space, const char* cwd);

// Kernel side: close every file a process has open, free its graphics
// surfaces, close its IPC handles and drop its descriptor table. DestroyProcess() calls it before freeing the address space, so
// every way a process ends (SYS_EXIT, a fatal fault, a kill) is covered;
// nothing else needs to. Thread context with interrupts on: it may sleep.
void syscall_process_exit(uint32_t process_id);

#endif






