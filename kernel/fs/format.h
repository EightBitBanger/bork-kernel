#ifndef _KERNEL_FS_FORMAT_H_
#define _KERNEL_FS_FORMAT_H_

// Formatting a block device
//
// One implementation for the kernel console's `format` command and for
// SYS_FORMAT (user programs), so both make the same checks and wipe the same
// way.
//
// A whole disk ("ata1") is wiped from sector 0, boot sector, partition table
// and every partition on it included, and gets one whole-disk volume. A
// partition ("ata0p1") is formatted inside its own range only.
//
// Everything a request asks for is checked (format_plan) before anything is
// written: an unknown filesystem, a bad block size or a size larger than the
// device fails with the disk untouched. Only one format runs at a time,
// system-wide (FORMAT_IN_PROGRESS for the next).

#include <stdint.h>
#include <stdbool.h>

#include <kernel/block/block.h>
#include <kernel/vfs/vfs.h>

#define FORMAT_FS_NAME_MAX      VFS_FS_NAME_MAX     // Including the terminator

struct FormatPlan;

typedef struct {
    const char* filesystem;     // "basefs", "fat32"; NULL or "" for the one it holds now
    uint64_t    capacity;       // Volume bytes; 0 = keep the current size (a
                                // partition) or use the whole device
    uint32_t    block_size;     // Allocation block; 0 = keep the current one, else 512
    bool        quick;          // Write the filesystem structures only. A whole
                                // disk still has its boot area, its last sectors
                                // and the head of every partition zeroed, so
                                // nothing on it can be found or booted afterwards.
    bool        quiet;          // Print nothing on the kernel console (the
                                // remount of a wiped disk says what it found)
    bool        allow_system;   // May format the device holding the system
                                // folders (home), or the disk it is on. Only
                                // the kernel console sets this.

    // format_run calls this (may be NULL) once it holds the format lock and
    // has checked the plan again, right before the first write
    void (*started)(const struct FormatPlan* plan);
} FormatRequest;

typedef struct FormatPlan {
    char     device[BLOCK_NAME_MAX];            // As registered ("ata1")
    char     filesystem[FORMAT_FS_NAME_MAX];    // What it will be / was formatted as
    bool     whole_disk;                        // Wiped from sector 0, partitions and all
    bool     formatted;                         // Holds a readable volume now
    bool     system;                            // Holds home, or is the disk home is on
    uint32_t device_sector_size;
    uint64_t device_bytes;
    uint64_t capacity;                          // Volume bytes, a whole number of blocks
    uint64_t max_capacity;                      // Largest volume the filesystem fits there
    uint32_t block_size;
    bool     quick;                             // As requested
} FormatPlan;

typedef enum {
    FORMAT_OK = 0,
    FORMAT_NO_DEVICE,           // No block device of that name
    FORMAT_NOT_MOUNTED,         // A partition with no mount to format through
    FORMAT_BAD_FILESYSTEM,      // No filesystem of that name
    FORMAT_BAD_BLOCK_SIZE,      // The filesystem does not take that block size
    FORMAT_TOO_LARGE,           // capacity > max_capacity
    FORMAT_TOO_SMALL,           // Less than one block, or below the filesystem's minimum (fat32: about 33 MB)
    FORMAT_SYSTEM_DEVICE,       // Holds the system folders (allow_system not set)
    FORMAT_BUSY,                // Files on the device are open
    FORMAT_IN_PROGRESS,         // Another format is running
    FORMAT_WIPE_FAILED,         // Whole disk: wiping failed, the disk is in an unknown state
    FORMAT_IO_ERROR,            // Writing the volume failed or it did not read back
} FormatResult;

// What formatting `device_name` with `request` would do, into *plan. Writes
// nothing. FORMAT_OK when format_run would go ahead (barring open files).
FormatResult format_plan(const char* device_name, const FormatRequest* request, FormatPlan* plan);

// Format it. Plans again first, under the format lock, so nothing changed in
// between; *plan (may be NULL) gets the plan used. `progress` (may be NULL)
// is called with 0..100 during a full wipe, with no lock held for a whole
// disk and with the VFS lock held for a partition: it may print, but must
// not touch the VFS or kernel_lock. Runs for as long as the wipe takes.
FormatResult format_run(const char* device_name, const FormatRequest* request, FormatPlan* plan,
                        void (*progress)(uint32_t percent));

// The format lock, for other commands that rewrite disks under the VFS
// (fdisk, boot install), so they never run during a format or each other.
// format_run takes it itself. False: something else holds it.
bool format_lock_try(void);
void format_lock_release(void);

// One line for a result ("device busy: close open files on it first")
const char* format_result_text(FormatResult result);

#endif
