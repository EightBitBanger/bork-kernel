#ifndef _VIRTUAL_FILE_SYSTEM_H_
#define _VIRTUAL_FILE_SYSTEM_H_

#define VFS_INVALID_FILE             0

// Longest file/directory name in bytes, without the terminator, on a
// storage device. Must equal FS_NAME_MAX (checked in vfs_parse.c). A buffer
// that receives a name needs VFS_NAME_MAX + 1 bytes.
//
// Kernel nodes (/, /dev, /mnt, ...) keep their own, shorter limit:
// VFS_KNODE_NAME_MAX in vfs_internal.h.
#define VFS_NAME_MAX               255
#define PATH_TOKEN_MAX             (VFS_NAME_MAX + 1)

// Longest path in bytes, including the terminator. Longer paths are
// refused (never cut short). Path resolution keeps a copy on the stack.
#define VFS_PATH_MAX               512

#define VFS_OPEN_READ             0x01
#define VFS_OPEN_WRITE            0x02
#define VFS_OPEN_CREATE           0x04

#define VFS_PERMISSION_EXECUTE    0x01
#define VFS_PERMISSION_READ       0x02
#define VFS_PERMISSION_WRITE      0x04

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/vfs/vfs_h_addition.h>

typedef uint32_t File;

// FSFileStats.type
#define VFS_TYPE_FILE              0
#define VFS_TYPE_DIRECTORY         1
#define VFS_TYPE_MOUNT             2    // A device's root, or a mount whose device
                                        // is unformatted or detached (then empty)

typedef struct {
    uint32_t size;                      // Files only, 0 for directories and mounts
    
    uint8_t permissions;                // VFS_PERMISSION_*
    uint8_t type;                       // VFS_TYPE_*
    uint32_t certificate;
    
    // Seconds since the Unix epoch, 0 = unknown (and always 0 for kernel
    // nodes). Only set when basefs has a clock (fs_set_time_source).
    uint32_t created;
    uint32_t modified;
} FSFileStats;

// Locking
//
// Serializes all VFS and mounted-filesystem access. Every vfs_* function
// below takes it internally, so ordinary callers never need it. Take it
// yourself only to:
//   - make several vfs_* calls atomic (e.g. get_item_count + get_item loop)
//   - call fs_* directly on a mounted device (formatting, mounting, tools)
//   - modify mount knodes outside the VFS
// Recursive per thread. Thread context only, never from an IRQ handler.
// If kernel_big_lock is also needed, take it first.
void vfs_lock(void);
void vfs_unlock(void);

// Paths
//
// Every path the VFS takes is absolute ("/mnt/ata0p1/usr"). Relative paths
// belong to whoever keeps a working directory (the console, later each
// process); this turns one into the other.
//
// vfs_path_normalize joins `path` onto `base` (unless `path` is already
// absolute) and folds "." and ".." as text: "/mnt/ata0/../ata1/./bin" ->
// "/mnt/ata1/bin". There are no links, so the textual parent is always the
// real parent, including out of a device root ("/mnt/ata0/.." -> "/mnt").
// ".." at "/" stays at "/". Repeated slashes collapse, and the result never
// ends in '/' (except "/" itself).
//
// It does not touch the disk, so the result need not exist. Returns false,
// leaving `out` undefined, if `base` is needed but not absolute or the result
// does not fit `size` (never truncated). `out` may be the same buffer as
// `base` or `path`.
bool vfs_path_normalize(const char* base, const char* path, char* out, size_t size);

// Device
uint64_t vfs_device_get_capacity(const char* path);
uint64_t vfs_device_get_used(const char* path);

// What the device holding `path` is. False if `path` is not on a mounted
// device (or the device has been detached, see vfs_device_release_disk).
#define VFS_DEVICE_NAME_MAX       16    // Including the terminator (BLOCK_NAME_MAX)

typedef struct {
    char     device[VFS_DEVICE_NAME_MAX];   // "ata0p1", "ahci1", ...
    uint32_t device_sector_size;            // Bytes per hardware sector
    uint64_t device_sectors;                // Hardware sectors on the device
    bool     formatted;                     // Holds a readable basefs volume
    uint32_t block_size;                    // basefs allocation block (formatted only)
    uint32_t total_size;                    // basefs pool size in bytes (formatted only)
} VfsDeviceInfo;

bool vfs_device_get_info(const char* path, VfsDeviceInfo* out);

// Formatting
//
// Both functions format the disk a mount actually sits on (taken from its
// own context, never a "current drive"), re-open the mount in place and
// refuse while any file on that disk is open. Everything on it is lost.

typedef enum {
    VFS_FORMAT_OK = 0,
    VFS_FORMAT_NOT_MOUNTED,     // No mounted device context
    VFS_FORMAT_BUSY,            // Files on the device are open
    VFS_FORMAT_BAD_SIZE,        // Capacity / block size invalid or larger than the disk
    VFS_FORMAT_IO_ERROR,        // A disk write failed or the result did not read back
} VfsFormatResult;

// Called with 1..100 during a full wipe. Runs with the VFS lock held: it
// may print, but must not touch the VFS or kernel_lock.
typedef void (*vfs_format_progress_fn)(uint32_t percent);

struct FSDeviceContext;

// Format a mount's context. capacity is in bytes (at most
// fs_device_max_capacity(ctx->device)), block_size is the basefs allocation
// block (a multiple of 4, at least FS_MIN_SECTOR_SIZE = 512). full_wipe
// zeroes the covered sectors first.
VfsFormatResult vfs_device_format_context(struct FSDeviceContext* ctx, uint32_t capacity,
                                          uint32_t block_size, bool full_wipe,
                                          vfs_format_progress_fn progress);

// Quick format of the device mounted at `path` (e.g. "/mnt/ata1"), using the
// whole disk and its native sector size as the block size
bool vfs_device_format(const char* path);

// Partition the whole disk mounted at `path`: write an MBR with one basefs
// partition from `first_sector` to the end of the disk, keeping any boot
// code already in sector 0. Everything on the disk is lost. The disk's
// mount is detached at once (it reports as not mounted); the partition
// appears as "<disk>p1" (e.g. /mnt/ata1p1) after a reboot, unformatted.
//
// first_sector: VFS_PARTITION_START_BOOT leaves room for the boot loader
// and kernel (build-disk.sh layout); VFS_PARTITION_START_DATA is the usual
// 1 MB alignment for a disk that will never boot.
#define VFS_PARTITION_START_BOOT    8192ULL     // = BOOT_RESERVED_SECTORS in build-disk.sh
#define VFS_PARTITION_START_DATA    2048ULL

VfsFormatResult vfs_device_partition(const char* path, uint64_t first_sector);

struct BlockDevice;

// Sync and detach every mount on `disk` and on its partitions, so the disk
// can be rewritten underneath them. VFS_FORMAT_BUSY (nothing detached) if a
// file on any of them is open. The mounts show as unformatted afterwards.
VfsFormatResult vfs_device_release_disk(struct BlockDevice* disk);

// The mount context of a block device ("ata1", "ata0p1"), or NULL if it is
// not mounted. Hand it to vfs_device_format_context; like that call's
// argument, it stays valid for as long as the mount exists.
struct FSDeviceContext* vfs_device_context(struct BlockDevice* device);

// Flush every mounted storage device (allocation bitmap, sector cache and
// the drive's write cache). Call before power-off to avoid corruption.
// Returns false if any device failed to flush.
bool vfs_sync_all(void);

// File
File vfs_open(const char* path, uint16_t flags);
void vfs_close(File file);

int32_t vfs_read(File file, void* buffer, uint32_t size);
int32_t vfs_write(File file, const void* buffer, uint32_t size);

uint32_t vfs_seek(File file, uint32_t position);
uint32_t vfs_tell(File file);

uint32_t vfs_get_size(File file);

// File system
bool vfs_mkdir(const char* path);
bool vfs_remove(const char* path);
bool vfs_rename(const char* path, const char* name);
bool vfs_exists(const char* path);
bool vfs_truncate(const char* path, uint32_t new_size);

// Copy a file, or a directory with everything inside it, to `dest` (the full
// new path, which must not exist yet). Permissions are carried over. Refuses
// mounted devices and copying a directory into itself. On failure nothing is
// left at `dest`. See vfs_copy.c.
bool vfs_copy(const char* src, const char* dest);

// Operations
bool vfs_stat(const char* path, FSFileStats* stats);

bool vfs_set_permissions(const char* path, uint8_t perm);
bool vfs_get_permissions(const char* path, uint8_t* perm);
bool vfs_directory_check(const char* path);
bool vfs_directory_check_mounted(const char* path);

// Cryptography
// Only files on a mounted storage device carry a certificate. Both return
// false for anything else. vfs_file_certify rejects 0 and 0xFFFFFFFF.
bool vfs_file_get_certificate(const char* path, uint32_t* certificate);
bool vfs_file_certify(const char* path, uint32_t certificate);

// Security clearance
//
// Every object on a mounted storage device carries a 32-bit security mask
// (FSBlockHeader.security): the clearance bits a caller must hold to open,
// list, change or remove it. The bits are the scheduler's SYS_SEC_* bits,
// so a process's ProcessSecurity mask is its clearance. VFS_SECURITY_ADMIN
// (== SYS_SEC_ADMIN, checked in syscall.c) marks administrator files; a
// user program (SYS_SEC_ADMIN clear) cannot touch them. 0 = anyone.
//
// The caller's clearance comes from the source set with
// vfs_set_clearance_source() (syscall_init installs
// process_current_clearance). Kernel threads, and everything before a
// source is set, have VFS_CLEARANCE_ALL, so kernel code is never refused.
//
// What is checked (object mask = the object's own, parent = its directory):
//   open, read, write, truncate, set permissions, certify    object
//   list a directory (item count / item)                     object
//   create a file or directory                               parent
//   rename                                                   object + parent
//   remove                                                   parent + object + everything below it
// stat, exists and get_permissions are always allowed, so a refused call
// can say "permission denied" instead of "not found". Kernel nodes (/, /dev,
// /mnt, ...) have no mask. New objects inherit their parent's mask.
//
// The mask is re-checked on every read and write, so raising a file's mask
// also stops handles that are already open.

#define VFS_SECURITY_NONE          0x00000000u
#define VFS_SECURITY_ADMIN         0x00000100u  // == SYS_SEC_ADMIN
#define VFS_CLEARANCE_ALL          0xFFFFFFFFu

typedef uint32_t (*vfs_clearance_fn)(void);

// Where the VFS asks for the calling thread's clearance. NULL: everyone has
// VFS_CLEARANCE_ALL. Called with the VFS lock held: it must not use the VFS.
void vfs_set_clearance_source(vfs_clearance_fn source);

// The calling thread's clearance
uint32_t vfs_caller_clearance(void);

// The object's mask. False if `path` is not an object on a mounted device.
bool vfs_get_security(const char* path, uint32_t* security);

// Replace the object's mask. The caller's clearance must cover both the old
// mask and the new one (nobody can grant or strip a bit they do not hold).
// Only affects that object; contents keep their own masks.
bool vfs_set_security(const char* path, uint32_t security);

// True unless the object at `path` (or, for the parent form, the directory
// holding it) exists and requires clearance the caller lacks. Used by the
// syscalls to report SYS_EACCES; every VFS operation also checks by itself.
bool vfs_check_access(const char* path);
bool vfs_check_parent_access(const char* path);

// Directory
//
// name_out receives the item's name, NUL-terminated. name_size is its
// size in bytes; VFS_NAME_MAX + 1 always fits. Returns false (with an
// empty name) if there is no such item or the name does not fit.
uint32_t vfs_directory_get_item_count(const char* path);
bool vfs_directory_get_item(const char* path, unsigned int index, char* name_out, size_t name_size);

#endif



