
#ifndef BASE_FILE_SYSTEM_CONFIGURATION_H
#define BASE_FILE_SYSTEM_CONFIGURATION_H

// On-disk format version, stored in FSPartitionBlock.version.
// fs_device_open refuses any other value (version 1 images had 0 there).
#define FS_FORMAT_VERSION           2

// Longest file or directory name in bytes, without terminator. Names live
// in directory entries (FSDirEntry.name_len is 8 bits, so at most 255).
// Path buffers in the VFS must be able to hold several names this long.
#ifndef FS_NAME_MAX
#define FS_NAME_MAX                 255
#endif

// Volume label in the partition block. Fixed: changing it moves the
// partition block's fields and breaks every image.
#define FS_PARTITION_NAME_MAX       16

// Smallest block (allocation) size fs_device_format accepts. A directory
// record must fit in one device sector, and a header plus at least one
// record must fit in one block.
#define FS_MIN_SECTOR_SIZE          512

#define BITMAP_FRAME_SIZE   32

// When a file grows past its last extent, the new extent (or the in-place
// extension of the last one) reserves up to file_size / 8 extra bytes,
// capped here, so a run of small appends does not allocate on every write.
// The spare room stays with the file; 0 disables preallocation.
#ifndef FS_FILE_PREALLOC_MAX
#define FS_FILE_PREALLOC_MAX (256UL * 1024UL)
#endif

// 1: aligned whole-sector runs in fs_mem_read/fs_mem_write go straight
// between the caller's buffer and the block device, several sectors per
// request. Only enable this if block_read/block_write accept ANY kernel
// buffer (e.g. the driver bounces or the kernel is identity mapped and the
// buffers are DMA-reachable). 0: everything goes through the sector window.
#ifndef FS_IO_DIRECT_TRANSFER
#define FS_IO_DIRECT_TRANSFER 0
#endif
#ifndef FS_IO_MAX_BURST_SECTORS
#define FS_IO_MAX_BURST_SECTORS 128
#endif

#endif
