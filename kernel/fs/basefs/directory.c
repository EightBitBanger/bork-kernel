#include <kernel/fs/fs.h>
#include <kernel/fs/basefs/directory.h>

#include <kernel/util/string.h>

// ---------------------------------------------------------------------------
// Layout (see FSDirectoryHeader / FSDirEntry in structs.h)
//
// A directory is its header allocation plus a chain of extent allocations.
// Each allocation has a record area after its own small header, and the
// records tile that area exactly, never crossing a device sector boundary.
//
// Every allocation is one whole block, so a record area always ends on a
// block boundary and starts at offset 52 (header) or 20 (extent) into it.
// With blocks and sectors of at least FS_MIN_SECTOR_SIZE bytes, the first
// stretch of every area is then longer than the largest possible record.
// ---------------------------------------------------------------------------

// Upper bound on any extent chain walk. A chain can never have more links
// than the device has blocks, so a longer walk means a corrupt (cyclic) chain.
static inline uint32_t dir_walk_limit(struct FSDeviceContext* ctx) {
    return (ctx && ctx->block_count) ? ctx->block_count : 0xFFFFU;
}

// Records never cross a multiple of this (absolute address). It is the
// device sector size, capped so that a whole-sector record still fits the
// 16-bit rec_len.
static uint32_t dir_unit(struct FSDeviceContext* ctx) {
    uint32_t unit = fs_device_window_size(ctx->device);
    if (unit == 0)
        unit = ctx->sector_size;
    while (unit > 0x8000U)
        unit >>= 1;
    return unit;
}

// Validated length of a name, or 0 if it cannot be stored
static uint32_t dir_name_length(const char* name) {
    if (name == NULL)
        return 0;
    size_t length = strnlen(name, FS_NAME_MAX + 1);
    if (length == 0 || length > FS_NAME_MAX)
        return 0;
    for (size_t i = 0; i < length; i++) {
        if (name[i] == '/')
            return 0;
    }
    return (uint32_t)length;
}

static bool dir_alloc_size(struct FSDeviceContext* ctx, uint32_t payload, uint32_t* size) {
    struct FSAllocHeader alloc;
    if (payload == FS_NULL || payload < sizeof(struct FSAllocHeader))
        return false;
    fs_mem_read(ctx, payload - sizeof(struct FSAllocHeader), &alloc, sizeof(struct FSAllocHeader));
    if (alloc.size == 0 || alloc.size > ctx->pool_size || payload + alloc.size < payload)
        return false;
    *size = alloc.size;
    return true;
}

static bool dir_header_read(struct FSDeviceContext* ctx, uint32_t address, struct FSDirectoryHeader* directory) {
    uint32_t size;
    if (!ctx || !dir_alloc_size(ctx, address, &size) || size < sizeof(struct FSDirectoryHeader))
        return false;
    fs_mem_read(ctx, address, directory, sizeof(struct FSDirectoryHeader));
    // A file header is the same size: the attribute tells them apart
    return (directory->block.attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
}

static void dir_header_write(struct FSDeviceContext* ctx, uint32_t address, const struct FSDirectoryHeader* directory) {
    fs_mem_write(ctx, address, directory, sizeof(struct FSDirectoryHeader));
}

static bool dir_extent_read(struct FSDeviceContext* ctx, uint32_t address, struct FSDirectoryExtent* extent) {
    uint32_t size;
    if (!dir_alloc_size(ctx, address, &size) || size < sizeof(struct FSDirectoryExtent))
        return false;
    fs_mem_read(ctx, address, extent, sizeof(struct FSDirectoryExtent));
    return true;
}

static void dir_extent_write(struct FSDeviceContext* ctx, uint32_t address, const struct FSDirectoryExtent* extent) {
    fs_mem_write(ctx, address, extent, sizeof(struct FSDirectoryExtent));
}

// ---------------------------------------------------------------------------
// Record areas
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t owner;         // Directory header or extent (payload address)
    bool     is_header;
    uint32_t start;         // Record area [start, end)
    uint32_t end;
    uint32_t next;          // Next extent, FS_NULL at the end of the chain
    uint32_t count;         // Live entries according to the owner's header
} DirArea;

static bool dir_area_load(struct FSDeviceContext* ctx, uint32_t owner, bool is_header, DirArea* area) {
    uint32_t size;
    uint32_t header_size = is_header ? sizeof(struct FSDirectoryHeader) : sizeof(struct FSDirectoryExtent);
    if (!dir_alloc_size(ctx, owner, &size) || size < header_size)
        return false;

    area->owner     = owner;
    area->is_header = is_header;
    area->start     = owner + header_size;
    area->end       = owner + size;

    if (is_header) {
        struct FSDirectoryHeader directory;
        if (!dir_header_read(ctx, owner, &directory))
            return false;
        area->next  = directory.extent.next;
        area->count = directory.inline_entries;
    } else {
        struct FSDirectoryExtent extent;
        if (!dir_extent_read(ctx, owner, &extent))
            return false;
        area->next  = extent.extent.next;
        area->count = extent.entry_count;
    }
    return true;
}

// Fill [start, end) with free records, one per device-sector stretch
static void dir_area_init(struct FSDeviceContext* ctx, uint32_t start, uint32_t end) {
    uint32_t unit = dir_unit(ctx);
    uint32_t at = start;
    while (at < end) {
        uint32_t stretch_end = (at / unit + 1U) * unit;
        if (stretch_end > end)
            stretch_end = end;

        struct FSDirEntry entry;
        entry.address  = FS_NULL;
        entry.rec_len  = (uint16_t)(stretch_end - at);
        entry.name_len = 0;
        entry.type     = FS_ENTRY_TYPE_NONE;
        fs_mem_write(ctx, at, &entry, sizeof(struct FSDirEntry));
        at = stretch_end;
    }
}

// Read and check the record at `at` inside an area ending at `end`
static bool dir_record_read(struct FSDeviceContext* ctx, uint32_t at, uint32_t end, uint32_t unit,
                            struct FSDirEntry* entry) {
    if (end - at < sizeof(struct FSDirEntry))
        return false;
    fs_mem_read(ctx, at, entry, sizeof(struct FSDirEntry));

    uint32_t length = entry->rec_len;
    if (length < sizeof(struct FSDirEntry) || (length % FS_DIR_ENTRY_ALIGN) != 0)
        return false;
    if (length > end - at || (at % unit) + length > unit)
        return false;
    if (entry->address != FS_NULL &&
        (entry->name_len == 0 || FS_DIR_ENTRY_SIZE(entry->name_len) > length))
        return false;
    return true;
}

// Write one record (header and name). Both land in the same device sector.
static void dir_record_write(struct FSDeviceContext* ctx, uint32_t at, uint32_t child, uint32_t rec_len,
                             const char* name, uint32_t name_len, uint8_t type) {
    struct FSDirEntry entry;
    entry.address  = child;
    entry.rec_len  = (uint16_t)rec_len;
    entry.name_len = (uint8_t)name_len;
    entry.type     = type;
    fs_mem_write(ctx, at + sizeof(struct FSDirEntry), name, name_len);
    fs_mem_write(ctx, at, &entry, sizeof(struct FSDirEntry));
}

static bool dir_record_name_equals(struct FSDeviceContext* ctx, uint32_t at, const struct FSDirEntry* entry,
                                   const char* name, uint32_t length) {
    if (entry->address == FS_NULL || entry->name_len != length)
        return false;

    uint8_t  chunk[32];
    uint32_t offset = 0;
    while (offset < length) {
        uint32_t piece = length - offset;
        if (piece > sizeof(chunk))
            piece = sizeof(chunk);
        fs_mem_read(ctx, at + sizeof(struct FSDirEntry) + offset, chunk, piece);
        if (memcmp(chunk, name + offset, piece) != 0)
            return false;
        offset += piece;
    }
    return true;
}

// Copy a record's name out, NUL-terminated. False if it does not fit.
static bool dir_record_copy_name(struct FSDeviceContext* ctx, uint32_t at, const struct FSDirEntry* entry,
                                 char* name, size_t name_size) {
    if (name == NULL)
        return true;
    if (name_size <= entry->name_len) {
        if (name_size > 0)
            name[0] = '\0';
        return false;
    }
    fs_mem_read(ctx, at + sizeof(struct FSDirEntry), name, entry->name_len);
    name[entry->name_len] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Walking every record of a directory
// ---------------------------------------------------------------------------

typedef struct {
    DirArea  area;
    uint32_t unit;
    uint32_t guard;         // Extent hops left
    uint32_t rec;           // Current record (FS_NULL before the first)
    uint32_t prev;          // Previous record in the same sector, or FS_NULL
    struct FSDirEntry entry;
    bool     corrupt;
} DirWalk;

static bool dir_walk_begin(struct FSDeviceContext* ctx, uint32_t directory, DirWalk* walk) {
    memset(walk, 0, sizeof(DirWalk));
    walk->rec  = FS_NULL;
    walk->prev = FS_NULL;
    if (!ctx || !dir_area_load(ctx, directory, true, &walk->area))
        return false;
    walk->unit  = dir_unit(ctx);
    walk->guard = dir_walk_limit(ctx);
    return true;
}

// Step to the next record, free or live. Returns false at the end of the
// directory; walk->corrupt says whether it stopped early on bad data.
static bool dir_walk_next(struct FSDeviceContext* ctx, DirWalk* walk) {
    for (;;) {
        uint32_t at;
        if (walk->rec == FS_NULL) {
            at = walk->area.start;
            walk->prev = FS_NULL;
        } else {
            at = walk->rec + walk->entry.rec_len;
            walk->prev = walk->rec;
        }

        if (at < walk->area.end) {
            if (at == walk->area.start || (at % walk->unit) == 0)
                walk->prev = FS_NULL;   // A sector starts here: no neighbor
            if (!dir_record_read(ctx, at, walk->area.end, walk->unit, &walk->entry)) {
                walk->corrupt = true;
                return false;
            }
            walk->rec = at;
            return true;
        }

        if (walk->area.next == FS_NULL)
            return false;
        if (walk->guard-- == 0 || !dir_area_load(ctx, walk->area.next, false, &walk->area)) {
            walk->corrupt = true;
            return false;
        }
        walk->rec = FS_NULL;
    }
}

// ---------------------------------------------------------------------------
// Counts and extents
// ---------------------------------------------------------------------------

// Adjust the live counts after adding (+1) or removing (-1) an entry in
// `area`, and stamp the directory as modified
static void dir_count_adjust(struct FSDeviceContext* ctx, uint32_t directory, const DirArea* area, int delta) {
    struct FSDirectoryHeader header;
    if (!dir_header_read(ctx, directory, &header))
        return;

    if (area->is_header) {
        if (delta > 0)                      header.inline_entries++;
        else if (header.inline_entries > 0) header.inline_entries--;
    } else {
        struct FSDirectoryExtent extent;
        if (dir_extent_read(ctx, area->owner, &extent)) {
            if (delta > 0)                   extent.entry_count++;
            else if (extent.entry_count > 0) extent.entry_count--;
            dir_extent_write(ctx, area->owner, &extent);
        }
    }

    if (delta > 0)                   header.entry_count++;
    else if (header.entry_count > 0) header.entry_count--;
    header.block.modified = fs_time_now();
    dir_header_write(ctx, directory, &header);
}

// New, unlinked extent of one block, its area filled with free records
static uint32_t dir_extent_create(struct FSDeviceContext* ctx, uint32_t prev) {
    uint32_t size = ctx->sector_size - sizeof(struct FSAllocHeader);
    uint32_t address = fs_alloc(ctx, size);
    if (address == FS_NULL)
        return FS_NULL;

    struct FSDirectoryExtent extent;
    memset(&extent, 0x00, sizeof(struct FSDirectoryExtent));
    extent.extent.next = FS_NULL;
    extent.extent.prev = prev;
    dir_extent_write(ctx, address, &extent);
    dir_area_init(ctx, address + sizeof(struct FSDirectoryExtent), address + size);
    return address;
}

static void dir_extent_unlink_and_free(struct FSDeviceContext* ctx, uint32_t directory, uint32_t extent_address) {
    struct FSDirectoryHeader header;
    struct FSDirectoryExtent extent;
    struct FSDirectoryExtent other;

    if (!dir_header_read(ctx, directory, &header))
        return;
    if (!dir_extent_read(ctx, extent_address, &extent))
        return;

    if (extent.extent.prev == directory) {
        header.extent.next = extent.extent.next;
    } else if (extent.extent.prev != FS_NULL && dir_extent_read(ctx, extent.extent.prev, &other)) {
        other.extent.next = extent.extent.next;
        dir_extent_write(ctx, extent.extent.prev, &other);
    }

    if (extent.extent.next != FS_NULL && dir_extent_read(ctx, extent.extent.next, &other)) {
        other.extent.prev = extent.extent.prev;
        dir_extent_write(ctx, extent.extent.next, &other);
    }

    if (header.extent.prev == extent_address)
        header.extent.prev = (extent.extent.prev == directory) ? FS_NULL : extent.extent.prev;
    dir_header_write(ctx, directory, &header);

    fs_free(ctx, extent_address);
}

// Remove the live record for `child` (at `record` if that is not FS_NULL)
static uint8_t dir_remove(struct FSDeviceContext* ctx, uint32_t directory, uint32_t child, uint32_t record) {
    DirWalk walk;
    if (!dir_walk_begin(ctx, directory, &walk))
        return FS_DIR_ERR_NOT_DIR;

    while (dir_walk_next(ctx, &walk)) {
        if (walk.entry.address != child)
            continue;
        if (record != FS_NULL && walk.rec != record)
            continue;

        if (walk.prev != FS_NULL) {
            // Give the space to the previous record in this sector
            struct FSDirEntry before;
            fs_mem_read(ctx, walk.prev, &before, sizeof(struct FSDirEntry));
            before.rec_len = (uint16_t)(before.rec_len + walk.entry.rec_len);
            fs_mem_write(ctx, walk.prev, &before, sizeof(struct FSDirEntry));
        } else {
            struct FSDirEntry freed = walk.entry;
            freed.address  = FS_NULL;
            freed.name_len = 0;
            freed.type     = FS_ENTRY_TYPE_NONE;
            fs_mem_write(ctx, walk.rec, &freed, sizeof(struct FSDirEntry));
        }

        dir_count_adjust(ctx, directory, &walk.area, -1);

        // An extent that no longer holds anything is given back
        if (!walk.area.is_header) {
            struct FSDirectoryExtent extent;
            if (dir_extent_read(ctx, walk.area.owner, &extent) && extent.entry_count == 0)
                dir_extent_unlink_and_free(ctx, directory, walk.area.owner);
        }
        return FS_DIR_OK;
    }
    return walk.corrupt ? FS_DIR_ERR_CORRUPT : FS_DIR_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Create / delete
// ---------------------------------------------------------------------------

uint32_t fs_directory_create(struct FSDeviceContext* ctx, const char* name, uint8_t permissions, uint32_t parent_directory) {
    if (!ctx || ctx->sector_size < FS_MIN_SECTOR_SIZE)
        return FS_NULL;
    if (parent_directory != FS_NULL && dir_name_length(name) == 0)
        return FS_NULL;

    // One whole block: header plus as many records as fit after it
    uint32_t size = ctx->sector_size - sizeof(struct FSAllocHeader);
    uint32_t address = fs_alloc(ctx, size);
    if (address == FS_NULL)
        return FS_NULL;

    // Write the directory completely and link it into the parent last, so
    // the parent never references an unwritten (or freed) directory
    struct FSDirectoryHeader directory;
    memset(&directory, 0x00, sizeof(struct FSDirectoryHeader));
    directory.block.attributes  = FS_ATTRIBUTE_DIRECTORY;
    directory.block.permissions = permissions;
    directory.block.parent      = parent_directory;
    // Inherit the parent's security clearance (the root starts at 0)
    if (parent_directory != FS_NULL) {
        uint32_t security = directory.block.security;
        fs_file_get_security(ctx, parent_directory, &security);
    }
    directory.block.created     = fs_time_now();
    directory.block.modified    = directory.block.created;
    directory.extent.next       = FS_NULL;
    directory.extent.prev       = FS_NULL;
    dir_header_write(ctx, address, &directory);
    dir_area_init(ctx, address + sizeof(struct FSDirectoryHeader), address + size);

    if (parent_directory != FS_NULL &&
        fs_directory_add_entry(ctx, parent_directory, name, address, FS_ENTRY_TYPE_DIRECTORY) != FS_DIR_OK) {
        fs_free(ctx, address);
        return FS_NULL;
    }
    return address;
}

bool fs_directory_delete(struct FSDeviceContext* ctx, uint32_t address) {
    struct FSDirectoryHeader directory;
    if (!dir_header_read(ctx, address, &directory))
        return false;

    uint32_t extent_address = directory.extent.next;
    uint32_t guard = dir_walk_limit(ctx);
    while (extent_address != FS_NULL && guard-- > 0) {
        struct FSDirectoryExtent extent;
        if (!dir_extent_read(ctx, extent_address, &extent))
            break;
        uint32_t next = extent.extent.next;
        fs_free(ctx, extent_address);
        extent_address = next;
    }

    fs_free(ctx, address);
    return true;
}

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

uint8_t fs_directory_add_entry(struct FSDeviceContext* ctx, uint32_t directory_address, const char* name,
                               uint32_t child, uint8_t type) {
    if (!ctx || child == FS_NULL || child == 0)
        return FS_DIR_ERR_ARGUMENT;
    uint32_t length = dir_name_length(name);
    if (length == 0)
        return FS_DIR_ERR_ARGUMENT;
    uint32_t need = FS_DIR_ENTRY_SIZE(length);

    DirWalk walk;
    if (!dir_walk_begin(ctx, directory_address, &walk))
        return FS_DIR_ERR_NOT_DIR;

    // One pass: the name must not exist anywhere, and remember the first
    // place the new record fits
    bool              found = false;
    uint32_t          slot = FS_NULL;
    uint32_t          slot_used = 0;    // 0: take a free record; else split a live one after this many bytes
    struct FSDirEntry slot_entry;
    DirArea           slot_area;

    while (dir_walk_next(ctx, &walk)) {
        if (dir_record_name_equals(ctx, walk.rec, &walk.entry, name, length))
            return FS_DIR_ERR_EXISTS;
        if (found)
            continue;

        if (walk.entry.address == FS_NULL) {
            if (walk.entry.rec_len >= need) {
                found = true;
                slot_used = 0;
            }
        } else {
            uint32_t used = FS_DIR_ENTRY_SIZE(walk.entry.name_len);
            if (walk.entry.rec_len - used >= need) {
                found = true;
                slot_used = used;
            }
        }
        if (found) {
            slot       = walk.rec;
            slot_entry = walk.entry;
            slot_area  = walk.area;
        }
    }
    if (walk.corrupt)
        return FS_DIR_ERR_CORRUPT;

    if (found) {
        if (slot_used == 0) {
            dir_record_write(ctx, slot, child, slot_entry.rec_len, name, length, type);
        } else {
            // The new record goes into the spare tail of an existing one.
            // Write it first (invisible until the old record shrinks), then
            // shrink the old one. Both are in the same device sector.
            dir_record_write(ctx, slot + slot_used, child, slot_entry.rec_len - slot_used, name, length, type);
            slot_entry.rec_len = (uint16_t)slot_used;
            fs_mem_write(ctx, slot, &slot_entry, sizeof(struct FSDirEntry));
        }
        dir_count_adjust(ctx, directory_address, &slot_area, +1);
        return FS_DIR_OK;
    }

    // No room: build a new extent completely, then link it after the last
    // area (where the walk ended)
    uint32_t tail = walk.area.owner;
    uint32_t extent_address = dir_extent_create(ctx, tail);
    if (extent_address == FS_NULL)
        return FS_DIR_ERR_NO_SPACE;

    struct FSDirEntry first;
    uint32_t first_at = extent_address + sizeof(struct FSDirectoryExtent);
    fs_mem_read(ctx, first_at, &first, sizeof(struct FSDirEntry));
    if (first.rec_len < need) {             // Only with a nonsense geometry
        fs_free(ctx, extent_address);
        return FS_DIR_ERR_NO_SPACE;
    }
    dir_record_write(ctx, first_at, child, first.rec_len, name, length, type);

    struct FSDirectoryExtent extent;
    dir_extent_read(ctx, extent_address, &extent);
    extent.entry_count = 1;
    dir_extent_write(ctx, extent_address, &extent);

    struct FSDirectoryHeader header;
    if (!dir_header_read(ctx, directory_address, &header)) {
        fs_free(ctx, extent_address);
        return FS_DIR_ERR_NOT_DIR;
    }
    if (walk.area.is_header) {
        header.extent.next = extent_address;
    } else {
        struct FSDirectoryExtent last;
        if (!dir_extent_read(ctx, tail, &last)) {
            fs_free(ctx, extent_address);
            return FS_DIR_ERR_CORRUPT;
        }
        last.extent.next = extent_address;
        dir_extent_write(ctx, tail, &last);
    }
    header.extent.prev = extent_address;
    header.entry_count++;
    header.block.modified = fs_time_now();
    dir_header_write(ctx, directory_address, &header);
    return FS_DIR_OK;
}

uint8_t fs_directory_remove_entry(struct FSDeviceContext* ctx, uint32_t directory_address, uint32_t child) {
    if (!ctx || child == FS_NULL)
        return FS_DIR_ERR_ARGUMENT;
    return dir_remove(ctx, directory_address, child, FS_NULL);
}

uint8_t fs_directory_rename_entry(struct FSDeviceContext* ctx, uint32_t directory_address, uint32_t child,
                                  const char* new_name) {
    if (!ctx || child == FS_NULL)
        return FS_DIR_ERR_ARGUMENT;
    uint32_t length = dir_name_length(new_name);
    if (length == 0)
        return FS_DIR_ERR_ARGUMENT;

    DirWalk walk;
    if (!dir_walk_begin(ctx, directory_address, &walk))
        return FS_DIR_ERR_NOT_DIR;

    uint32_t          record = FS_NULL;
    struct FSDirEntry entry;
    while (dir_walk_next(ctx, &walk)) {
        if (dir_record_name_equals(ctx, walk.rec, &walk.entry, new_name, length)) {
            if (walk.entry.address != child)
                return FS_DIR_ERR_EXISTS;
            return FS_DIR_OK;               // Already called that
        }
        if (record == FS_NULL && walk.entry.address == child) {
            record = walk.rec;
            entry  = walk.entry;
        }
    }
    if (walk.corrupt)
        return FS_DIR_ERR_CORRUPT;
    if (record == FS_NULL)
        return FS_DIR_ERR_NOT_FOUND;

    if (FS_DIR_ENTRY_SIZE(length) <= entry.rec_len) {
        // Fits in place: one sector write
        dir_record_write(ctx, record, child, entry.rec_len, new_name, length, entry.type);
        struct FSDirectoryHeader header;
        if (dir_header_read(ctx, directory_address, &header)) {
            header.block.modified = fs_time_now();
            dir_header_write(ctx, directory_address, &header);
        }
        return FS_DIR_OK;
    }

    // Longer than the record: link the new name first, then drop the old
    // record. A crash in between leaves both names, never neither. (Adding
    // cannot move the old record: it only splits records with enough spare
    // room, and this one has too little.)
    uint8_t result = fs_directory_add_entry(ctx, directory_address, new_name, child, entry.type);
    if (result != FS_DIR_OK)
        return result;
    return dir_remove(ctx, directory_address, child, record);
}

// Locate live entry `index` using the per-area counts to skip whole areas
static bool dir_locate_index(struct FSDeviceContext* ctx, uint32_t directory, uint32_t index,
                             uint32_t* record, struct FSDirEntry* entry) {
    DirArea area;
    if (!ctx || !dir_area_load(ctx, directory, true, &area))
        return false;

    uint32_t unit  = dir_unit(ctx);
    uint32_t guard = dir_walk_limit(ctx);
    for (;;) {
        if (index < area.count) {
            uint32_t at = area.start;
            while (at < area.end) {
                if (!dir_record_read(ctx, at, area.end, unit, entry))
                    return false;
                if (entry->address != FS_NULL) {
                    if (index == 0) {
                        *record = at;
                        return true;
                    }
                    index--;
                }
                at += entry->rec_len;
            }
            return false;   // Count was stale (crash): treat as missing
        }

        index -= area.count;
        if (area.next == FS_NULL || guard-- == 0)
            return false;
        if (!dir_area_load(ctx, area.next, false, &area))
            return false;
    }
}

uint32_t fs_directory_get_reference_count(struct FSDeviceContext* ctx, uint32_t directory_address) {
    struct FSDirectoryHeader directory;
    // Return 0 (not FS_NULL) on failure: callers use this as a loop bound
    if (!dir_header_read(ctx, directory_address, &directory))
        return 0;
    return directory.entry_count;
}

uint32_t fs_directory_get_reference(struct FSDeviceContext* ctx, uint32_t directory_address, uint32_t index) {
    uint32_t record;
    struct FSDirEntry entry;
    if (!dir_locate_index(ctx, directory_address, index, &record, &entry))
        return FS_NULL;
    return entry.address;
}

bool fs_directory_get_entry(struct FSDeviceContext* ctx, uint32_t directory_address, uint32_t index,
                            uint32_t* address, char* name, size_t name_size, uint8_t* type) {
    uint32_t record;
    struct FSDirEntry entry;
    if (name != NULL && name_size > 0)
        name[0] = '\0';
    if (!dir_locate_index(ctx, directory_address, index, &record, &entry))
        return false;
    if (!dir_record_copy_name(ctx, record, &entry, name, name_size))
        return false;
    if (address) *address = entry.address;
    if (type)    *type    = entry.type;
    return true;
}

bool fs_directory_get_name(struct FSDeviceContext* ctx, uint32_t directory_address, uint32_t child,
                           char* name, size_t name_size) {
    if (name != NULL && name_size > 0)
        name[0] = '\0';
    if (child == FS_NULL)
        return false;

    DirWalk walk;
    if (!dir_walk_begin(ctx, directory_address, &walk))
        return false;
    while (dir_walk_next(ctx, &walk)) {
        if (walk.entry.address == child)
            return dir_record_copy_name(ctx, walk.rec, &walk.entry, name, name_size);
    }
    return false;
}

uint32_t fs_directory_find(struct FSDeviceContext* ctx, uint32_t directory_address, const char* name) {
    uint32_t length = dir_name_length(name);
    if (length == 0)
        return FS_NULL;

    DirWalk walk;
    if (!dir_walk_begin(ctx, directory_address, &walk))
        return FS_NULL;
    while (dir_walk_next(ctx, &walk)) {
        if (dir_record_name_equals(ctx, walk.rec, &walk.entry, name, length))
            return walk.entry.address;
    }
    return FS_NULL;
}

uint32_t fs_directory_get_parent(struct FSDeviceContext* ctx, uint32_t directory_address) {
    struct FSDirectoryHeader directory;
    if (!dir_header_read(ctx, directory_address, &directory))
        return FS_NULL;
    return directory.block.parent;
}

// ---------------------------------------------------------------------------
// Recursive delete
// ---------------------------------------------------------------------------

// First live entry of a directory. False when it has none (or on corruption,
// which *corrupt reports).
static bool dir_first_live(struct FSDeviceContext* ctx, uint32_t directory, uint32_t* child, bool* corrupt) {
    DirWalk walk;
    *corrupt = false;
    if (!dir_walk_begin(ctx, directory, &walk)) {
        *corrupt = true;
        return false;
    }
    while (dir_walk_next(ctx, &walk)) {
        if (walk.entry.address != FS_NULL) {
            *child = walk.entry.address;
            return true;
        }
    }
    *corrupt = walk.corrupt;
    return false;
}

static bool dir_is_directory(struct FSDeviceContext* ctx, uint32_t address) {
    struct FSDirectoryHeader directory;
    return dir_header_read(ctx, address, &directory);
}

static uint32_t dir_node_parent(struct FSDeviceContext* ctx, uint32_t address) {
    struct FSBlockHeader block;
    fs_mem_read(ctx, address, &block, sizeof(struct FSBlockHeader));
    return block.parent;
}

// Delete a directory together with everything below it, then the directory
// itself. If `parent_directory` is not FS_NULL the directory is unlinked from
// it first, so an interrupted delete leaves orphaned blocks rather than a
// parent entry pointing at freed storage.
//
// The walk is iterative (no recursion on the kernel stack): it always works
// on the first entry of the current directory, descends into non-empty
// subdirectories, and climbs back up through the `parent` field once a
// directory has been emptied.
//
// Only objects whose `parent` field names the directory they were found in
// are descended into or freed. Anything else (a corrupt or cross-linked
// entry, or a link back to the directory being deleted) is only unlinked,
// never freed, so the walk cannot escape the subtree or loop forever.
//
// Permissions are not checked here; that is the caller's job.
// Refuses to delete the partition's root directory.
bool fs_directory_delete_recursive(struct FSDeviceContext* ctx, uint32_t address, uint32_t parent_directory) {
    if (!ctx || address == FS_NULL)
        return false;
    if (!dir_is_directory(ctx, address))
        return false;

    struct FSPartitionBlock partition;
    if (fs_device_get_partition(ctx, &partition) != 0)
        return false;
    if (address == partition.root_directory)
        return false;

    if (parent_directory != FS_NULL)
        fs_directory_remove_entry(ctx, parent_directory, address);

    // Every step frees an object, unlinks an entry or descends once into a
    // directory that will later be freed. Every object owns at least one
    // block, so running out of steps means the structure is corrupt.
    uint32_t steps = dir_walk_limit(ctx) * 4U;
    uint32_t current = address;

    while (steps-- > 0) {
        uint32_t child = FS_NULL;
        bool     corrupt;

        if (!dir_first_live(ctx, current, &child, &corrupt)) {
            if (corrupt)
                return false;
            if (current == address) {
                fs_directory_delete(ctx, current);
                return true;
            }
            // Emptied a subdirectory: unlink it, free it, climb back up
            uint32_t up = fs_directory_get_parent(ctx, current);
            if (up == FS_NULL || fs_directory_remove_entry(ctx, up, current) != FS_DIR_OK)
                return false;
            fs_directory_delete(ctx, current);
            current = up;
            continue;
        }

        if (child != address && child != current &&
            dir_is_directory(ctx, child) && fs_directory_get_parent(ctx, child) == current) {
            // Owned subdirectory: empty it first (unlinked when we come back)
            current = child;
            continue;
        }

        // A file, or a foreign/corrupt entry: unlink it, and free it only if
        // it is a file that belongs here
        if (fs_directory_remove_entry(ctx, current, child) != FS_DIR_OK)
            return false;
        if (fs_file_check(ctx, child) && dir_node_parent(ctx, child) == current)
            fs_file_delete(ctx, child);
    }

    return false;
}

