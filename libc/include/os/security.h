#ifndef _OS_SECURITY_H
#define _OS_SECURITY_H

/*
 * Process security clearance.
 *
 * Every process has a mask of clearance bits. A system call that needs a
 * bit the process lacks fails with errno EPERM and does nothing, so for
 * example fopen("x", "w") without SEC_FILE_WRITE returns NULL / EPERM. The
 * kernel checks the mask on every call: taking a bit away also stops files
 * and surfaces that are already open (close() always works).
 *
 *   SEC_FILE_READ    open for reading, read, stat, fstat, opendir/readdir,
 *                    chdir, access
 *   SEC_FILE_WRITE   open for writing / create / truncate / append, write to
 *                    a file, mkdir, remove, unlink, rmdir, rename, os_move,
 *                    truncate (os_copy and a rename across volumes need both)
 *   SEC_CONSOLE      printf to stdout / stderr, os_print
 *   SEC_GFX          os/gfx.h surfaces, and presenting them into this
 *                    program's window through the window manager
 *   SEC_GFX_SCREEN   presenting straight onto the display when no window
 *                    manager takes the surface (needs SEC_GFX as well)
 *   SEC_IPC          os/ipc.h pipes, ports and shared memory
 *   SEC_INPUT        os/input.h: take the keyboard and mouse
 *   SEC_EXEC         os/process.h: start other programs (spawn)
 *   SEC_ADMIN        change security masks: sec_set_process() on any
 *                    process, this one included, and reading other
 *                    processes' masks
 *   SEC_ELEVATE      ask the user for SEC_ADMIN with sec_elevate() (below)
 *   SEC_NET          os/net.h: TCP connections. Off unless a program is
 *                    given it; listening below port 1024 needs SEC_ADMIN too
 *   SEC_NET_CONFIG   join or leave a Wi-Fi network (with SEC_NET); only the
 *                    network tray has it
 *
 * Always available: exit, sbrk/malloc, sleep, yield, getpid, close, lseek,
 * getcwd, ipc_close, ipc_info, net_close, and reading this process's own mask.
 *
 * The bits are independent. SEC_ADMIN does not imply the others, but an
 * administrative process may grant itself any of them. A process that
 * clears its own SEC_ADMIN cannot get it back; only another admin (or the
 * kernel) can.
 *
 * pid 0 always means "this process".
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <os/syscall.h>

typedef uint32_t sec_mask_t;

#define SEC_FILE_READ    SYS_SEC_FILE_READ
#define SEC_FILE_WRITE   SYS_SEC_FILE_WRITE
#define SEC_FILE         (SEC_FILE_READ | SEC_FILE_WRITE)
#define SEC_CONSOLE      SYS_SEC_CONSOLE
#define SEC_GFX          SYS_SEC_GFX
#define SEC_GFX_SCREEN   SYS_SEC_GFX_SCREEN
#define SEC_IPC          SYS_SEC_IPC
#define SEC_INPUT        SYS_SEC_INPUT
#define SEC_EXEC         SYS_SEC_EXEC
#define SEC_ADMIN        SYS_SEC_ADMIN
#define SEC_ELEVATE      SYS_SEC_ELEVATE
#define SEC_NET          SYS_SEC_NET
#define SEC_NET_CONFIG   SYS_SEC_NET_CONFIG

#define SEC_NONE         SYS_SEC_NONE
#define SEC_ALL          SYS_SEC_VALID      // Every bit the kernel knows

//
// This process
//

// Get this process's security mask
sec_mask_t sec_get(void);

// True when this process matches `bits`
bool sec_has(sec_mask_t bits);

// True when this process may change security masks
bool sec_is_admin(void);

//
// Changing masks (SEC_ADMIN only)
//
// All return 0, or -1 with errno:
//   EPERM   this process does not have SEC_ADMIN (or, for reading another
//           process's mask, is not an admin)
//   ESRCH   no running process with that id
//   EINVAL  bits outside SEC_ALL
//

// Read any process's mask into *out (pid 0: this one; others need SEC_ADMIN)
int sec_get_process(pid_t pid, sec_mask_t* out);

// Replace a process's whole mask. *previous (may be NULL) gets the old one.
int sec_set_process(pid_t pid, sec_mask_t mask, sec_mask_t* previous);

// Add / remove bits, leaving the rest as they are. Both read the mask and
// then write it back, so two admins changing one process at the same moment
// can overwrite each other; use sec_set_process() for an exact mask.
int sec_grant(pid_t pid, sec_mask_t bits);
int sec_revoke(pid_t pid, sec_mask_t bits);

// sec_set_process(0, ...)
int sec_set(sec_mask_t mask);

//
// Asking the user for admin (needs SEC_ELEVATE)
//
// sec_elevate() puts a prompt in front of the user, drawn by the desktop:
// this program's name and path (as the kernel knows them, not as we say),
// `reason` underneath as our own words, and Allow / Cancel. It blocks until
// the user answers (at most SYS_ELEVATE_TIMEOUT_MS, two minutes). On Allow
// this process holds SEC_ADMIN when it returns.
//
//     if (sec_elevate("Format the USB stick in drive usb0") < 0) {
//         if (errno == EACCES) ...         // the user said no
//     }
//     ... the admin-only work ...
//     sec_drop(SEC_ADMIN);                 // give it back when done
//
// reason: NULL, or one short line (127 bytes are kept; anything but
// printable ASCII shows as '?'). 0 (at once when we are admin already), or
// -1 with errno:
//   EACCES     the user chose Cancel
//   ETIMEDOUT  nobody answered
//   EAGAIN     no desktop is running to ask
//   EBUSY      too many prompts waiting, or this program was refused a
//              moment ago (SYS_ELEVATE_COOLDOWN_MS)
//   EPERM      this process has no SEC_ELEVATE
//
// Admin got this way is not passed on to programs we spawn with
// SPAWN_INHERIT; name SEC_ADMIN in spawn_ex()'s mask to hand it down.
int sec_elevate(const char* reason);

// The same prompt for just the permissions a program needs, which it
// lists by name, and only those are granted: a downloader asks for
// SEC_NET, not for administration. 0 once every bit in `bits` is held
// (at once when they already are, or without a prompt for an
// administrator), or -1 with the errno values above (EINVAL: bits is 0 or
// has unknown bits).
//
//     if (!sec_has(SEC_NET) && sec_elevate_bits("Download a file", SEC_NET) < 0)
//         ...
int sec_elevate_bits(const char* reason, sec_mask_t bits);

// Writing has its own clearance (os_finfo's write_security, on top of
// security): a folder like <home>/sys can be read by every program and
// changed only by an administrator. These tell what is missing for a
// change and ask for just that, so a program can offer the prompt when a
// write is refused (EACCES) instead of only failing.
//
// sec_write_missing: the bits this process lacks to write, delete, rename
// or change the permissions of `path`. With `entry` also those to add or
// remove it in its folder (creating a file, deleting, renaming, moving).
// 0 when nothing is missing or the path can't be looked at (a path that
// doesn't exist yet only counts through its folder).
//
// sec_elevate_write: sec_elevate_bits() for exactly those bits. 0 when
// nothing was missing, the bits now granted (> 0, so the program can
// sec_drop() them when done), or -1 with sec_elevate_bits()'s errno.
//
//     if (save(path) < 0 && errno == EACCES) {
//         int got = sec_elevate_write("Save notes.txt in a protected folder", path, true);
//         if (got > 0) { save(path); sec_drop((sec_mask_t)got); }
//     }
sec_mask_t sec_write_missing(const char* path, bool entry);
int sec_elevate_write(const char* reason, const char* path, bool entry);

//
// The prompt's side (the desktop only: needs SEC_ADMIN, SEC_INPUT and
// SEC_GFX_SCREEN, and admin that did not come from sec_elevate)
//

typedef SysElevateRequest sec_elevate_request_t;

// Become the one process that shows and answers elevation prompts. 0, or
// -1 with errno (EBUSY: another process is it).
int sec_agent_register(void);

// The request to show: 1 (*out filled in), 0 for none, -1 with errno
int sec_agent_next(sec_elevate_request_t* out);

// 1 while request `id` still waits for an answer, 0 once it is over, -1
int sec_agent_waiting(uint32_t id);

// The user's answer. -1 with errno EAGAIN for an Allow sooner than
// SYS_ELEVATE_ARM_MS after the request was handed out, ESRCH when it is over.
int sec_agent_answer(uint32_t id, bool allow);

// Give up bits this process holds for good: sec_revoke(0, bits). Any
// process may drop its own bits; getting one back needs SEC_ADMIN, so
// drop SEC_ADMIN last (or together with the rest, in the same call).
int sec_drop(sec_mask_t bits);

//
// Names
//

// "file_read", "graphics", "administration", "process communication", ...
// for one bit; NULL for anything else. sec_parse() also takes the older
// short names ("gfx", "gfx_screen", "ipc", "exec", "admin").
const char* sec_bit_name(sec_mask_t bit);

// The mask as names joined by '|' ("file_read|console"), "none" for 0, and
// unknown bits as one hex number at the end ("console|0x8000"). Like
// snprintf: the length the whole text needs, written truncated (and always
// terminated) when size > 0.
int sec_format(sec_mask_t mask, char* buf, size_t size);

// Parse names joined by '|', ',' or spaces ("file_read|gfx"), plus "none",
// "all", "file" (both file bits) and hex/decimal numbers. 0 and *out on
// success, -1 with errno EINVAL for an unknown name.
int sec_parse(const char* text, sec_mask_t* out);

#endif



