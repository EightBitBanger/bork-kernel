
#include <kernel/fs/fs.h>
#include <kernel/fs/basefs/file.h>
#include <kernel/fs/config.h>

#include <kernel/util/string.h>

// ---------------------------------------------------------------------------
// Layout (see FSFileHeader in structs.h)
//
//   header allocation:  [FSAllocHeader][FSFileHeader][inline data][slack]
//   overflow extents:   [FSAllocHeader][FSFileExtent][data][slack] -> ...
//
// The inline data is "part 0" of the file and the extents follow it in
// order. Only the last part ever grows, so once a file has extents its
// inline part keeps its size until a truncate removes the extents.
// ---------------------------------------------------------------------------

static inline uint32_t fs_file_part_header_size(uint32_t part, uint32_t file_address) {
    return (part == file_address) ? (uint32_t)sizeof(struct FSFileHeader) : (uint32_t)sizeof(struct FSFileExtent);
}

bool fs_file_check(struct FSDeviceContext* ctx, uint32_t address) {
    if (!ctx) return false;
    struct FSAllocHeader alloc;
    struct FSBlockHeader block;
    if (address == FS_NULL || address < sizeof(struct FSAllocHeader))
        return false;
    fs_mem_read(ctx, address - sizeof(struct FSAllocHeader), &alloc, sizeof(struct FSAllocHeader));
    if (alloc.size < sizeof(struct FSFileHeader) || alloc.size > ctx->pool_size)
        return false;
    fs_mem_read(ctx, address, &block, sizeof(struct FSBlockHeader));

    if (block.attributes & FS_ATTRIBUTE_DIRECTORY)
        return false;
    return true;
}

static bool fs_file_struct_is_valid(const FileHandle* file) {
    if (file == NULL || file->ctx == NULL)
        return false;
    if (!file->is_open)
        return false;
    if (!fs_file_check(file->ctx, file->address))
        return false;
    return true;
}

static bool fs_file_extent_read(struct FSDeviceContext* ctx, uint32_t extent_address, struct FSFileExtent* extent) {
    struct FSAllocHeader alloc_header;
    if (extent_address == FS_NULL || extent_address < sizeof(struct FSAllocHeader)) return false;
    fs_mem_read(ctx, extent_address - sizeof(struct FSAllocHeader), &alloc_header, sizeof(struct FSAllocHeader));
    if (alloc_header.size < sizeof(struct FSFileExtent))
        return false;
    fs_mem_read(ctx, extent_address, extent, sizeof(struct FSFileExtent));
    return true;
}

static void fs_file_extent_write(struct FSDeviceContext* ctx, uint32_t extent_address, const struct FSFileExtent* extent) {
    fs_mem_write(ctx, extent_address, extent, sizeof(struct FSFileExtent));
}

static inline uint32_t fs_file_walk_limit(struct FSDeviceContext* ctx) {
    return (ctx && ctx->block_count) ? ctx->block_count : 0xFFFFU;
}

// Free every extent in the chain starting at `extent_address`. The walk is
// bounded by the device's block count so a corrupt (cyclic) chain cannot
// loop forever.
static void fs_file_free_chain(struct FSDeviceContext* ctx, uint32_t extent_address) {
    uint32_t guard = fs_file_walk_limit(ctx);
    while (extent_address != FS_NULL && guard-- > 0) {
        struct FSFileExtent extent;
        if (!fs_file_extent_read(ctx, extent_address, &extent))
            break;
        uint32_t next = extent.extent.next;
        fs_free(ctx, extent_address);
        extent_address = next;
    }
}

// ---------------------------------------------------------------------------
// Create / delete
// ---------------------------------------------------------------------------

static bool fs_file_grow(struct FSDeviceContext* ctx, uint32_t address, struct FSFileHeader* file,
                         uint32_t new_size, uint32_t start_hint);

uint32_t fs_file_create(struct FSDeviceContext* ctx, const char* name, uint8_t permissions, uint32_t size, uint32_t parent_directory) {
    struct FSFileHeader file;
    uint32_t            address;

    if (!ctx)
        return FS_NULL;
    if (parent_directory != FS_NULL) {
        if (name == NULL)
            return FS_NULL;
        // Don't build (and zero) a large file only to find the name taken
        if (size > 0 && fs_directory_find(ctx, parent_directory, name) != FS_NULL)
            return FS_NULL;
    }

    address = fs_alloc(ctx, sizeof(struct FSFileHeader));
    if (address == FS_NULL)
        return FS_NULL;

    // Build the file completely (header, then data) BEFORE linking it into
    // the parent. A failure or crash part way through then leaves at worst
    // an orphaned allocation, never a directory entry pointing at garbage
    // or at freed storage.
    memset(&file, 0x00, sizeof(struct FSFileHeader));
    file.block.attributes  = 0;
    file.block.permissions = permissions;
    file.block.parent      = parent_directory;
    file.block.created     = fs_time_now();
    file.block.modified    = file.block.created;
    file.total_logical_size = 0;
    file.inline_size        = 0;
    file.extent.next = FS_NULL;
    file.extent.prev = FS_NULL;

    fs_mem_write(ctx, address, &file, sizeof(struct FSFileHeader));
    if (size > 0 && !fs_file_grow(ctx, address, &file, size, FS_NULL)) {
        // fs_file_grow rolls back its own extents on failure
        fs_file_delete(ctx, address);
        return FS_NULL;
    }

    if (parent_directory != FS_NULL &&
        fs_directory_add_entry(ctx, parent_directory, name, address, FS_ENTRY_TYPE_FILE) != FS_DIR_OK) {
        fs_file_delete(ctx, address);
        return FS_NULL;
    }

    return address;
}

bool fs_file_delete(struct FSDeviceContext* ctx, uint32_t address) {
    struct FSFileHeader file;
    if (!fs_file_check(ctx, address))
        return false;

    fs_mem_read(ctx, address, &file, sizeof(struct FSFileHeader));
    fs_file_free_chain(ctx, file.extent.next);
    fs_free(ctx, address);
    return true;
}

uint32_t fs_file_get_size(struct FSDeviceContext* ctx, uint32_t address) {
    struct FSFileHeader file;
    if (!fs_file_check(ctx, address))
        return 0;
    fs_mem_read(ctx, address, &file, sizeof(struct FSFileHeader));
    return file.total_logical_size;
}

// ---------------------------------------------------------------------------
// Shared header fields (files and directories)
// ---------------------------------------------------------------------------

static bool fs_block_read(struct FSDeviceContext* ctx, uint32_t address, struct FSBlockHeader* block) {
    if (!ctx || address == FS_NULL)
        return false;
    fs_mem_read(ctx, address, block, sizeof(struct FSBlockHeader));
    return true;
}

static void fs_block_write(struct FSDeviceContext* ctx, uint32_t address, const struct FSBlockHeader* block) {
    fs_mem_write(ctx, address, block, sizeof(struct FSBlockHeader));
}

bool fs_file_get_permissions(struct FSDeviceContext* ctx, uint32_t address, uint8_t* permissions) {
    struct FSBlockHeader block;
    if (permissions == NULL || !fs_block_read(ctx, address, &block))
        return false;
    *permissions = block.permissions;
    return true;
}

bool fs_file_set_permissions(struct FSDeviceContext* ctx, uint32_t address, uint8_t permissions) {
    struct FSBlockHeader block;
    if (!fs_block_read(ctx, address, &block))
        return false;
    block.permissions = permissions;
    fs_block_write(ctx, address, &block);
    return true;
}

bool fs_file_get_attributes(struct FSDeviceContext* ctx, uint32_t address, uint8_t* attributes) {
    struct FSBlockHeader block;
    if (attributes == NULL || !fs_block_read(ctx, address, &block))
        return false;
    *attributes = block.attributes;
    return true;
}

bool fs_file_set_attributes(struct FSDeviceContext* ctx, uint32_t address, uint8_t attributes) {
    struct FSBlockHeader block;
    if (!fs_block_read(ctx, address, &block))
        return false;
    block.attributes = attributes;
    fs_block_write(ctx, address, &block);
    return true;
}

bool fs_file_get_times(struct FSDeviceContext* ctx, uint32_t address, uint32_t* created, uint32_t* modified) {
    struct FSBlockHeader block;
    if (!fs_block_read(ctx, address, &block))
        return false;
    if (created)  *created  = block.created;
    if (modified) *modified = block.modified;
    return true;
}

uint32_t fs_file_get_parent(struct FSDeviceContext* ctx, uint32_t address) {
    struct FSBlockHeader block;
    if (!fs_block_read(ctx, address, &block))
        return FS_NULL;
    return block.parent;
}

// ---------------------------------------------------------------------------
// Part helpers
// ---------------------------------------------------------------------------

// Data bytes the allocation behind a part (header or extent) can hold. That
// is everything up to the end of its last block, not just what was asked of
// fs_alloc, so the block-rounding slack is usable too.
static uint32_t fs_file_part_capacity(struct FSDeviceContext* ctx, uint32_t part, uint32_t header_size, uint32_t* alloc_size) {
    struct FSAllocHeader alloc;
    fs_mem_read(ctx, part - sizeof(struct FSAllocHeader), &alloc, sizeof(struct FSAllocHeader));
    if (alloc_size) *alloc_size = alloc.size;
    if (alloc.size < header_size || ctx->sector_size == 0)
        return 0;
    uint64_t ss     = ctx->sector_size;
    uint64_t blocks = ((uint64_t)sizeof(struct FSAllocHeader) + alloc.size + ss - 1U) / ss;
    uint64_t room   = blocks * ss - sizeof(struct FSAllocHeader) - header_size;
    return (room > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)room;
}

// Extra room to reserve when a file of `size` bytes has to allocate
static inline uint32_t fs_file_prealloc_for(uint32_t size) {
    uint32_t extra = size / 8U;
    return (extra > FS_FILE_PREALLOC_MAX) ? (uint32_t)FS_FILE_PREALLOC_MAX : extra;
}

// The header's tail hint (extent.prev), if it still names the last extent
// of this file's chain. FS_NULL means "unknown, walk the chain".
static uint32_t fs_file_tail_from_hint(struct FSDeviceContext* ctx, uint32_t address,
                                       const struct FSFileHeader* file, struct FSFileExtent* tail) {
    uint32_t hint = file->extent.prev;
    if (hint == FS_NULL || hint == address || file->extent.next == FS_NULL)
        return FS_NULL;
    if (!fs_file_extent_read(ctx, hint, tail) || tail->extent.next != FS_NULL)
        return FS_NULL;
    if (tail->extent.prev == address)
        return (file->extent.next == hint) ? hint : FS_NULL;

    struct FSFileExtent before;
    if (!fs_file_extent_read(ctx, tail->extent.prev, &before) || before.extent.next != hint)
        return FS_NULL;
    return hint;
}

// Last overflow extent of a file that has at least one.
// `start_hint` (may be FS_NULL) is any extent known to be in the chain.
static bool fs_file_find_tail(struct FSDeviceContext* ctx, uint32_t address, const struct FSFileHeader* file,
                              uint32_t start_hint, uint32_t* tail_out, struct FSFileExtent* tail) {
    *tail_out = FS_NULL;
    if (file->extent.next == FS_NULL)
        return false;

    uint32_t found = fs_file_tail_from_hint(ctx, address, file, tail);
    if (found != FS_NULL) {
        *tail_out = found;
        return true;
    }

    uint32_t current = start_hint;
    if (current == FS_NULL || current == address || !fs_file_extent_read(ctx, current, tail)) {
        current = file->extent.next;
        if (!fs_file_extent_read(ctx, current, tail))
            return false;
    }

    uint32_t guard = fs_file_walk_limit(ctx);
    while (tail->extent.next != FS_NULL) {
        if (guard-- == 0)
            return false;
        current = tail->extent.next;
        if (!fs_file_extent_read(ctx, current, tail))
            return false;
    }
    *tail_out = current;
    return true;
}

// Move the handle's cursor to the part holding byte `position` (which must
// be below the file size). Uses the inline part directly, starts from the
// cached extent when it can, and walks backwards through `prev` links for
// short backward seeks, so sequential I/O never walks the chain from the
// head again.
//
// On return file->cur_extent / cur_start / cur_size describe that part.
static bool fs_file_locate(FileHandle* file, const struct FSFileHeader* header, uint32_t position) {
    struct FSDeviceContext* ctx = file->ctx;
    struct FSFileExtent ext_buf;
    struct FSFileExtent* ext = &ext_buf;
    bool cursor_current = file->cur_extent != FS_NULL && file->cur_extent != 0 &&
                          file->cur_generation == header->block.chain_generation;

    // Still inside the cached part: no I/O at all. Without a truncate
    // (which bumps the generation) a part never shrinks or moves, so the
    // remembered size is a safe lower bound.
    if (cursor_current && position >= file->cur_start && position - file->cur_start < file->cur_size)
        return true;

    // Inline data
    if (position < header->inline_size) {
        file->cur_extent     = file->address;
        file->cur_start      = 0;
        file->cur_size       = header->inline_size;
        file->cur_generation = header->block.chain_generation;
        return true;
    }

    uint32_t guard   = fs_file_walk_limit(ctx);
    uint32_t current = FS_NULL;
    uint32_t start   = 0;
    bool     cached  = false;

    if (cursor_current && file->cur_extent != file->address &&
        fs_file_extent_read(ctx, file->cur_extent, ext)) {
        current = file->cur_extent;
        start   = file->cur_start;
        cached  = true;

        if (position < start) {
            if (position - header->inline_size < (start - header->inline_size) / 2U) {
                cached = false;     // Nearer the head: restart from there
            } else {
                while (position < start) {
                    uint32_t prev = ext->extent.prev;
                    struct FSFileExtent before;
                    if (guard-- == 0 || prev == FS_NULL || prev == file->address ||
                        !fs_file_extent_read(ctx, prev, &before) ||
                        before.extent.next != current ||
                        before.extent_data_size > start - header->inline_size) {
                        cached = false;     // Broken back link: use the head
                        break;
                    }
                    start  -= before.extent_data_size;
                    current = prev;
                    *ext    = before;
                }
            }
        }
    }

    if (!cached) {
        current = header->extent.next;
        start   = header->inline_size;
        if (!fs_file_extent_read(ctx, current, ext)) {
            file->cur_extent = FS_NULL;
            return false;
        }
    }

    // Here start <= position
    while (position - start >= ext->extent_data_size) {
        if (ext->extent.next == FS_NULL || guard-- == 0) {
            file->cur_extent = FS_NULL;
            return false;
        }
        start  += ext->extent_data_size;
        current = ext->extent.next;
        if (!fs_file_extent_read(ctx, current, ext)) {
            file->cur_extent = FS_NULL;
            return false;
        }
    }

    file->cur_extent     = current;
    file->cur_start      = start;
    file->cur_size       = ext->extent_data_size;
    file->cur_generation = header->block.chain_generation;
    return true;
}

static bool fs_file_handle_header(const FileHandle* file, struct FSFileHeader* header) {
    if (!fs_file_struct_is_valid(file))
        return false;
    fs_mem_read(file->ctx, file->address, header, sizeof(struct FSFileHeader));
    return true;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

// Grow `file` (header at `address`, already read into *file) to new_size.
// The last part of the file (the inline data, or the last extent) takes the
// new bytes first:
//   1. spare room already inside its allocation,
//   2. extending its allocation in place (blocks right after it are free),
// then
//   3. new extents, as large as the free space allows, with the whole
//      remainder in one extent when possible.
// New extents are built detached and linked only once everything has been
// allocated; on failure everything is rolled back and the file is unchanged.
static bool fs_file_grow(struct FSDeviceContext* ctx, uint32_t address, struct FSFileHeader* file,
                         uint32_t new_size, uint32_t start_hint) {
    uint32_t growth  = new_size - file->total_logical_size;
    uint32_t reserve = fs_file_prealloc_for(new_size);

    // The last part: the header's inline data, or the last extent
    bool     tail_is_header = (file->extent.next == FS_NULL);
    uint32_t tail = address;
    struct FSFileExtent tail_ext;
    uint32_t tail_data;
    if (tail_is_header) {
        tail_data = file->inline_size;
    } else {
        if (!fs_file_find_tail(ctx, address, file, start_hint, &tail, &tail_ext))
            return false;
        tail_data = tail_ext.extent_data_size;
    }
    uint32_t tail_header = fs_file_part_header_size(tail, address);

    // 1 + 2: room in (or right after) the last part
    uint32_t tail_alloc_before = 0;
    bool     tail_extended = false;
    uint32_t capacity = fs_file_part_capacity(ctx, tail, tail_header, &tail_alloc_before);
    if (capacity < tail_data)
        capacity = tail_data;
    uint32_t room = capacity - tail_data;

    if (room < growth) {
        uint64_t want = (uint64_t)tail_header + tail_data + growth;
        if (want + reserve <= 0xFFFFFFFFULL && fs_alloc_resize(ctx, tail, (uint32_t)(want + reserve)))
            tail_extended = true;
        else if (want <= 0xFFFFFFFFULL && fs_alloc_resize(ctx, tail, (uint32_t)want))
            tail_extended = true;

        if (tail_extended) {
            capacity = fs_file_part_capacity(ctx, tail, tail_header, NULL);
            room = capacity - tail_data;
        }
    }
    uint32_t take = (room < growth) ? room : growth;

    // 3: new extents for whatever is left
    uint32_t remaining = growth - take;
    uint32_t first_new = FS_NULL;
    uint32_t last_new  = FS_NULL;
    struct FSFileExtent last_new_ext;
    uint32_t min_chunk = (ctx->sector_size > 16U) ? ctx->sector_size - 16U : 1U;
    uint32_t max_chunk = 0xFFFFFFFFU - sizeof(struct FSFileExtent);

    while (remaining > 0) {
        uint32_t chunk    = (remaining > max_chunk) ? max_chunk : remaining;
        uint32_t new_addr = FS_NULL;

        // Whole remainder plus preallocation in a single extent if possible
        if (reserve > 0 && chunk <= max_chunk - reserve)
            new_addr = fs_alloc(ctx, sizeof(struct FSFileExtent) + chunk + reserve);

        // Otherwise the largest piece the free space allows
        while (new_addr == FS_NULL) {
            new_addr = fs_alloc(ctx, sizeof(struct FSFileExtent) + chunk);
            if (new_addr != FS_NULL || chunk <= min_chunk)
                break;
            chunk /= 2U;
            if (chunk < min_chunk)
                chunk = min_chunk;
        }

        if (new_addr == FS_NULL) {
            fs_file_free_chain(ctx, first_new);
            if (tail_extended)
                fs_alloc_resize(ctx, tail, tail_alloc_before);
            return false;
        }

        uint32_t new_capacity = fs_file_part_capacity(ctx, new_addr, sizeof(struct FSFileExtent), NULL);
        uint32_t data         = (new_capacity < remaining) ? new_capacity : remaining;

        struct FSFileExtent new_ext;
        memset(&new_ext, 0x00, sizeof(struct FSFileExtent));
        // The first new extent's prev already names the file's current last
        // part (an extent, or the header), which is where it will be linked
        new_ext.extent.prev = (last_new != FS_NULL) ? last_new : tail;
        new_ext.extent.next = FS_NULL;
        new_ext.extent_data_size = data;
        fs_file_extent_write(ctx, new_addr, &new_ext);
        fs_mem_zero(ctx, new_addr + sizeof(struct FSFileExtent), data);

        if (last_new == FS_NULL) {
            first_new = new_addr;
        } else {
            last_new_ext.extent.next = new_addr;
            fs_file_extent_write(ctx, last_new, &last_new_ext);
        }
        last_new     = new_addr;
        last_new_ext = new_ext;
        remaining   -= data;
    }

    // Everything allocated: expose the last part's new bytes, link the chain
    if (take > 0)
        fs_mem_zero(ctx, tail + tail_header + tail_data, take);

    if (tail_is_header) {
        file->inline_size += take;
        if (first_new != FS_NULL)
            file->extent.next = first_new;
    } else {
        tail_ext.extent_data_size += take;
        if (first_new != FS_NULL)
            tail_ext.extent.next = first_new;
        fs_file_extent_write(ctx, tail, &tail_ext);
    }

    // Tail hint: the last extent, or none while the file is all inline
    file->extent.prev        = (last_new != FS_NULL) ? last_new : (tail_is_header ? FS_NULL : tail);
    file->total_logical_size = new_size;
    file->block.modified     = fs_time_now();
    fs_mem_write(ctx, address, file, sizeof(struct FSFileHeader));
    return true;
}

static bool fs_file_shrink(struct FSDeviceContext* ctx, uint32_t address, struct FSFileHeader* file,
                           uint32_t new_size) {
    uint32_t cut         = FS_NULL;     // First extent to free
    uint32_t new_tail    = FS_NULL;     // FS_NULL = no extents left
    bool     trim_header = false;

    if (new_size <= file->inline_size) {
        // The new end is in the inline data: every extent goes
        cut                = file->extent.next;
        file->extent.next  = FS_NULL;
        file->inline_size  = new_size;
        trim_header        = true;
    } else {
        uint32_t accumulated = file->inline_size;
        uint32_t current     = file->extent.next;
        uint32_t previous    = FS_NULL;     // FS_NULL = the header
        uint32_t guard       = fs_file_walk_limit(ctx);

        while (current != FS_NULL && guard-- > 0) {
            struct FSFileExtent ext;
            if (!fs_file_extent_read(ctx, current, &ext))
                break;

            if (ext.extent_data_size >= new_size - accumulated) {
                uint32_t keep = new_size - accumulated;
                if (keep == 0) {
                    // Drop this extent and everything after it
                    cut = current;
                    if (previous == FS_NULL) {
                        file->extent.next = FS_NULL;
                    } else {
                        struct FSFileExtent before;
                        if (fs_file_extent_read(ctx, previous, &before)) {
                            before.extent.next = FS_NULL;
                            fs_file_extent_write(ctx, previous, &before);
                        }
                    }
                    new_tail = previous;
                } else {
                    // New end falls inside this extent
                    cut = ext.extent.next;
                    ext.extent.next      = FS_NULL;
                    ext.extent_data_size = keep;
                    fs_file_extent_write(ctx, current, &ext);
                    // Hand back whole blocks (and any preallocation) past the end
                    fs_alloc_resize(ctx, current, sizeof(struct FSFileExtent) + keep);
                    new_tail = current;
                }
                break;
            }

            accumulated += ext.extent_data_size;
            previous     = current;
            new_tail     = current;
            current      = ext.extent.next;
        }
    }

    // Open handles may hold a cursor into the part that is about to go
    file->block.chain_generation++;
    file->block.modified     = fs_time_now();
    file->extent.prev        = new_tail;
    file->total_logical_size = new_size;
    fs_mem_write(ctx, address, file, sizeof(struct FSFileHeader));

    // Hand back whole blocks past the new end of the inline data
    if (trim_header)
        fs_alloc_resize(ctx, address, sizeof(struct FSFileHeader) + new_size);

    fs_file_free_chain(ctx, cut);
    return true;
}

bool fs_file_resize(struct FSDeviceContext* ctx, uint32_t address, uint32_t new_size) {
    if (!ctx) return false;
    struct FSFileHeader file;
    if (!fs_file_check(ctx, address))
        return false;

    fs_mem_read(ctx, address, &file, sizeof(struct FSFileHeader));
    if (file.total_logical_size == new_size)
        return true;
    if (new_size < file.total_logical_size)
        return fs_file_shrink(ctx, address, &file, new_size);
    return fs_file_grow(ctx, address, &file, new_size, FS_NULL);
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

bool fs_file_open(FileHandle* file, struct FSDeviceContext* ctx, uint32_t address, uint8_t mode) {
    struct FSFileHeader header;

    if (file == NULL)
        return false;
    memset(file, 0x00, sizeof(FileHandle));
    file->address    = FS_NULL;
    file->cur_extent = FS_NULL;
    if (!fs_file_check(ctx, address))
        return false;

    fs_mem_read(ctx, address, &header, sizeof(struct FSFileHeader));
    if ((mode & FS_FILE_MODE_READ) && !(header.block.permissions & FS_PERMISSION_READ))
        return false;
    if ((mode & FS_FILE_MODE_WRITE) && !(header.block.permissions & FS_PERMISSION_WRITE))
        return false;
    file->ctx     = ctx;
    file->address = address;
    file->mode    = mode;
    file->is_open = 1;
    file->position = (mode & FS_FILE_MODE_APPEND) ? header.total_logical_size : 0;
    return true;
}

void fs_file_close(FileHandle* file) {
    if (file == NULL)
        return;

    // Stamp the modification time once per open instead of on every write
    if (file->is_open && file->written && fs_file_check(file->ctx, file->address)) {
        struct FSBlockHeader block;
        fs_mem_read(file->ctx, file->address, &block, sizeof(struct FSBlockHeader));
        block.modified = fs_time_now();
        fs_mem_write(file->ctx, file->address, &block, sizeof(struct FSBlockHeader));
    }

    file->ctx            = NULL;
    file->address        = FS_NULL;
    file->position       = 0;
    file->mode           = 0;
    file->is_open        = 0;
    file->written        = 0;
    file->cur_extent     = FS_NULL;
    file->cur_start      = 0;
    file->cur_size       = 0;
    file->cur_generation = 0;
}

static inline uint32_t fs_file_cursor_data(const FileHandle* file) {
    return file->cur_extent + fs_file_part_header_size(file->cur_extent, file->address);
}

uint32_t fs_file_read(FileHandle* file, void* destination, uint32_t size) {
    struct FSFileHeader header;

    if (destination == NULL || size == 0) return 0;
    if (!fs_file_handle_header(file, &header)) return 0;
    if (!(file->mode & FS_FILE_MODE_READ)) return 0;
    if (file->position >= header.total_logical_size) return 0;

    uint32_t to_read = header.total_logical_size - file->position;
    if (to_read > size)
        to_read = size;

    uint8_t* dest = (uint8_t*)destination;
    uint32_t done = 0;
    while (done < to_read) {
        if (!fs_file_locate(file, &header, file->position))
            break;
        uint32_t offset = file->position - file->cur_start;
        uint32_t chunk  = file->cur_size - offset;
        if (chunk > to_read - done)
            chunk = to_read - done;
        fs_mem_read(file->ctx, fs_file_cursor_data(file) + offset, dest + done, chunk);
        done           += chunk;
        file->position += chunk;
    }
    return done;
}

uint32_t fs_file_write(FileHandle* file, const void* source, uint32_t size) {
    struct FSFileHeader header;

    if (source == NULL || size == 0) return 0;
    if (!fs_file_handle_header(file, &header)) return 0;
    if (!(file->mode & FS_FILE_MODE_WRITE)) return 0;
    if (file->position > 0xFFFFFFFFU - size) return 0;   // Would pass 4 GB

    uint32_t end = file->position + size;
    if (end > header.total_logical_size) {
        // The cursor (if current, and in an extent) is somewhere in the
        // chain: start the tail search there rather than at the head
        uint32_t hint = (file->cur_extent != 0 && file->cur_extent != file->address &&
                         file->cur_generation == header.block.chain_generation) ? file->cur_extent : FS_NULL;
        if (!fs_file_grow(file->ctx, file->address, &header, end, hint))
            return 0;
    }

    const uint8_t* src = (const uint8_t*)source;
    uint32_t done = 0;
    while (done < size) {
        if (!fs_file_locate(file, &header, file->position))
            break;
        uint32_t offset = file->position - file->cur_start;
        uint32_t chunk  = file->cur_size - offset;
        if (chunk > size - done)
            chunk = size - done;
        fs_mem_write(file->ctx, fs_file_cursor_data(file) + offset, src + done, chunk);
        done           += chunk;
        file->position += chunk;
    }
    if (done > 0)
        file->written = 1;
    return done;
}

uint32_t fs_file_seek(FileHandle* file, uint32_t position) {
    uint32_t file_size;
    if (!fs_file_struct_is_valid(file))
        return FS_NULL;

    file_size = fs_file_get_size(file->ctx, file->address);
    if (position > file_size)
        position = file_size;

    file->position = position;
    return position;
}

uint32_t fs_file_tell(const FileHandle* file) {
    if (file == NULL)
        return FS_NULL;
    if (!file->is_open)
        return FS_NULL;
    return file->position;
}

uint32_t fs_file_get_address(const FileHandle* file) {
    if (file == NULL)
        return FS_NULL;
    if (!file->is_open)
        return FS_NULL;
    return file->address;
}
