#ifndef _OS_DISK_H
#define _OS_DISK_H

/*
 * Formatting storage devices (SYS_FORMAT), and unmounting them before
 * they are pulled out (SYS_UNMOUNT, at the end).
 *
 *     disk_format_t f;
 *     disk_format_init(&f);
 *     f.block_size = 1024;                         // 0: keep the current one, else 512
 *     if (disk_format_query("usb0", &f) < 0) ...   // what it would do, nothing written
 *     printf("%llu bytes%s\n", f.capacity, (f.info & DISK_INFO_WHOLE_DISK) ? ", whole disk" : "");
 *
 *     if (sec_elevate("Erase usb0 and format it as basefs") == 0) {
 *         int r = disk_format("usb0", &f);         // blocks until done
 *         sec_drop(SEC_ADMIN);
 *     }
 *
 * device is a name as listed in /mnt ("ata1", "usb0", "ata0p1"), never a
 * path. A whole disk is wiped from sector 0, partition table and every
 * partition on it included, and gets one volume; a partition is formatted
 * inside its own range. Everything on it is lost.
 *
 * disk_format_query() needs SEC_FILE_READ. disk_format() also needs
 * SEC_ADMIN and SEC_FILE_WRITE (ask with sec_elevate() from os/security.h).
 * The device holding the system folders, and the disk it is on, cannot be
 * formatted from a program. One format runs at a time, system-wide.
 *
 * Both return 0 (the spec filled in with what was / would be done), or -1
 * with errno:
 *   EPERM    no SEC_FILE_READ; to format, no SEC_ADMIN or SEC_FILE_WRITE
 *   EINVAL   bad name or fields; a filesystem or block size the device
 *            can't take (DISK_BLOCK_MIN..DISK_BLOCK_MAX, a power of two)
 *   ENOSPC   capacity is more than fits: max_capacity is filled in
 *   ENOENT   no such device, or a partition that is not mounted
 *   EACCES   the system device: the spec is filled in (DISK_INFO_SYSTEM)
 *   EBUSY    files on the device are open, or another format is running
 *   EIO      the device failed
 */

#include <stdbool.h>
#include <stdint.h>
#include <os/syscall.h>

typedef SysFormatSpec disk_format_t;

#define DISK_FORMAT_QUICK       SYS_FORMAT_QUICK    /* flags: structures only */

#define DISK_INFO_WHOLE_DISK    SYS_FORMAT_INFO_WHOLE_DISK
#define DISK_INFO_FORMATTED     SYS_FORMAT_INFO_FORMATTED
#define DISK_INFO_SYSTEM        SYS_FORMAT_INFO_SYSTEM

#define DISK_BLOCK_MIN          SYS_FORMAT_BLOCK_MIN
#define DISK_BLOCK_MAX          SYS_FORMAT_BLOCK_MAX
#define DISK_FS_MAX             SYS_FORMAT_FS_MAX

/* Empty request: the default filesystem, sizes and a full format */
void disk_format_init(disk_format_t* spec);

/* Check the request and fill in what formatting would do. Writes nothing. */
int disk_format_query(const char* device, disk_format_t* spec);

/* Format the device as the spec asks (DISK_FORMAT_QUICK in flags for a
   quick format). Blocks until it is done and cannot be cancelled. */
int disk_format(const char* device, disk_format_t* spec);

/*
 * Get a drive ready to be pulled out: everything cached for it is written
 * out, the drive commits its own write cache, then it is unmounted and gone
 * from /mnt (until it is plugged in again, or the next boot). device is a
 * name as listed in /mnt ("usb0"); a partition ("usb0p1") unmounts the
 * whole disk it is on. Needs SEC_FILE_WRITE.
 *
 * Returns 0, or -1 with errno:
 *   EPERM    no SEC_FILE_WRITE
 *   EINVAL   bad name
 *   ENOENT   no such device, or nothing on it is mounted
 *   EACCES   the system drive, or the disk it is on
 *   EBUSY    a file on it is open, or a format is running
 *   EIO      a write failed: it stays mounted, its files are still there
 */
int disk_unmount(const char* device);

#endif
