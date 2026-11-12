
#ifndef _OS_OS_H
#define _OS_OS_H

/*
 * Calls specific to this kernel, with no standard equivalent. They follow
 * the usual convention: -1 and errno on failure.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <os/syscall.h>

/* Open with the kernel's own flags (the VFS_OPEN_* names, SYS_O_* values),
   no POSIX translation. At least one of READ / WRITE.
     OS_OPEN_CREATE     create the file if it does not exist
     OS_OPEN_TRUNCATE   empty it as it opens            (needs OS_OPEN_WRITE)
     OS_OPEN_APPEND     every write goes to the end     (needs OS_OPEN_WRITE)
     OS_OPEN_EXCL       with CREATE: EEXIST if it is already there
   TRUNCATE and APPEND work on files on a storage device; a file without
   write permission is EACCES (never opened read-only instead). The same as
   open() with O_TRUNC / O_APPEND, which map onto these.
   Returns a descriptor, or -1 with errno. */
#define OS_OPEN_READ        SYS_O_READ
#define OS_OPEN_WRITE       SYS_O_WRITE
#define OS_OPEN_RW          (SYS_O_READ | SYS_O_WRITE)
#define OS_OPEN_CREATE      SYS_O_CREATE
#define OS_OPEN_TRUNCATE    SYS_O_TRUNC
#define OS_OPEN_APPEND      SYS_O_APPEND
#define OS_OPEN_EXCL        SYS_O_EXCL
int os_open(const char* path, uint32_t flags);

/* Write straight to the console (and COM1). A NUL ends the console line. */
ssize_t os_print(const char* buf, size_t length);

/* Copy a file or a whole directory tree to a new path (must not exist) */
int os_copy(const char* src, const char* dest);

/* Rename within the same directory: new_name is a name, not a path */
int os_rename(const char* path, const char* new_name);

/* Move a file or a whole directory tree to a new full path (must not exist)
   on the same volume, by relinking its directory entry: nothing is copied,
   so it takes the same time whatever the size. Open files stay open.
   -1 with errno EXDEV when src and dest are on different volumes (copy and
   remove instead), EEXIST if dest is taken, EINVAL for a folder moved into
   itself. */
int os_move(const char* src, const char* dest);

/* The home directory: mount root of the device that holds the system
   folders ("/mnt/ata0p1"). Same buffer rules as getcwd(): buf NULL allocates
   (size 0 = PATH_MAX). Returns buf, or NULL with errno ENOENT (no home
   device), ERANGE (buf too small), EFAULT. Also in the environment as HOME. */
char* os_gethome(char* buf, size_t size);

/* "<home>/sys", or "<home>/sys/<sub>" when sub is given ("images",
   "images/icon_folder"). Returns out, or NULL with errno (ENAMETOOLONG
   if it does not fit, else as os_gethome). */
char* os_syspath(const char* sub, char* out, size_t size);

/* Where the program `name` is installed: the first of "<home>/sys/<name>"
   and "<home>/bin/<name>" that is a file. System programs come first, so
   nothing dropped into bin can stand in for one. Returns out, or NULL with
   errno (ENOENT if it is in neither, ENAMETOOLONG if it does not fit, else
   as os_gethome). */
char* os_programpath(const char* name, char* out, size_t size);

/* Milliseconds since boot. Monotonic (never goes back, never set); 0 if the
   kernel has no SYS_TIME. Also as clock_gettime(CLOCK_MONOTONIC) in time.h. */
uint64_t os_time_ms(void);

/* The wall clock (SYS_CLOCK): milliseconds since 1970-01-01 00:00:00 into
   *out, as the machine's clock chip keeps it (the CMOS clock on a PC). No
   time zones: the chip's time is shown as it is. Not monotonic: it can step
   a little when the kernel resyncs with the chip. 0, or -1 with errno ENOENT
   (out untouched) when the machine has no clock chip, or ENOSYS from an older
   kernel without SYS_CLOCK. time() and clock_gettime(CLOCK_REALTIME) in
   time.h use it, and fall back to uptime without it. */
int os_clock_ms(uint64_t* out);

/* Set the date and time (SYS_CLOCK_SET): ms since 1970, in os_clock_ms()'s
   terms. The clock chip keeps it across power off; only whole seconds
   reach it. Needs admin (sec_elevate() in os/security.h). 0, or -1 with
   errno: EPERM (not admin), EINVAL (before 2000 or after 2099: the PC's
   clock holds two year digits), ENOENT (no clock chip), EIO (the chip
   would not take it), ENOSYS (an older kernel). */
int os_clock_set(uint64_t ms);

/* Block for at least ms milliseconds (0 = just yield) */
int os_sleep_ms(uint32_t ms);

/* What stat() doesn't say (SYS_FINFO): the clearance a file requires
   (`security` to use it, `write_security` the extra bits writing needs),
   whether this process has them (SYS_FINFO_ACCESS, SYS_FINFO_WRITE),
   whether its permissions can be changed here, and how full a storage
   device is (for its root). 0, or -1 with errno. */
typedef SysFileInfo os_finfo_t;
int os_finfo(const char* path, os_finfo_t* out);

/* Replace a file's or folder's permissions: SYS_PERM_READ / _WRITE /
   _EXECUTE (chmod() in sys/stat.h takes POSIX modes). 0, or -1 with errno:
   EACCES (the folder holding it isn't writable, or no clearance for it),
   EBUSY (a device's root), EPERM (no file write clearance). */
int os_set_permissions(const char* path, uint32_t permissions);

/* Put a file or folder in a group policy: SYS_POLICY_USER, _ADMIN, _SYSTEM,
   _NETWORK, or SYS_POLICY_NONE to take it out of its group. Only the group
   changes: its security mask stays as it is (os_finfo's `security` shows
   it, `policy` the group). The policies are listed read-only in the registry
   under hkey_root/system/policy. Needs admin (sec_elevate() first). 0, or
   -1 with errno: as os_set_permissions, plus EPERM (not admin, or not
   cleared for the mask it has now) and EINVAL (no such policy). */
int os_set_policy(const char* path, uint32_t policy);

/* Replace a file's or folder's security mask (SYS_SEC_* bits a process
   needs to use it), leaving its policy. Needs admin, like os_set_policy;
   EINVAL for bits outside SYS_SEC_VALID. */
int os_set_security(const char* path, uint32_t mask);

/* Replace the extra SYS_SEC_* bits writing a file or folder needs on top of
   its security mask (os_finfo's write_security); 0 makes writing need what
   reading needs. Writing, creating or deleting inside a folder, renaming,
   moving and changing permissions all count as writing. Needs admin, like
   os_set_policy; EINVAL for bits outside SYS_SEC_VALID, EACCES on a file
   system that keeps no write mask (FAT). */
int os_set_write_security(const char* path, uint32_t mask);

/* The name of a group policy ("admin"), NULL for SYS_POLICY_NONE or an id
   this library doesn't know */
const char* os_policy_name(uint32_t policy);

/* Make `path` absolute against the working directory and fold "." and "..",
   the way the kernel does. Returns out, or NULL (errno) if it does not fit. */
/* Write everything the file systems still hold in memory out to their
   devices (SYS_SYNC). Do it before shutting down or after saving something
   that must survive a power cut. 0, or -1 with errno: EIO, EPERM (no file
   write clearance). */
int os_sync(void);

char* os_abspath(const char* path, char* out, size_t size);

/* Fill buf with `length` random bytes from the kernel's pool (SYS_RANDOM),
   good for keys and nonces. 0, or -1 with errno (EFAULT). Always allowed. */
int os_random(void* buf, size_t length);

#endif



