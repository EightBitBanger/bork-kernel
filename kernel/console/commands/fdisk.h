#ifndef CONSOLE_CMD_FDISK_H
#define CONSOLE_CMD_FDISK_H

#include <stdint.h>
#include <stdbool.h>

#include <kernel/console/display.h>
#include <kernel/util/string.h>
#include <kernel/vfs/vfs.h>
#include <kernel/fs/format.h>
#include <kernel/fs/fs_type.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/arch/x86/bus/pci/pci.h>

// Partition table (MBR) of a whole disk
//
//   fdisk <disk>                     show the partitions and the free space
//                                    between them ("empty")
//   fdisk <disk> new [-d] [-t<fs>] [-M<MB>..]
//                                    add a partition in the first free space
//                                    it fits (one per -M, up to 4 in all). A
//                                    disk with no table gets one. -t sets
//                                    the filesystem it is for (basefs by
//                                    default, or fat32); format it after.
//   fdisk <disk> del <n>             delete partition n; its space stays free
//                                    in place, the others do not move
//   fdisk <disk> grow <n> [-M<MB>]   make partition n longer by MB, into the
//                                    free space right after it
//   fdisk <disk> shrink <n> -M<MB> [-f]
//                                    make partition n shorter by MB
//   fdisk <disk> active <n>          make partition n the boot partition
//
// Without -M, new takes the whole free space it lands in and grow takes all
// the free space after the partition. Partitions never move: grow and shrink
// only change where one ends.
//
// The sectors in front of the first partition hold the boot loader and
// kernel ("boot install"). New partitions start after that boot area (8 MB),
// or at 1 MB with -d (a data disk that will never boot). Once a partition
// starts lower than 8 MB, the space down to it counts as free as well.
//
// Deleting every partition leaves an empty table; the disk then shows in
// /mnt whole again (blank) until a partition is added.
// <disk> is "ata1" or "/mnt/ata1", never a partition.
//
// new, del, grow and shrink release everything mounted from the disk and
// rescan it afterwards, so /mnt matches a fresh boot at once: new partitions
// appear unformatted (run format or boot install on them), and the
// whole-disk entry and deleted partitions are gone. Partitions keep their
// data. A filesystem keeps its own size through grow until it is formatted
// again; shrink refuses to cut into one unless given -f. active only
// rewrites sector 0.

#define FDISK_ALIGN_SECTORS     2048ULL         // 1 MB
#define FDISK_MIN_SECTORS       2048ULL         // Smallest partition fdisk makes
#define FDISK_HEAD_SECTORS      64ULL           // Zeroed at the head of a new partition
#define FDISK_MBR_LIMIT         0xFFFFFFFFULL   // What 32-bit MBR fields reach

static uint8_t fdisk_sector0[MBR_SECTOR_SIZE] __attribute__((aligned(16)));

static const char* msg_fdisk_usage =
    "Usage:\n"
    "  fdisk <disk>                     show the partition table\n"
    "  fdisk <disk> new [-d] [-t<fs>] [-M<MB>..]\n"
    "                                   new partition in the first free space\n"
    "                                   (one per -M; no -M: all of that space;\n"
    "                                   -d: data disk, may use space below 8 MB;\n"
    "                                   -t: basefs (default) or fat32)\n"
    "  fdisk <disk> del <n>             delete partition n, leaving free space\n"
    "  fdisk <disk> grow <n> [-M<MB>]   grow partition n into the space after it\n"
    "  fdisk <disk> shrink <n> -M<MB>   shrink partition n by MB (-f: even if\n"
    "                                   that cuts into its filesystem)\n"
    "  fdisk <disk> active <n>          make partition n the boot partition\n";

static void fdisk_print_u64(uint64_t value) {
    char text[21];
    u64tos(value, text);
    print(text);
}

static void fdisk_print_mb(uint64_t sectors) {
    fdisk_print_u64(sectors / 2048);
    print(" MB");
}

// Print `value` left-aligned in a column `width` wide
static void fdisk_print_column(uint64_t value, uint32_t width) {
    char text[21];
    u64tos(value, text);
    print(text);
    for (size_t i = strlen(text); i < width; i++)
        print(" ");
}

static const char* fdisk_type_name(uint8_t type) {
    switch (type) {
        case MBR_TYPE_BASEFS:         return "basefs ";
        case MBR_TYPE_GPT_PROTECTIVE: return "gpt    ";
        case 0x0B: case 0x0C:         return "fat32  ";
        case 0x04: case 0x06: case 0x0E: return "fat16  ";
        case 0x01:                    return "fat12  ";
        case 0x07:                    return "ntfs   ";
        default:                      return "other  ";
    }
}

static uint64_t fdisk_align_up(uint64_t sector) {
    return (sector + FDISK_ALIGN_SECTORS - 1) / FDISK_ALIGN_SECTORS * FDISK_ALIGN_SECTORS;
}

// The table on `disk`. False if sector 0 holds none; *has_table tells an
// unreadable disk (false returned, *has_table untouched) from one with no
// table.
static bool fdisk_read(struct BlockDevice* disk, struct MbrPartition table[MBR_PARTITION_COUNT],
                       uint8_t* count, bool* has_table) {
    *count = 0;
    if (!block_read(disk, 0, 1, fdisk_sector0)) {
        print("Cannot read sector 0 of ");
        print(disk->name);
        print("\n");
        return false;
    }
    *has_table = block_mbr_parse(fdisk_sector0, disk->sector_count, table, count) &&
                 !vfs_fs_probe(fdisk_sector0, disk->sector_size);
    if (!*has_table)
        *count = 0;
    return true;
}

//
// Free space
//
// Every entry with a length counts as taken, a bad range included (clamped
// to the disk), so nothing new is ever placed over it.
//

static uint64_t fdisk_end(const struct MbrPartition* p, uint64_t disk_sectors) {
    if (p->start >= disk_sectors)
        return disk_sectors;
    return (p->count > disk_sectors - p->start) ? disk_sectors : p->start + p->count;
}

// Where free space may begin: after the boot area, or lower when a partition
// already starts lower (a data disk) or `data` (-d) says the disk never boots
static uint64_t fdisk_low_bound(const struct MbrPartition* table, uint8_t count, bool data) {
    uint64_t low = data ? VFS_PARTITION_START_DATA : VFS_PARTITION_START_BOOT;
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].count != 0 && table[i].start < low)
            low = table[i].start;
    }
    return (low < FDISK_ALIGN_SECTORS) ? FDISK_ALIGN_SECTORS : low;
}

// Start of the first partition at or after `sector`, or the end of what an
// MBR can describe on the disk when none follows
static uint64_t fdisk_next_start(const struct MbrPartition* table, uint8_t count,
                                 uint64_t sector, uint64_t disk_sectors) {
    uint64_t limit = (disk_sectors > FDISK_MBR_LIMIT) ? FDISK_MBR_LIMIT : disk_sectors;
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].count != 0 && table[i].start >= sector && table[i].start < limit)
            limit = table[i].start;
    }
    return limit;
}

// True if `sector` lies inside a partition; *end is where that one ends
static bool fdisk_occupied(const struct MbrPartition* table, uint8_t count, uint64_t sector,
                           uint64_t disk_sectors, uint64_t* end) {
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].count == 0)
            continue;
        uint64_t e = fdisk_end(&table[i], disk_sectors);
        if (sector >= table[i].start && sector < e) {
            *end = e;
            return true;
        }
    }
    return false;
}

// The first free stretch of at least 1 MB at or after `*cursor`, 1 MB
// aligned, as [*start, *end). False when there is none. Moves *cursor past it.
static bool fdisk_next_free(const struct MbrPartition* table, uint8_t count, uint64_t disk_sectors,
                            uint64_t* cursor, uint64_t* start, uint64_t* end) {
    uint64_t sector = *cursor;
    while (sector < disk_sectors) {
        uint64_t past;
        if (fdisk_occupied(table, count, sector, disk_sectors, &past)) {
            sector = past;
            continue;
        }
        uint64_t aligned = fdisk_align_up(sector);
        uint64_t limit = fdisk_next_start(table, count, sector, disk_sectors);
        if (aligned < limit && limit - aligned >= FDISK_MIN_SECTORS) {
            *start  = aligned;
            *end    = limit;
            *cursor = limit;
            return true;
        }
        if (limit >= disk_sectors || limit >= FDISK_MBR_LIMIT)
            break;
        sector = limit;
    }
    *cursor = disk_sectors;
    return false;
}

//
// Listing
//

static void fdisk_print_row(char number, bool active, const char* type, uint64_t start, uint64_t sectors) {
    char text[2] = { number, '\0' };
    print("  ");
    print(text);
    print(active ? "  *     " : "        ");
    print(type);
    print("  ");
    fdisk_print_column(start, 12);
    fdisk_print_column(sectors, 12);
    fdisk_print_mb(sectors);
}

static void fdisk_show(struct BlockDevice* disk) {
    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t count = 0;
    bool has_table = false;
    if (!fdisk_read(disk, table, &count, &has_table))
        return;

    print(disk->name);
    print(": ");
    fdisk_print_mb(disk->sector_count);
    print(", ");
    fdisk_print_u64(disk->sector_count);
    print(" sectors\n");

    if (!has_table) {
        print("  no partition table (fdisk ");
        print(disk->name);
        print(" new creates one)\n");
        return;
    }

    print("  signature ");
    print_hex32(block_mbr_signature(fdisk_sector0));
    print("\n\n");

    print("  #  boot  type     start       sectors     size\n");

    // Partitions and the free space between them, in disk order
    uint64_t cursor = fdisk_low_bound(table, count, false);
    for (;;) {
        const struct MbrPartition* next = NULL;
        for (uint8_t i = 0; i < count; i++) {
            const struct MbrPartition* p = &table[i];
            if (p->usable && p->start >= cursor && (next == NULL || p->start < next->start))
                next = p;
        }

        uint64_t free_start, free_end, walk = cursor;
        if (fdisk_next_free(table, count, disk->sector_count, &walk, &free_start, &free_end) &&
            (next == NULL || free_start < next->start)) {
            fdisk_print_row('-', false, "empty  ", free_start, free_end - free_start);
            print("\n");
            cursor = free_end;
            continue;
        }
        if (next == NULL)
            break;

        fdisk_print_row((char)('0' + next->number), next->active, fdisk_type_name(next->type),
                        next->start, next->count);
        print("\n");
        cursor = next->start + next->count;
    }

    // Entries that do not fit the disk or overlap, last
    for (uint8_t i = 0; i < count; i++) {
        const struct MbrPartition* p = &table[i];
        if (p->usable)
            continue;
        fdisk_print_row((char)('0' + p->number), p->active, fdisk_type_name(p->type), p->start, p->count);
        print("  (bad range)\n");
    }
}

//
// Arguments
//

// "-M<MB>" into sectors. False (message printed) if malformed or zero.
static bool fdisk_parse_size(const char* argument, uint64_t* sectors) {
    const char* str = argument + 2;
    uint64_t value = 0;
    while (*str >= '0' && *str <= '9') {
        value = value * 10 + (uint64_t)(*str - '0');
        if (value > 0xFFFFFFFFULL)
            break;
        str++;
    }
    if (str == argument + 2 || *str != '\0' || value == 0) {
        print("Invalid size '");
        print(argument);
        print("'\n");
        return false;
    }
    *sectors = value * 2048ULL;
    return true;
}

// Partition number argument, 1..4. 0 (message printed) if invalid.
static uint8_t fdisk_parse_number(const char* argument) {
    if (argument[0] >= '1' && argument[0] <= '4' && argument[1] == '\0')
        return (uint8_t)(argument[0] - '0');
    print("Partition number must be 1 to 4\n");
    return 0;
}

//
// Changes
//

// Write `table`, zero the heads of the partitions in `fresh` (a bit per
// table slot, so they read as unformatted instead of showing whatever was
// there before), then rescan the disk so /mnt matches the new table. With
// `release`, every mount on the disk is released first.
static int fdisk_commit(struct BlockDevice* disk, const struct MbrPartition* table, uint8_t count,
                        bool keep_code, uint8_t fresh, bool release) {
    if (release) {
        if (vfs_device_release_disk(disk) == VFS_FORMAT_BUSY) {
            print("Files on ");
            print(disk->name);
            print(" are open, close them first\n");
            return 3;
        }
        block_partition_drop_all(disk);
    }

    bool ok = block_mbr_write(disk, table, count, keep_code);
    for (uint8_t i = 0; i < count && ok; i++) {
        if (!(fresh & (1u << (table[i].number - 1))))
            continue;
        uint64_t head = table[i].count < FDISK_HEAD_SECTORS ? table[i].count : FDISK_HEAD_SECTORS;
        ok = block_zero(disk, table[i].start, head, NULL);
    }
    ok = ok && block_flush(disk);

    // Even after a failed write: mount whatever the disk holds now. Entries
    // the scan does not bring back (the whole disk, deleted partitions) are
    // removed from /mnt
    if (release)
        pci_mount_storage(disk);

    if (!ok) {
        print("Write failed, check the table with: fdisk ");
        print(disk->name);
        print("\n");
        return 4;
    }

    fdisk_show(disk);
    return 0;
}

// Read the table for an edit. 0 when there is one, else the exit code
// (message printed).
static int fdisk_load(struct BlockDevice* disk, struct MbrPartition table[MBR_PARTITION_COUNT],
                      uint8_t* count) {
    bool has_table = false;
    if (!fdisk_read(disk, table, count, &has_table))
        return 4;
    if (!has_table) {
        print(disk->name);
        print(" has no partition table (fdisk ");
        print(disk->name);
        print(" new creates one)\n");
        return 2;
    }
    return 0;
}

// Index of partition `number` in `table`, or MBR_PARTITION_COUNT (message
// printed) if it is not there. Unless `any_range`, one with a bad range is
// refused too.
static uint8_t fdisk_find(struct BlockDevice* disk, const struct MbrPartition* table, uint8_t count,
                          uint8_t number, bool any_range) {
    for (uint8_t i = 0; i < count; i++) {
        if (table[i].number != number)
            continue;
        if (!table[i].usable && !any_range) {
            print("Partition ");
            print_int(number);
            print(" does not fit the disk, delete it first\n");
            return MBR_PARTITION_COUNT;
        }
        return i;
    }
    print("No partition ");
    print_int(number);
    print(" on ");
    print(disk->name);
    print("\n");
    return MBR_PARTITION_COUNT;
}

// The first free stretch from `low` on that holds `wanted` sectors (0: take
// all of the first one). False (message printed) when none does.
static bool fdisk_place(struct BlockDevice* disk, const struct MbrPartition* table, uint8_t count,
                        uint64_t low, uint64_t wanted, uint64_t* start, uint64_t* sectors) {
    uint64_t cursor = low, free_start, free_end, largest = 0;
    while (fdisk_next_free(table, count, disk->sector_count, &cursor, &free_start, &free_end)) {
        uint64_t room = free_end - free_start;
        if (wanted == 0 || wanted <= room) {
            *start   = free_start;
            *sectors = (wanted == 0) ? room : wanted;
            return true;
        }
        if (room > largest)
            largest = room;
    }
    if (largest == 0) {
        print("No free space left on ");
        print(disk->name);
        print(" (1 MB minimum), make room with del or shrink\n");
    } else {
        print("Too large, the largest free space is ");
        fdisk_print_mb(largest);
        print("\n");
    }
    return false;
}

static int fdisk_new(struct BlockDevice* disk, int arg_count, char** args) {
    bool data = false;
    uint8_t type = MBR_TYPE_BASEFS;
    uint64_t wanted[MBR_PARTITION_COUNT] = { 0 };
    uint8_t sizes = 0;

    for (int i = 0; i < arg_count; i++) {
        if (strcmp(args[i], "-d") == 0)
            data = true;
        else if (args[i][0] == '-' && args[i][1] == 't') {
            const struct FsType* fs = vfs_fs_type_by_name(&args[i][2]);
            if (fs == NULL || fs->mbr_types[0] == 0) {
                print("Unknown file system '");
                print(&args[i][2]);
                print("'\n");
                return 2;
            }
            type = fs->mbr_types[0];
        } else if (args[i][0] == '-' && args[i][1] == 'M') {
            if (sizes == MBR_PARTITION_COUNT) {
                print("At most 4 partitions\n");
                return 2;
            }
            if (!fdisk_parse_size(args[i], &wanted[sizes]))
                return 2;
            sizes++;
        } else {
            print(msg_fdisk_usage);
            return 1;
        }
    }

    // With no table yet the new one starts empty. Its boot code is kept only
    // if sector 0 is an MBR already: a whole-disk volume's header there must
    // not survive, or the scan would still take the disk for that volume.
    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t count = 0;
    bool has_table = false;
    if (!fdisk_read(disk, table, &count, &has_table))
        return 4;

    uint8_t to_add = (sizes == 0) ? 1 : sizes;
    if (count + to_add > MBR_PARTITION_COUNT) {
        print(count == MBR_PARTITION_COUNT ? "All 4 partition slots are in use\n"
                                           : "Not enough free partition slots (4 at most)\n");
        return 2;
    }

    uint64_t low = fdisk_low_bound(table, count, data);
    bool has_active = false;
    for (uint8_t i = 0; i < count; i++)
        has_active = has_active || table[i].active;

    uint8_t fresh = 0;
    for (uint8_t n = 0; n < to_add; n++) {
        uint64_t start = 0, sectors = 0;
        if (!fdisk_place(disk, table, count, low, wanted[n], &start, &sectors))
            return 2;

        uint8_t used_slots = 0;
        for (uint8_t i = 0; i < count; i++)
            used_slots |= (uint8_t)(1u << (table[i].number - 1));
        uint8_t number = 1;
        while (used_slots & (1u << (number - 1)))
            number++;

        // A table without a boot partition gets one: the first one added
        struct MbrPartition entry = { number, type, !has_active, true, start, sectors };
        has_active = true;
        table[count++] = entry;
        fresh |= (uint8_t)(1u << (number - 1));
    }

    return fdisk_commit(disk, table, count, has_table, fresh, true);
}

static int fdisk_del(struct BlockDevice* disk, uint8_t number) {
    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t count = 0;
    int result = fdisk_load(disk, table, &count);
    if (result != 0)
        return result;
    uint8_t index = fdisk_find(disk, table, count, number, true);
    if (index == MBR_PARTITION_COUNT)
        return 2;

    // The others keep their slots and ranges, so its space is simply free
    for (uint8_t i = index; i + 1 < count; i++)
        table[i] = table[i + 1];
    count--;
    return fdisk_commit(disk, table, count, true, 0, true);
}

static int fdisk_active(struct BlockDevice* disk, uint8_t number) {
    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t count = 0;
    int result = fdisk_load(disk, table, &count);
    if (result != 0)
        return result;
    uint8_t index = fdisk_find(disk, table, count, number, false);
    if (index == MBR_PARTITION_COUNT)
        return 2;

    for (uint8_t i = 0; i < count; i++)
        table[i].active = (i == index);
    // Only sector 0 changes, which no partition covers: mounts stay
    return fdisk_commit(disk, table, count, true, 0, false);
}

// Bytes the formatted volume on partition `number` covers, 0 if it is not
// mounted or not formatted
static uint64_t fdisk_volume_bytes(struct BlockDevice* disk, uint8_t number) {
    char path[32] = "/mnt/";
    strncat(path, disk->name, sizeof(path) - strlen(path) - 4);
    size_t length = strlen(path);
    path[length]     = 'p';
    path[length + 1] = (char)('0' + number);
    path[length + 2] = '\0';

    VfsDeviceInfo info;
    if (!vfs_device_get_info(path, &info) || !info.formatted)
        return 0;
    return info.total_size;
}

// grow <n> [-M<MB>], shrink <n> -M<MB> [-f]: move where partition n ends
static int fdisk_resize(struct BlockDevice* disk, uint8_t number, bool grow, int arg_count, char** args) {
    uint64_t change = 0;
    bool force = false;
    for (int i = 0; i < arg_count; i++) {
        if (args[i][0] == '-' && args[i][1] == 'M') {
            if (!fdisk_parse_size(args[i], &change))
                return 2;
        } else if (!grow && strcmp(args[i], "-f") == 0) {
            force = true;
        } else {
            print(msg_fdisk_usage);
            return 1;
        }
    }
    if (!grow && change == 0) {
        print("Give the size to take off, e.g.: fdisk ");
        print(disk->name);
        print(" shrink ");
        print_int(number);
        print(" -M100\n");
        return 1;
    }

    struct MbrPartition table[MBR_PARTITION_COUNT];
    uint8_t count = 0;
    int result = fdisk_load(disk, table, &count);
    if (result != 0)
        return result;
    uint8_t index = fdisk_find(disk, table, count, number, false);
    if (index == MBR_PARTITION_COUNT)
        return 2;
    struct MbrPartition* p = &table[index];
    uint64_t end = p->start + p->count;

    uint64_t sectors;
    uint64_t volume = fdisk_volume_bytes(disk, number);
    if (grow) {
        uint64_t limit = fdisk_next_start(table, count, end, disk->sector_count);
        if (limit - p->start > FDISK_MBR_LIMIT)
            limit = p->start + FDISK_MBR_LIMIT;
        uint64_t room = (limit > end) ? limit - end : 0;
        if (room == 0) {
            print("No free space right after partition ");
            print_int(number);
            print("\n");
            return 2;
        }
        if (change > room) {
            print("Too large, ");
            fdisk_print_mb(room);
            print(" free right after partition ");
            print_int(number);
            print("\n");
            return 2;
        }
        sectors = p->count + (change == 0 ? room : change);
    } else {
        if (change >= p->count || p->count - change < FDISK_MIN_SECTORS) {
            print("Too small, a partition is at least 1 MB (partition ");
            print_int(number);
            print(" is ");
            fdisk_print_mb(p->count);
            print(")\n");
            return 2;
        }
        sectors = p->count - change;

        // The volume's size is in its own header: it would run past the end
        if (volume > sectors * disk->sector_size && !force) {
            print("The filesystem on partition ");
            print_int(number);
            print(" is ");
            fdisk_print_mb(volume / disk->sector_size);
            print(" and would be cut off.\nAdd -f to shrink anyway, then format it\n");
            return 2;
        }
    }

    p->count = sectors;
    result = fdisk_commit(disk, table, count, true, 0, true);

    if (result == 0 && grow && volume > 0) {
        print("\nThe filesystem keeps its size of ");
        fdisk_print_mb(volume / disk->sector_size);
        print(" until the partition is formatted again\n");
    }
    return result;
}

static int cmd_fdisk_run(int arg_count, char** args) {
    if (arg_count < 1 || args[0][0] == '-') {
        print(msg_fdisk_usage);
        return 1;
    }

    const char* name = args[0];
    if (strncmp(name, "/mnt/", 5) == 0)
        name += 5;

    struct BlockDevice* disk = block_find(name);
    if (disk == NULL) {
        print("No disk named ");
        print(name);
        print("\n");
        return 2;
    }
    if (block_partition_parent(disk) != NULL) {
        print("Need a whole disk (");
        print(block_partition_parent(disk)->name);
        print(", not ");
        print(disk->name);
        print(")\n");
        return 2;
    }
    if (disk->sector_size != MBR_SECTOR_SIZE) {
        print("Only 512-byte sectors are supported\n");
        return 2;
    }

    if (arg_count == 1) {
        fdisk_show(disk);
        return 0;
    }

    const char* action = args[1];
    if (strcmp(action, "new") == 0)
        return fdisk_new(disk, arg_count - 2, args + 2);

    bool del    = strcmp(action, "del") == 0;
    bool active = strcmp(action, "active") == 0;
    bool grow   = strcmp(action, "grow") == 0;
    bool shrink = strcmp(action, "shrink") == 0;
    if (arg_count >= 3 && (del || active || grow || shrink) &&
        (arg_count == 3 || grow || shrink)) {
        uint8_t number = fdisk_parse_number(args[2]);
        if (number == 0)
            return 2;
        if (del)
            return fdisk_del(disk, number);
        if (active)
            return fdisk_active(disk, number);
        return fdisk_resize(disk, number, grow, arg_count - 3, args + 3);
    }

    print(msg_fdisk_usage);
    return 1;
}

int console_cmd_fdisk(int arg_count, char** args) {
    // Never while a program formats a disk (SYS_FORMAT), or the reverse
    if (!format_lock_try()) {
        print("Another format is running, try again when it is done\n");
        console_command_end();
        return 5;
    }
    int result = cmd_fdisk_run(arg_count, args);
    format_lock_release();
    console_command_end();
    return result;
}

#endif
