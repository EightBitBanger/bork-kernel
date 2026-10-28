// basefs consistency check (see check.h)
//
// Pass 1 walks the tree from the root. Every object found is checked and
// its blocks claimed in a bitmap of our own (one bit per block); a block
// claimed twice means two objects share storage, and the second link is
// the one dropped. Directories are visited depth first from a stack of
// addresses, so the depth of the tree costs no kernel stack.
//
// Pass 2 compares the claims with the allocation bitmap on the disk. It
// only runs when pass 1 saw the whole tree, so a repair never frees blocks
// of something it could not reach for another reason.
//
// Paths in the messages are built only when there is something to say, by
// climbing the parent links of the directory in question.

#include <stdint.h>
#include <stdbool.h>

#include <kernel/malloc.h>
#include <kernel/fs/fs.h>
#include <kernel/fs/basefs/check.h>
#include <kernel/util/string.h>

// Largest claim bitmap the check allocates (bytes): 32M blocks, 16 GB at
// 512-byte blocks. A bigger volume gets the tree check only.
#define CHECK_CLAIMS_MAX        (4UL * 1024UL * 1024UL)

// Levels a message path is built from; deeper paths start with "..."
#define CHECK_PATH_DEPTH        32
#define CHECK_PATH_MAX          VFS_PATH_MAX

typedef struct {
    struct FSDeviceContext* ctx;
    bool            repair;
    VfsCheckReport* report;
    uint64_t        root;

    uint8_t*        claims;         // One bit per block, NULL: no block check
    uint64_t        claimed;        // Blocks claimed so far

    uint64_t*       stack;          // Directories still to visit
    uint32_t        stack_count;
    uint32_t        stack_max;
    bool            out_of_memory;

    uint64_t        visits_left;    // Bound on the walk without claims
    bool            looped;         // ...which ran out: the tree loops
} Check;

typedef enum {
    CLAIM_OK,
    CLAIM_BAD,          // No live allocation there
    CLAIM_SHARED,       // Its blocks already belong to something else
} CheckClaim;

// Used while the VFS lock is held, one check at a time
static char check_path[CHECK_PATH_MAX];

//
// Messages
//

// Append `text` to check_path, keeping it terminated; control characters
// show as '?'
static void check_path_append(size_t* length, const char* text, size_t count) {
    for (size_t i = 0; i < count && text[i] != '\0' && *length + 1 < sizeof(check_path); i++) {
        char c = text[i];
        check_path[(*length)++] = ((uint8_t)c < 0x20 || c == 0x7F) ? '?' : c;
    }
    check_path[*length] = '\0';
}

static uint64_t check_parent_of(Check* c, uint64_t node) {
    struct FSBlockHeader block;
    fs_mem_read(c->ctx, node, &block, sizeof(struct FSBlockHeader));
    return block.parent;
}

// "/a/b" for directory `dir`, then "/name" when `name` is given
// (name_length bytes, not terminated)
static const char* check_path_of(Check* c, uint64_t dir, const char* name, size_t name_length) {
    uint64_t chain[CHECK_PATH_DEPTH];
    uint32_t depth = 0;
    uint64_t node = dir;
    while (node != c->root && depth < CHECK_PATH_DEPTH) {
        chain[depth++] = node;
        node = check_parent_of(c, node);
        if (node == FS_NULL || !fs_check_directory_valid(c->ctx, node))
            break;
    }

    size_t length = 0;
    check_path[0] = '\0';
    if (node != c->root)
        check_path_append(&length, "...", 3);

    char part[FS_NAME_MAX + 1];
    while (depth > 0) {
        uint64_t child  = chain[--depth];
        uint64_t parent = check_parent_of(c, child);
        if (!fs_directory_get_name(c->ctx, parent, child, part, sizeof(part)))
            strcpy(part, "?");
        check_path_append(&length, "/", 1);
        check_path_append(&length, part, sizeof(part));
    }
    if (name != NULL) {
        check_path_append(&length, "/", 1);
        check_path_append(&length, name, name_length);
    }
    if (length == 0)
        check_path_append(&length, "/", 1);
    return check_path;
}

// Count a problem and pass it on. `fixable`: repaired when repairing.
static void check_problem(Check* c, const char* path, const char* problem, bool fixable) {
    bool fixed = fixable && c->repair;
    c->report->problems++;
    if (fixed)
        c->report->fixed++;
    if (c->report->problem != NULL)
        c->report->problem(c->report->user, path, problem, fixed);
}

//
// Claims
//

static inline uint64_t check_blocks_for(Check* c, uint64_t size) {
    return ((uint64_t)sizeof(struct FSAllocHeader) + size + c->ctx->sector_size - 1U) / c->ctx->sector_size;
}

static inline bool check_claimed(Check* c, uint64_t block) {
    return (c->claims[block >> 3] & (uint8_t)(1U << (block & 7U))) != 0;
}

// Whether `payload` is a live allocation of at least `min_size` bytes: it
// starts a block inside the pool and its size fits. *first / *count give
// its blocks.
static bool check_allocation(Check* c, uint64_t payload, uint64_t min_size, uint64_t* first, uint64_t* count) {
    struct FSDeviceContext* ctx = c->ctx;
    if (payload == FS_NULL || payload < sizeof(struct FSAllocHeader))
        return false;
    uint64_t at = payload - sizeof(struct FSAllocHeader);
    if ((at % ctx->sector_size) != 0)
        return false;

    struct FSAllocHeader alloc;
    fs_mem_read(ctx, at, &alloc, sizeof(struct FSAllocHeader));
    if (alloc.size < min_size || alloc.size > ctx->pool_size)
        return false;

    *first = at / ctx->sector_size;
    *count = check_blocks_for(c, alloc.size);
    return *first >= ctx->reserved_blocks && *first < ctx->block_count &&
           *count <= ctx->block_count - *first;
}

// Claim the blocks of the allocation at `payload`. Nothing is claimed
// unless all of them are free to claim.
static CheckClaim check_claim(Check* c, uint64_t payload, uint64_t min_size) {
    uint64_t first, count;
    if (!check_allocation(c, payload, min_size, &first, &count))
        return CLAIM_BAD;
    if (c->claims == NULL)
        return CLAIM_OK;

    for (uint64_t b = first; b < first + count; b++) {
        if (check_claimed(c, b))
            return CLAIM_SHARED;
    }
    for (uint64_t b = first; b < first + count; b++)
        c->claims[b >> 3] |= (uint8_t)(1U << (b & 7U));
    c->claimed += count;
    return CLAIM_OK;
}

// fs_check_extent_fn for directory and file extents (both 24 bytes)
_Static_assert(sizeof(struct FSDirectoryExtent) == sizeof(struct FSFileExtent), "extent sizes differ");

static bool check_extent(void* user, uint64_t extent) {
    return check_claim((Check*)user, extent, sizeof(struct FSFileExtent)) == CLAIM_OK;
}

//
// Pass 1: the tree
//

static bool check_push(Check* c, uint64_t directory) {
    if (c->stack_count == c->stack_max) {
        uint32_t grown = c->stack_max ? c->stack_max * 2U : 64U;
        uint64_t* stack = realloc(c->stack, grown * sizeof(uint64_t));
        if (stack == NULL) {
            c->out_of_memory = true;
            return false;
        }
        c->stack = stack;
        c->stack_max = grown;
    }
    c->stack[c->stack_count++] = directory;
    return true;
}

// A name the VFS can use: no '/', no NUL, not "." or ".."
static bool check_name_valid(const char* name, uint32_t length) {
    if (length == 0)
        return false;
    if ((length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.'))
        return false;
    for (uint32_t i = 0; i < length; i++) {
        if (name[i] == '/' || name[i] == '\0')
            return false;
    }
    return true;
}

// True if `directory` is a directory with an entry for `child`
static bool check_links_to(Check* c, uint64_t directory, uint64_t child) {
    return directory != FS_NULL && fs_check_directory_valid(c->ctx, directory) &&
           fs_directory_get_name(c->ctx, directory, child, NULL, 0);
}

// True if the parent links from `directory` lead up to the root, each one
// matched by an entry
static bool check_reachable(Check* c, uint64_t directory) {
    uint64_t node = directory;
    for (uint32_t i = 0; i < CHECK_PATH_DEPTH * 4U; i++) {
        if (node == c->root)
            return true;
        uint64_t parent = check_parent_of(c, node);
        if (!check_links_to(c, parent, node))
            return false;
        node = parent;
    }
    return false;
}

static void check_file(Check* c, uint64_t dir, const char* name, uint32_t name_length, uint64_t file) {
    FSFileCheck found;
    uint64_t size = 0;
    if (!fs_file_check_chain(c->ctx, file, c->repair, check_extent, c, &found, &size))
        return;

    if (found.bad_chain)
        check_problem(c, check_path_of(c, dir, name, name_length),
                      c->repair ? "damaged data chain, file cut short" : "damaged data chain", true);
    else if (found.bad_size)
        check_problem(c, check_path_of(c, dir, name, name_length), "size does not match its data", true);
    if (found.bad_links)
        check_problem(c, check_path_of(c, dir, name, name_length), "wrong links in its data chain", true);

    c->report->files++;
    c->report->file_bytes += size;
}

// fs_check_entry_fn: one entry of the directory being checked
static bool check_entry(void* user, uint64_t dir, uint64_t record, const struct FSDirEntry* entry) {
    Check* c = (Check*)user;
    struct FSDeviceContext* ctx = c->ctx;

    char name[FS_NAME_MAX + 1];
    uint32_t name_length = entry->name_len;
    fs_mem_read(ctx, record + sizeof(struct FSDirEntry), name, name_length);
    name[name_length] = '\0';

    if (c->claims == NULL && c->visits_left-- == 0) {
        c->looped = true;
        return true;
    }

    // Where it points: a live file or directory header
    uint64_t child = entry->address;
    if (child == c->root || child == dir) {
        check_problem(c, check_path_of(c, dir, name, name_length),
                      child == dir ? "entry links the folder to itself, removed"
                                   : "entry links back to the root, removed", true);
        return false;
    }

    uint64_t first, count;
    struct FSBlockHeader block;
    bool live = check_allocation(c, child, sizeof(struct FSFileHeader), &first, &count);
    if (live)
        fs_mem_read(ctx, child, &block, sizeof(struct FSBlockHeader));
    bool is_dir = live && (block.attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
    if (!live || (is_dir && !fs_check_directory_valid(ctx, child))) {
        check_problem(c, check_path_of(c, dir, name, name_length),
                      c->repair ? "entry points at nothing, removed" : "entry points at nothing", true);
        return false;
    }

    // A folder in the tree that it names as its parent, and that links it
    // too, owns it: this entry is the extra one
    bool wrong_parent = block.parent != dir;
    if (wrong_parent && check_links_to(c, block.parent, child) && check_reachable(c, block.parent)) {
        check_problem(c, check_path_of(c, dir, name, name_length),
                      c->repair ? "also linked from another folder, removed here"
                                : "also linked from another folder", true);
        return false;
    }

    CheckClaim claim = check_claim(c, child, is_dir ? sizeof(struct FSDirectoryHeader)
                                                    : sizeof(struct FSFileHeader));
    if (claim != CLAIM_OK) {
        check_problem(c, check_path_of(c, dir, name, name_length),
                      c->repair ? "shares its storage with another entry, removed"
                                : "shares its storage with another entry", true);
        return false;
    }

    // Otherwise the parent link is just wrong: this folder is the one
    if (wrong_parent) {
        check_problem(c, check_path_of(c, dir, name, name_length), "wrong parent link", true);
        if (c->repair) {
            block.parent = dir;
            fs_mem_write(ctx, child, &block, sizeof(struct FSBlockHeader));
        }
    }

    // Repairs that keep the entry, written into its record
    if (!check_name_valid(name, name_length)) {
        check_problem(c, check_path_of(c, dir, name, name_length),
                      c->repair ? "invalid name, bad characters replaced with '_'" : "invalid name", true);
        if (c->repair) {
            bool dots = name[0] == '.' && (name_length == 1 || (name_length == 2 && name[1] == '.'));
            for (uint32_t i = 0; i < name_length; i++) {
                if (dots || name[i] == '/' || name[i] == '\0')
                    name[i] = '_';
            }
            fs_mem_write(ctx, record + sizeof(struct FSDirEntry), name, name_length);
        }
    }

    uint8_t type = is_dir ? FS_ENTRY_TYPE_DIRECTORY : FS_ENTRY_TYPE_FILE;
    if (entry->type != type) {
        check_problem(c, check_path_of(c, dir, name, name_length), "wrong entry type", true);
        if (c->repair)
            fs_mem_write(ctx, record + __builtin_offsetof(struct FSDirEntry, type), &type, sizeof(type));
    }

    if (is_dir) {
        c->report->directories++;
        check_push(c, child);
    } else {
        check_file(c, dir, name, name_length, child);
    }
    return true;
}

static void check_directory(Check* c, uint64_t dir) {
    FSDirCheck found;
    if (!fs_directory_check(c->ctx, dir, c->repair, check_extent, check_entry, c, &found))
        return;     // Checked when its entry was: cannot happen

    if (found.bad_records > 0)
        check_problem(c, check_path_of(c, dir, NULL, 0),
                      c->repair ? "damaged entries, cleared" : "damaged entries", true);
    if (found.bad_chain)
        check_problem(c, check_path_of(c, dir, NULL, 0),
                      c->repair ? "damaged extent chain, entries past it lost" : "damaged extent chain", true);
    if (found.bad_links)
        check_problem(c, check_path_of(c, dir, NULL, 0), "wrong extent links", true);
    if (found.bad_counts)
        check_problem(c, check_path_of(c, dir, NULL, 0), "wrong entry count", true);
}

//
// Pass 2: the allocation bitmap
//

static uint32_t check_bit_count(uint8_t byte) {
    uint32_t count = 0;
    for (; byte != 0; byte &= (uint8_t)(byte - 1))
        count++;
    return count;
}

static void check_bitmap(Check* c) {
    struct FSDeviceContext* ctx = c->ctx;
    uint64_t lost = 0, unmarked = 0;

    for (uint64_t i = 0; i < ctx->bitmap_size; i++) {
        uint8_t valid = 0xFF;
        if ((i + 1) * 8 > ctx->block_count)
            valid = (uint8_t)((1U << (ctx->block_count - i * 8)) - 1U);

        uint8_t disk = fs_bitmap_get_byte(ctx, i) & valid;
        uint8_t mine = c->claims[i] & valid;
        uint8_t free_these = disk & (uint8_t)~mine;
        uint8_t mark_these = mine & (uint8_t)~disk;
        if (free_these == 0 && mark_these == 0)
            continue;

        lost     += check_bit_count(free_these);
        unmarked += check_bit_count(mark_these);
        if (c->repair) {
            for (uint32_t bit = 0; bit < 8; bit++) {
                if (free_these & (1U << bit)) fs_bitmap_clear(ctx, i * 8 + bit);
                if (mark_these & (1U << bit)) fs_bitmap_set(ctx, i * 8 + bit);
            }
        }
    }
    if (c->repair)
        fs_bitmap_flush(ctx);

    char text[80];
    if (lost > 0) {
        u64tos(lost, text);
        strcat(text, lost == 1 ? " lost block" : " lost blocks");
        strcat(text, c->repair ? " (allocated, used by nothing), freed" : " (allocated, used by nothing)");
        check_problem(c, "", text, true);
    }
    if (unmarked > 0) {
        u64tos(unmarked, text);
        strcat(text, unmarked == 1 ? " block in use but marked free" : " blocks in use but marked free");
        check_problem(c, "", text, true);
    }
}

//
// Entry point
//

VfsCheckResult fs_device_check(struct FSDeviceContext* ctx, bool repair, VfsCheckReport* report) {
    if (ctx == NULL || !ctx->is_open)
        return VFS_CHECK_UNFORMATTED;

    struct FSPartitionBlock partition;
    if (fs_device_get_partition(ctx, &partition) != 0)
        return VFS_CHECK_UNFORMATTED;

    Check c;
    memset(&c, 0, sizeof(Check));
    c.ctx    = ctx;
    c.repair = repair;
    c.report = report;
    c.root   = partition.root_directory;
    c.visits_left = ctx->block_count;

    report->block_size   = ctx->sector_size;
    report->blocks_total = ctx->block_count;

    uint64_t claims_size = ctx->bitmap_size;
    if (claims_size > 0 && claims_size <= CHECK_CLAIMS_MAX) {
        c.claims = malloc((size_t)claims_size);
        if (c.claims != NULL) {
            memset(c.claims, 0, (size_t)claims_size);
            for (uint64_t b = 0; b < ctx->reserved_blocks && b < ctx->block_count; b++)
                c.claims[b >> 3] |= (uint8_t)(1U << (b & 7U));
            c.claimed = ctx->reserved_blocks;
        }
    }

    // The root: nothing below it can be checked without it
    if (check_claim(&c, c.root, sizeof(struct FSDirectoryHeader)) != CLAIM_OK ||
        !fs_check_directory_valid(ctx, c.root)) {
        check_problem(&c, "/", "root folder damaged, the volume has to be formatted", false);
        free(c.claims);
        return VFS_CHECK_OK;
    }

    // Its parent is FS_NULL (itself or 0 on volumes from older builds)
    uint64_t parent = check_parent_of(&c, c.root);
    if (parent != FS_NULL && parent != c.root && parent != 0) {
        check_problem(&c, "/", "wrong parent link", true);
        if (repair) {
            struct FSBlockHeader block;
            fs_mem_read(ctx, c.root, &block, sizeof(struct FSBlockHeader));
            block.parent = FS_NULL;
            fs_mem_write(ctx, c.root, &block, sizeof(struct FSBlockHeader));
        }
    }

    check_push(&c, c.root);
    while (c.stack_count > 0 && !c.out_of_memory && !c.looped)
        check_directory(&c, c.stack[--c.stack_count]);
    free(c.stack);

    VfsCheckResult result = VFS_CHECK_OK;
    if (c.out_of_memory) {
        result = VFS_CHECK_NO_MEMORY;
    } else if (c.looped) {
        check_problem(&c, "", "folder tree loops back on itself, check stopped", false);
    } else if (c.claims != NULL) {
        check_bitmap(&c);
        report->blocks_checked = true;
        report->blocks_used    = c.claimed;
    }

    free(c.claims);
    return result;
}
