#ifndef _KERNEL_FS_FAT_H_
#define _KERNEL_FS_FAT_H_

// FAT12/16/32 volumes through FatFs (fs/fatfs/source, ChaN, BSD-style
// license kept in its files). fstype.c is the FsType the VFS drives;
// fat_diskio.c is the glue FatFs calls back into. Nothing outside this
// directory includes FatFs's headers.

#include <stdint.h>
#include <stdbool.h>

struct BlockDevice;

// Bind a FatFs drive number to `device` (-1: all FF_VOLUMES are in use)
int  fat_drive_claim(struct BlockDevice* device);
void fat_drive_release(int drive);

// Sectors f_mkfs sees on the drive, so a volume can be smaller than its
// device. 0: the whole device.
void fat_drive_set_limit(int drive, uint32_t sectors);

// FAT packed date/time (date << 16 | time) from Unix seconds, and back
uint32_t fat_time_from_unix(uint32_t seconds);
uint32_t fat_time_to_unix(uint16_t date, uint16_t time);

#endif
