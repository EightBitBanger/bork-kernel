
#ifndef BASE_FILE_STRUCTS_H
#define BASE_FILE_STRUCTS_H

#include <kernel/fs/config.h>

#include <stdint.h>
#include <stdbool.h>

// ===========================================================================
// basefs on-disk format, version 2
//
// Changes from version 1:
//   - Names live in the directory, not in the file. A directory's payload is
//     a run of variable-length FSDirEntry records (name, type, address).
//     Headers no longer carry a name at all.
//   - Every header has a parent back-pointer and timestamps.
//   - A file's first data bytes are stored inline, right after its header,
//     in the same allocation. Small files need a single allocation; larger
//     ones spill into an FSFileExtent chain exactly as before.
//
// Version 1 images are not readable: fs_device_open must reject any
// partition whose `version` is not FS_FORMAT_VERSION.
// ===========================================================================

// The block device a filesystem lives on. basefs never looks inside it; it
// only passes it to block_read / block_write / block_flush (block/block.h).
struct BlockDevice;

// In memory only (unchanged)
struct FSDeviceContext {
    bool is_open;
    bool frame_dirty;
    
    struct BlockDevice* device;     // Where the sectors live
    uint32_t device_address;        // One-sector cache window (device sector size bytes)
    uint32_t sector_size;
    uint32_t pool_size;
    uint32_t block_count;
    uint32_t bitmap_size;
    uint32_t reserved_blocks;
    
    uint32_t frame_offset;
    uint8_t  frame_bitmap[BITMAP_FRAME_SIZE];
    
    uint32_t sector_frame;
    bool sector_dirty;
    
    uint32_t alloc_hint;            // Next-fit start block for fs_alloc (in memory only)
};

// ---------------------------------------------------------------------------
// Device and partition
// ---------------------------------------------------------------------------

struct __attribute__((packed)) FSDeviceHeader {
    uint8_t id;
    char name[10];
    uint8_t pad[5];
};

// Same size and field offsets as version 1 (whose name field was also 16
// bytes), so `magic` is found in the same place and an old image is
// recognized and refused by `version` instead of being misread.
struct __attribute__((packed)) FSPartitionBlock {
    uint32_t total_size;
    uint32_t sector_size;
    
    uint32_t root_directory;
    
    char name[FS_PARTITION_NAME_MAX];   // Volume label, NUL-padded
    
    uint8_t  flags;
    uint8_t  version;                   // FS_FORMAT_VERSION (was `type`, 0 on v1)
    
    uint8_t  reserved;
    uint8_t  magic;
};

// ---------------------------------------------------------------------------
// Allocations and extent links
// ---------------------------------------------------------------------------

// Precedes every allocation. Payload addresses point just past it.
struct __attribute__((packed)) FSAllocHeader {
    uint32_t size;
};

struct __attribute__((packed)) FSExtent {
    uint32_t next;
    uint32_t prev;
};

// ---------------------------------------------------------------------------
// Headers (the "inode": everything about an object except its name)
// ---------------------------------------------------------------------------

// Shared first part of FSFileHeader and FSDirectoryHeader. Code that only
// needs attributes, permissions or security can read just this.
struct __attribute__((packed)) FSBlockHeader {
    uint8_t  attributes;            // FS_ATTRIBUTE_*
    uint8_t  permissions;           // FS_PERMISSION_*
    
    // Bumped every time a file's extent chain is truncated. Open handles
    // cache their position in the chain and use this to notice that the
    // cached extent may have been freed. Unused (0) on directories.
    uint16_t chain_generation;
    
    // Directory holding this object's entry; FS_NULL for the partition
    // root. Lets fsck rebuild links and lets the VFS build a path from an
    // address. Kept in sync by create, move and directory delete.
    uint32_t parent;
    
    uint32_t security;
    uint32_t certificate;
    
    // Seconds since the Unix epoch, 0 = unknown (no clock at the time)
    uint32_t created;
    uint32_t modified;
    
    uint8_t  reserved[8];           // Free for future use, zero on format
};

// File header. The allocation holding it continues with file data:
//
//   [FSAllocHeader][FSFileHeader][inline data ...][slack to end of block]
//
// File offsets [0, inline_size) live inline. Everything past that is in
// the FSFileExtent chain starting at extent.next. The inline part can use
// everything up to the end of the allocation's last block (as extents do),
// and grows in place with fs_alloc_resize when the next blocks are free.
// Only the LAST part of a file ever grows: once a file has overflow
// extents, inline_size no longer changes until a truncate removes them.
struct __attribute__((packed)) FSFileHeader {
    struct FSBlockHeader block;
    uint32_t             total_logical_size;
    uint32_t             inline_size;   // File bytes held after the header
    
    // next: first overflow extent (FS_NULL = none).
    // prev: hint naming the LAST overflow extent (FS_NULL = unknown or
    // none). Validated before use, as in version 1.
    struct FSExtent      extent;
};

// Overflow data extent (unchanged from version 1). extent_data_size is the
// number of file bytes held in this extent. The allocation behind it may be
// larger (sector rounding and preallocation); that spare room is used when
// the file grows, before new extents are made. The first overflow extent's
// prev names the file header.
struct __attribute__((packed)) FSFileExtent {
    struct FSExtent extent;
    uint32_t        extent_data_size;
};

// ---------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------

// Directory header. Its allocation continues with entry records:
//
//   [FSAllocHeader][FSDirectoryHeader][FSDirEntry ...........]
//
// and each FSDirectoryExtent in the chain likewise holds records after its
// own small header. A record area runs from just after its header to the
// end of the allocation (payload + FSAllocHeader.size; directory
// allocations are always sized to whole blocks). The records tile the area
// exactly: the rec_len values add up to its end, so a walk never needs a
// count to know where to stop.
//
// The counts below are bookkeeping for fast indexing; the records are the
// truth. A crash between writing a record and its count can leave a count
// off by one, which only affects index-based listing.
struct __attribute__((packed)) FSDirectoryHeader {
    struct FSBlockHeader block;     // block.parent is the parent directory
    
    struct FSExtent extent;         // next: first extent, prev: last (hint)
    
    uint32_t entry_count;           // Live entries in the WHOLE directory
    uint32_t inline_entries;        // Live entries in this header's own area
};

struct __attribute__((packed)) FSDirectoryExtent {
    struct FSExtent extent;
    uint32_t        entry_count;    // Live entries in this extent
    uint32_t        reserved;
};

// One directory entry, followed by name_len bytes of name (NOT
// NUL-terminated on disk; never contains '/' or NUL).
//
// Rules, as in ext2:
//   - rec_len is a multiple of FS_DIR_ENTRY_ALIGN and at least
//     FS_DIR_ENTRY_SIZE(name_len). Any extra is free space that a new
//     entry can be carved out of.
//   - Deleting an entry adds its rec_len to the previous record in the
//     same device sector. A record that starts a sector (or the area) has
//     no such neighbor and is instead marked free: address = FS_NULL,
//     name_len = 0, rec_len kept.
//   - A record never crosses a device sector boundary (absolute address).
//     The record that would reach one is padded up to it instead. Each
//     entry is therefore written by exactly one sector write, so a crash
//     can never leave half a name.
struct __attribute__((packed)) FSDirEntry {
    uint32_t address;               // Child's header, FS_NULL = free record
    uint16_t rec_len;               // Bytes from this record to the next
    uint8_t  name_len;              // 1..FS_NAME_MAX (0 only when free)
    uint8_t  type;                  // FS_ENTRY_TYPE_*
    // char name[name_len];
};

#define FS_ENTRY_TYPE_NONE          0x00
#define FS_ENTRY_TYPE_FILE          0x01
#define FS_ENTRY_TYPE_DIRECTORY     0x02

#define FS_DIR_ENTRY_ALIGN          4U

// Bytes a record needs for a name of `len` characters
#define FS_DIR_ENTRY_SIZE(len) \
    ((uint32_t)((sizeof(struct FSDirEntry) + (uint32_t)(len) + FS_DIR_ENTRY_ALIGN - 1U) & ~(FS_DIR_ENTRY_ALIGN - 1U)))

#define FS_DIR_ENTRY_MAX_SIZE       FS_DIR_ENTRY_SIZE(FS_NAME_MAX)

// ---------------------------------------------------------------------------
// Layout checks
// ---------------------------------------------------------------------------

_Static_assert(sizeof(struct FSPartitionBlock)  == 32, "FSPartitionBlock on-disk size changed");
_Static_assert(sizeof(struct FSBlockHeader)     == 32, "FSBlockHeader on-disk size changed");
_Static_assert(sizeof(struct FSFileHeader)      == 48, "FSFileHeader on-disk size changed");
_Static_assert(sizeof(struct FSFileExtent)      == 12, "FSFileExtent on-disk size changed");
_Static_assert(sizeof(struct FSDirectoryHeader) == 48, "FSDirectoryHeader on-disk size changed");
_Static_assert(sizeof(struct FSDirectoryExtent) == 16, "FSDirectoryExtent on-disk size changed");
_Static_assert(sizeof(struct FSDirEntry)        ==  8, "FSDirEntry on-disk size changed");

// Header + alloc header keep records 4-byte aligned
_Static_assert(((sizeof(struct FSAllocHeader) + sizeof(struct FSDirectoryHeader)) % FS_DIR_ENTRY_ALIGN) == 0,
               "Directory records would be misaligned");
_Static_assert(((sizeof(struct FSAllocHeader) + sizeof(struct FSDirectoryExtent)) % FS_DIR_ENTRY_ALIGN) == 0,
               "Directory extent records would be misaligned");

// rec_len is 16 bits and name_len 8 bits
_Static_assert(FS_NAME_MAX <= 255, "FS_NAME_MAX must fit in FSDirEntry.name_len");

#endif
