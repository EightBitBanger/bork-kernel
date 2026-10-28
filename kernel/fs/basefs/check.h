#ifndef BASE_CHECK_H
#define BASE_CHECK_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/vfs/vfs.h>
#include <kernel/fs/basefs/structs.h>

// Consistency check (check.c), the basefs side of vfs_device_check.
//
// Walks the tree from the root directory, checking every directory's records
// and extent chain, every file's data chain, the parent links and entry
// types, and that no block is used twice. Then compares the blocks the tree
// uses with the allocation bitmap: blocks marked used that nothing uses are
// lost (a crash between allocating and linking), and blocks in use that are
// marked free would be handed out again. With `repair` everything found is
// fixed; without it, nothing is written.
//
// Repairs never guess at lost data. A damaged record or link is cut off,
// an entry that points at nothing (or at something linked elsewhere) is
// removed, sizes are set to the data that is really there, and blocks no
// longer reachable are freed.
VfsCheckResult fs_device_check(struct FSDeviceContext* ctx, bool repair, VfsCheckReport* report);

// Called for each extent of a chain before it is followed. False when it is
// damaged or belongs to something else: the chain is cut in front of it.
typedef bool (*fs_check_extent_fn)(void* user, uint64_t extent);

// Called for each live directory entry (the record at `record`). False to
// have it removed when repairing.
typedef bool (*fs_check_entry_fn)(void* user, uint64_t directory, uint64_t record,
                                  const struct FSDirEntry* entry);

// What fs_directory_check found (and, repairing, fixed)
typedef struct {
    uint32_t bad_records;   // Damaged record stretches (cleared)
    bool     bad_chain;     // Extent chain damaged or looping (cut)
    bool     bad_links;     // Back links or the tail hint wrong (rewritten)
    bool     bad_counts;    // Entry counts wrong (recounted)
} FSDirCheck;

// What fs_file_check_chain found (and, repairing, fixed)
typedef struct {
    bool bad_chain;         // A data extent is damaged or shared (file cut short there)
    bool bad_links;         // Back links wrong (rewritten)
    bool bad_size;          // Size does not match the data (set to what is there)
} FSFileCheck;

#endif
