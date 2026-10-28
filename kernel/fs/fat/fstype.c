// FAT as an FsType: the table the VFS drives FatFs volumes through.
//
// Nodes
//
// The VFS names files by 64-bit node, but FAT has no inode numbers and
// FatFs works on paths ("3:/photos/cat.jpg"). Each mount keeps a node
// table: a node is a number handed out for one path on the volume, and the
// table maps it back. Paths in the table are spelled with each entry's 8.3
// name ("/PHOTOS/CAT~1.JPG"), which is unique in its directory however the
// name was typed: "README", "readme" and the long name all reach the same
// node. (FatFs echoes a looked-up long name back as it was typed, so the
// long name cannot serve as the key.) A rename rewrites the paths in the
// table, the renamed entry's and everything below it, so a node stays valid
// across it.
//
// The table is bounded. When it is full the least recently used node is
// dropped, never one with an open file (or the root); a dropped node no
// longer resolves (node_type says FS_NODE_NONE), and the next lookup of its
// path hands out a fresh one. The VFS resolves paths afresh for every call,
// so it only ever holds nodes it has just used, plus the ones open files
// pin.
//
// Open files
//
// FatFs refuses (FF_FS_LOCK) to open a file for writing that is already
// open, or to rename or delete an open object, so those VFS calls fail on a
// FAT volume while the file is open, rather than corrupting it. Sizes and
// resizes go through the open handle when there is one, because FatFs only
// writes a file's size to its directory entry on sync.
//
// Attributes
//
// FAT stores no security mask or certificate (security 0: anyone), and no
// execute bit. Files report READ, plus WRITE unless the read-only attribute
// is set; FAT_ALLOW_EXECUTE decides whether programs on a FAT volume run.

#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/fs/fs_type.h>
#include <kernel/fs/fat/fat.h>
#include <kernel/block/block.h>
#include <kernel/block/partition.h>
#include <kernel/util/string.h>

#include <kernel/fs/fatfs/source/ff.h>

// 1: every file on a FAT volume is executable. 0: none is, so a stick can
// carry programs but not run them.
#ifndef FAT_ALLOW_EXECUTE
#define FAT_ALLOW_EXECUTE   0
#endif

#define FAT_NODE_MAX        1024
#define FAT_NODE_ROOT       1ULL            // 0 and FS_NODE_NULL are never nodes
#define FAT_PATH_MAX        (VFS_PATH_MAX + 4)  // "N:" + path on the volume

// FAT32 needs this many clusters; fewer and it would be FAT16
#define FAT32_MIN_CLUSTERS  65526ULL

// f_mkfs work area: bigger formats faster (it writes the FATs in runs this long)
#define FAT_MKFS_WORK       (32U * 1024U)

struct FatNode {
    uint64_t id;                    // 0: free slot
    char*    path;                  // On the volume: "" for the root, else "/a/b"
    uint32_t hash;
    uint32_t last_use;
    uint32_t open_count;            // Open files on this node: never dropped
};

struct FatFile;

struct FatMount {
    FATFS    fs;
    int      drive;                 // FatFs drive number, -1 while detached
    bool     mounted;               // f_mount succeeded

    struct FatNode* nodes;          // FAT_NODE_MAX of them
    uint64_t next_id;
    uint32_t clock;

    // Listing cursor: dir_entry(index) carries on from the last entry
    // instead of reading the directory from the start every time
    DIR      cursor;
    uint64_t cursor_dir;            // 0: closed
    uint32_t cursor_index;          // Entry the next f_readdir returns
    int32_t  cursor_count;          // Entries in cursor_dir, -1 unknown

    struct FatFile* files;          // Open handles, linked
};

struct FatFile {
    FIL      fil;
    struct FatMount* fm;
    uint64_t node;
    bool     writable;
    struct FatFile* next;
};

// Scratch shared by every call (the VFS lock makes them one at a time)
static char    fat_path[FAT_PATH_MAX];
static char    fat_path2[FAT_PATH_MAX];
static FILINFO fat_info;

static inline struct FatMount* fat_mount(struct FsMount* m) {
    return (struct FatMount*)m->data;
}

// The name fat_info's entry is keyed by in the node table: its 8.3 name,
// or the long one if the short one has characters FatFs cannot convert
static const char* fat_key(void) {
    return (fat_info.altname[0] != '\0') ? fat_info.altname : fat_info.fname;
}

//
// Node table
//

static uint32_t fat_hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s != '\0')
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void fat_node_free(struct FatNode* n) {
    if (n->path != NULL)
        free(n->path);
    memset(n, 0, sizeof(struct FatNode));
}

static void fat_nodes_clear(struct FatMount* fm) {
    if (fm->nodes == NULL)
        return;
    for (uint32_t i = 0; i < FAT_NODE_MAX; i++)
        fat_node_free(&fm->nodes[i]);
}

static struct FatNode* fat_node_find(struct FatMount* fm, uint64_t id) {
    if (id == 0 || id == FS_NODE_NULL || fm->nodes == NULL)
        return NULL;
    for (uint32_t i = 0; i < FAT_NODE_MAX; i++) {
        if (fm->nodes[i].id == id) {
            fm->nodes[i].last_use = ++fm->clock;
            return &fm->nodes[i];
        }
    }
    return NULL;
}

// The node for `path` (on the volume), adding it if it has none
static uint64_t fat_node_intern(struct FatMount* fm, const char* path) {
    if (path[0] == '\0')
        return FAT_NODE_ROOT;
    if (fm->nodes == NULL)
        return FS_NODE_NULL;

    uint32_t hash = fat_hash(path);
    struct FatNode* slot = NULL;
    for (uint32_t i = 0; i < FAT_NODE_MAX; i++) {
        struct FatNode* n = &fm->nodes[i];
        if (n->id == 0) {
            if (slot == NULL || slot->id != 0)
                slot = n;
            continue;
        }
        if (n->hash == hash && strcmp(n->path, path) == 0) {
            n->last_use = ++fm->clock;
            return n->id;
        }
        // Least recently used node without an open file, in case no slot is free
        if (n->open_count == 0 && (slot == NULL || (slot->id != 0 && n->last_use < slot->last_use)))
            slot = n;
    }
    if (slot == NULL)
        return FS_NODE_NULL;        // Every node has an open file

    size_t length = strlen(path);
    char* copy = malloc(length + 1);
    if (copy == NULL)
        return FS_NODE_NULL;
    memcpy(copy, path, length + 1);

    fat_node_free(slot);
    slot->id       = fm->next_id++;
    slot->path     = copy;
    slot->hash     = hash;
    slot->last_use = ++fm->clock;
    return slot->id;
}

// The path of `node` on the volume, NULL if it is not (or no longer) a node
static const char* fat_node_path(struct FatMount* fm, uint64_t node) {
    if (node == FAT_NODE_ROOT)
        return "";
    struct FatNode* n = fat_node_find(fm, node);
    return (n != NULL) ? n->path : NULL;
}

// `rel` (a path on the volume) as FatFs spells it: "N:/a/b", "N:/" for the root
static bool fat_full_path(struct FatMount* fm, const char* rel, char* out) {
    size_t length = strlen(rel);
    if (length + 4 > FAT_PATH_MAX || fm->drive < 0)
        return false;
    out[0] = (char)('0' + fm->drive);
    out[1] = ':';
    if (length == 0) {
        out[2] = '/';
        out[3] = '\0';
    } else {
        memcpy(&out[2], rel, length + 1);
    }
    return true;
}

static bool fat_node_full_path(struct FatMount* fm, uint64_t node, char* out) {
    const char* rel = fat_node_path(fm, node);
    return rel != NULL && fat_full_path(fm, rel, out);
}

// "<dir path>/<name>" on the volume, into out (FAT_PATH_MAX bytes)
static bool fat_child_path(struct FatMount* fm, uint64_t dir, const char* name, char* out) {
    const char* rel = fat_node_path(fm, dir);
    if (rel == NULL)
        return false;
    size_t dir_len = strlen(rel), name_len = strlen(name);
    if (dir_len + 1 + name_len + 1 > VFS_PATH_MAX)
        return false;
    memcpy(out, rel, dir_len);
    out[dir_len] = '/';
    memcpy(&out[dir_len + 1], name, name_len + 1);
    return true;
}

// The node of fat_info's entry, which is in directory `dir`
static uint64_t fat_node_intern_entry(struct FatMount* fm, uint64_t dir) {
    if (!fat_child_path(fm, dir, fat_key(), fat_path2))
        return FS_NODE_NULL;
    return fat_node_intern(fm, fat_path2);
}

// Paths of `old_path` and everything below it now start with `new_path`
static void fat_nodes_rename(struct FatMount* fm, const char* old_path, const char* new_path) {
    size_t old_len = strlen(old_path), new_len = strlen(new_path);
    for (uint32_t i = 0; i < FAT_NODE_MAX; i++) {
        struct FatNode* n = &fm->nodes[i];
        if (n->id == 0 || strncmp(n->path, old_path, old_len) != 0)
            continue;
        if (n->path[old_len] != '\0' && n->path[old_len] != '/')
            continue;

        size_t rest = strlen(n->path) - old_len;
        char* path = malloc(new_len + rest + 1);
        if (path == NULL) {
            // Cannot follow the rename: drop the node rather than leave it
            // naming whatever ends up at the old path (open files keep theirs)
            if (n->open_count == 0)
                fat_node_free(n);
            continue;
        }
        memcpy(path, new_path, new_len);
        memcpy(&path[new_len], &n->path[old_len], rest + 1);
        free(n->path);
        n->path = path;
        n->hash = fat_hash(path);
    }
}

// Forget `path` and everything below it (it was deleted)
static void fat_nodes_forget(struct FatMount* fm, const char* path) {
    size_t length = strlen(path);
    for (uint32_t i = 0; i < FAT_NODE_MAX; i++) {
        struct FatNode* n = &fm->nodes[i];
        if (n->id == 0 || n->open_count != 0 || strncmp(n->path, path, length) != 0)
            continue;
        if (n->path[length] == '\0' || n->path[length] == '/')
            fat_node_free(n);
    }
}

//
// Listing cursor
//

static void fat_cursor_close(struct FatMount* fm) {
    if (fm->cursor_dir != 0)
        f_closedir(&fm->cursor);
    fm->cursor_dir   = 0;
    fm->cursor_index = 0;
    fm->cursor_count = -1;
}

// Something on the volume changed: entries may have moved
static void fat_changed(struct FatMount* fm) {
    fat_cursor_close(fm);
}

static bool fat_cursor_open(struct FatMount* fm, uint64_t dir) {
    if (fm->cursor_dir == dir && fm->cursor_dir != 0)
        return true;
    fat_cursor_close(fm);
    if (!fat_node_full_path(fm, dir, fat_path) || f_opendir(&fm->cursor, fat_path) != FR_OK)
        return false;
    fm->cursor_dir = dir;
    return true;
}

// Entry `index` of `dir` into fat_info
static bool fat_cursor_entry(struct FatMount* fm, uint64_t dir, uint32_t index) {
    if (!fat_cursor_open(fm, dir))
        return false;
    if (index < fm->cursor_index) {
        if (f_readdir(&fm->cursor, NULL) != FR_OK) {     // Rewind
            fat_cursor_close(fm);
            return false;
        }
        fm->cursor_index = 0;
    }
    while (fm->cursor_index <= index) {
        if (f_readdir(&fm->cursor, &fat_info) != FR_OK || fat_info.fname[0] == '\0') {
            fat_cursor_close(fm);
            return false;
        }
        fm->cursor_index++;
    }
    return true;
}

//
// Open files
//

static struct FatFile* fat_open_writer(struct FatMount* fm, uint64_t node) {
    for (struct FatFile* f = fm->files; f != NULL; f = f->next) {
        if (f->node == node && f->writable)
            return f;
    }
    return NULL;
}

//
// Volume
//

static bool fat_probe(const uint8_t* s, uint32_t sector_size) {
    if (sector_size != FF_MAX_SS || s[510] != 0x55 || s[511] != 0xAA)
        return false;
    if (!((s[0] == 0xEB && s[2] == 0x90) || s[0] == 0xE9))
        return false;                                       // No jump: an MBR, not a boot sector

    uint16_t bytes_per_sector = (uint16_t)(s[11] | (s[12] << 8));
    uint8_t  cluster          = s[13];
    uint16_t reserved         = (uint16_t)(s[14] | (s[15] << 8));
    uint8_t  fats             = s[16];
    uint8_t  media            = s[21];
    uint32_t total            = (uint32_t)(s[19] | (s[20] << 8));
    uint32_t fat_size         = (uint32_t)(s[22] | (s[23] << 8));
    if (total == 0)
        total = (uint32_t)s[32] | ((uint32_t)s[33] << 8) | ((uint32_t)s[34] << 16) | ((uint32_t)s[35] << 24);
    if (fat_size == 0)
        fat_size = (uint32_t)s[36] | ((uint32_t)s[37] << 8) | ((uint32_t)s[38] << 16) | ((uint32_t)s[39] << 24);

    return bytes_per_sector == sector_size &&
           cluster != 0 && (cluster & (cluster - 1)) == 0 &&
           reserved != 0 && (fats == 1 || fats == 2) &&
           (media == 0xF0 || media >= 0xF8) &&
           total != 0 && fat_size != 0;
}

static bool fat_attach(struct FsMount* m, struct BlockDevice* device) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL) {
        fm = malloc(sizeof(struct FatMount));
        if (fm == NULL)
            return false;
        memset(fm, 0, sizeof(struct FatMount));
        fm->nodes = malloc(sizeof(struct FatNode) * FAT_NODE_MAX);
        if (fm->nodes == NULL) {
            free(fm);
            return false;
        }
        memset(fm->nodes, 0, sizeof(struct FatNode) * FAT_NODE_MAX);
        fm->drive = -1;
        fm->cursor_count = -1;
        m->data = fm;
    }

    m->device  = device;
    m->is_open = false;
    if (fm->next_id <= FAT_NODE_ROOT)
        fm->next_id = FAT_NODE_ROOT + 1;

    // Every FatFs drive number taken (FF_VOLUMES FAT volumes mounted):
    // attached, but not open
    if (fm->drive < 0)
        fm->drive = fat_drive_claim(device);
    if (fm->drive < 0)
        return true;

    char root[4] = { (char)('0' + fm->drive), ':', '\0', '\0' };
    fm->mounted = f_mount(&fm->fs, root, 1) == FR_OK;
    m->is_open  = fm->mounted;
    return true;
}

static void fat_detach(struct FsMount* m) {
    struct FatMount* fm = fat_mount(m);
    if (fm != NULL) {
        fat_cursor_close(fm);
        if (fm->drive >= 0) {
            char root[4] = { (char)('0' + fm->drive), ':', '\0', '\0' };
            f_mount(NULL, root, 0);     // Open handles on it go stale (FatFs checks)
            fat_drive_release(fm->drive);
        }
        fm->drive   = -1;
        fm->mounted = false;

        // Nodes named paths on that volume; open handles keep theirs until
        // closed, which then fails harmlessly
        for (uint32_t i = 0; i < FAT_NODE_MAX; i++) {
            if (fm->nodes[i].open_count == 0)
                fat_node_free(&fm->nodes[i]);
        }
    }
    m->device  = NULL;
    m->is_open = false;
}

static void fat_release(struct FsMount* m) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL)
        return;
    if (fm->drive >= 0)
        fat_detach(m);
    while (fm->files != NULL) {
        struct FatFile* f = fm->files;
        fm->files = f->next;
        free(f);
    }
    fat_nodes_clear(fm);
    free(fm->nodes);
    free(fm);
    m->data = NULL;
}

static bool fat_sync(struct FsMount* m) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !fm->mounted)
        return true;
    bool ok = true;
    for (struct FatFile* f = fm->files; f != NULL; f = f->next) {
        if (f->writable && f_sync(&f->fil) != FR_OK)
            ok = false;
    }
    if (m->device != NULL && !block_flush(m->device))
        ok = false;
    return ok;
}

static bool fat_volume_info(struct FsMount* m, FsVolumeInfo* out) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open)
        return false;
    out->block_size = (uint32_t)fm->fs.csize * FF_MAX_SS;
    out->total_size = (uint64_t)(fm->fs.n_fatent - 2) * out->block_size;
    return true;
}

// One call: FatFs reads the free count from FSInfo, or counts the FAT
static bool fat_used_bytes(struct FsMount* m, uint64_t* out, uint32_t* percent) {
    struct FatMount* fm = fat_mount(m);
    if (percent != NULL)
        *percent = 100;
    *out = 0;
    if (fm == NULL || !m->is_open)
        return true;

    DWORD free_clusters = 0;
    FATFS* fs = NULL;
    char root[4] = { (char)('0' + fm->drive), ':', '\0', '\0' };
    if (f_getfree(root, &free_clusters, &fs) != FR_OK)
        return true;
    uint64_t total = fm->fs.n_fatent - 2;
    if (free_clusters > total)
        free_clusters = (DWORD)total;
    *out = (total - free_clusters) * fm->fs.csize * FF_MAX_SS;
    return true;
}

static uint64_t fat_max_capacity(struct BlockDevice* device) {
    if (device->sector_size != FF_MAX_SS)
        return 0;
    uint64_t sectors = device->sector_count;
    if (sectors > 0xFFFFFFFFULL)
        sectors = 0xFFFFFFFFULL;    // 32-bit sector numbers (FF_LBA64 off)
    return sectors * FF_MAX_SS;
}

// Cluster size by volume size, as Windows picks it for FAT32
static uint32_t fat_default_block_size(struct BlockDevice* device, uint64_t capacity) {
    (void)device;
    const uint64_t MB = 1024ULL * 1024ULL;
    if (capacity <= 64 * MB)    return 512;
    if (capacity <= 128 * MB)   return 1024;
    if (capacity <= 256 * MB)   return 2048;
    if (capacity <= 8192 * MB)  return 4096;
    if (capacity <= 16384 * MB) return 8192;
    if (capacity <= 32768 * MB) return 16384;
    return 32768;
}

// FAT32 only: enough clusters of block_size, within 32-bit FAT entries
static bool fat_format_check(struct BlockDevice* device, uint64_t capacity, uint32_t block_size) {
    if (device->sector_size != FF_MAX_SS)
        return false;
    if (block_size < FF_MAX_SS || block_size > 32768 || (block_size & (block_size - 1)) != 0)
        return false;
    if (capacity > fat_max_capacity(device))
        return false;
    uint64_t clusters = capacity / block_size;
    // Rough room for the reserved sectors and two FATs at 4 bytes a cluster
    uint64_t overhead = (32 * FF_MAX_SS + 2 * 4 * clusters) / block_size + 1;
    return clusters > overhead && clusters - overhead >= FAT32_MIN_CLUSTERS &&
           clusters < 0x0FFFFFF5ULL;
}

static bool fat_format(struct FsMount* m, struct BlockDevice* device, uint64_t capacity, uint32_t block_size) {
    (void)m;
    if (!fat_format_check(device, capacity, block_size))
        return false;

    int drive = fat_drive_claim(device);
    if (drive < 0)
        return false;
    void* work = malloc(FAT_MKFS_WORK);
    if (work == NULL) {
        fat_drive_release(drive);
        return false;
    }

    // No partition table of its own (FM_SFD): a partition already has one
    // around it, and a whole disk becomes one volume like basefs does
    fat_drive_set_limit(drive, (uint32_t)(capacity / FF_MAX_SS));
    MKFS_PARM params = { FM_FAT32 | FM_SFD, 2, 0, 0, block_size };
    char root[4] = { (char)('0' + drive), ':', '\0', '\0' };
    FRESULT result = f_mkfs(root, &params, work, FAT_MKFS_WORK);

    free(work);
    fat_drive_release(drive);
    return result == FR_OK && block_flush(device);
}

//
// Nodes
//

static uint64_t fat_root(struct FsMount* m) {
    return m->is_open ? FAT_NODE_ROOT : FS_NODE_NULL;
}

// fat_info for `node` (not the root, which FatFs cannot stat)
static bool fat_stat(struct FatMount* fm, uint64_t node) {
    return fat_node_full_path(fm, node, fat_path) && f_stat(fat_path, &fat_info) == FR_OK;
}

static uint8_t fat_node_type(struct FsMount* m, uint64_t node) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open)
        return FS_NODE_NONE;
    if (node == FAT_NODE_ROOT)
        return FS_NODE_DIRECTORY;
    if (!fat_stat(fm, node))
        return FS_NODE_NONE;
    return (fat_info.fattrib & AM_DIR) ? FS_NODE_DIRECTORY : FS_NODE_FILE;
}

static uint64_t fat_parent(struct FsMount* m, uint64_t node) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || node == FAT_NODE_ROOT)
        return FS_NODE_NULL;
    const char* path = fat_node_path(fm, node);
    if (path == NULL)
        return FS_NODE_NULL;

    const char* slash = strrchr(path, '/');
    size_t length = (slash != NULL) ? (size_t)(slash - path) : 0;
    if (length >= sizeof(fat_path2))
        return FS_NODE_NULL;
    memcpy(fat_path2, path, length);
    fat_path2[length] = '\0';
    return fat_node_intern(fm, fat_path2);
}

static uint64_t fat_lookup(struct FsMount* m, uint64_t dir, const char* name) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open || !fat_child_path(fm, dir, name, fat_path2))
        return FS_NODE_NULL;
    if (!fat_full_path(fm, fat_path2, fat_path) || f_stat(fat_path, &fat_info) != FR_OK)
        return FS_NODE_NULL;

    // Keyed by the 8.3 name, so every spelling of it maps to one node
    return fat_node_intern_entry(fm, dir);
}

static uint32_t fat_dir_count(struct FsMount* m, uint64_t dir) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open)
        return 0;
    if (fm->cursor_dir == dir && fm->cursor_count >= 0)
        return (uint32_t)fm->cursor_count;

    if (!fat_cursor_open(fm, dir))
        return 0;
    if (f_readdir(&fm->cursor, NULL) != FR_OK) {
        fat_cursor_close(fm);
        return 0;
    }
    uint32_t count = 0;
    while (f_readdir(&fm->cursor, &fat_info) == FR_OK && fat_info.fname[0] != '\0')
        count++;
    f_readdir(&fm->cursor, NULL);
    fm->cursor_index = 0;
    fm->cursor_count = (int32_t)count;
    return count;
}

static bool fat_dir_entry(struct FsMount* m, uint64_t dir, uint32_t index,
                          uint64_t* node, char* name, size_t name_size) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open || !fat_cursor_entry(fm, dir, index))
        return false;

    size_t length = strlen(fat_info.fname);
    if (name != NULL) {
        if (length + 1 > name_size)
            return false;
        memcpy(name, fat_info.fname, length + 1);
    }
    if (node != NULL) {
        *node = fat_node_intern_entry(fm, dir);
        if (*node == FS_NODE_NULL)
            return false;
    }
    return true;
}

// FatFs quietly drops trailing dots and spaces from a new name, so the
// entry would not be called what the VFS asked for: refuse those. Other
// characters FAT forbids FatFs refuses itself (FR_INVALID_NAME).
static bool fat_name_ok(const char* name) {
    size_t length = strlen(name);
    return length > 0 && name[length - 1] != '.' && name[length - 1] != ' ';
}

static uint64_t fat_create(struct FsMount* m, uint64_t dir, const char* name,
                           uint8_t node_type, uint8_t permissions) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open || !fat_name_ok(name))
        return FS_NODE_NULL;
    if (!fat_child_path(fm, dir, name, fat_path2) || !fat_full_path(fm, fat_path2, fat_path))
        return FS_NODE_NULL;
    fat_changed(fm);

    FRESULT result;
    if (node_type == FS_NODE_DIRECTORY) {
        result = f_mkdir(fat_path);
    } else if (node_type == FS_NODE_FILE) {
        FIL* fil = malloc(sizeof(FIL));
        if (fil == NULL)
            return FS_NODE_NULL;
        result = f_open(fil, fat_path, FA_CREATE_NEW | FA_WRITE);
        if (result == FR_OK)
            result = f_close(fil);
        free(fil);
    } else {
        return FS_NODE_NULL;
    }
    if (result != FR_OK)
        return FS_NODE_NULL;

    if (!(permissions & VFS_PERMISSION_WRITE))
        f_chmod(fat_path, AM_RDO, AM_RDO);
    if (f_stat(fat_path, &fat_info) != FR_OK)
        return FS_NODE_NULL;
    return fat_node_intern_entry(fm, dir);
}

// Delete the directory at fat_path2 (a path on the volume) with everything
// in it, without recursion: go down to an entry, delete it if it is a file
// or an empty directory, and start again from where that left off
static FRESULT fat_remove_tree(struct FatMount* fm) {
    size_t base = strlen(fat_path2);
    DIR* dir = malloc(sizeof(DIR));
    if (dir == NULL)
        return FR_NOT_ENOUGH_CORE;

    FRESULT result = FR_OK;
    for (;;) {
        if (!fat_full_path(fm, fat_path2, fat_path)) {
            result = FR_INVALID_NAME;
            break;
        }
        result = f_opendir(dir, fat_path);
        if (result != FR_OK)
            break;
        result = f_readdir(dir, &fat_info);
        f_closedir(dir);
        if (result != FR_OK)
            break;

        size_t length = strlen(fat_path2);
        if (fat_info.fname[0] == '\0') {
            // Empty: delete it, then carry on in its parent (or stop at the top)
            result = f_unlink(fat_path);
            if (result != FR_OK || length == base)
                break;
            char* slash = strrchr(fat_path2, '/');
            *slash = '\0';
            continue;
        }

        const char* name = fat_key();
        size_t name_len = strlen(name);
        if (length + 1 + name_len + 1 > VFS_PATH_MAX) {
            result = FR_INVALID_NAME;
            break;
        }
        fat_path2[length] = '/';
        memcpy(&fat_path2[length + 1], name, name_len + 1);

        if (fat_info.fattrib & AM_DIR)
            continue;                       // Go down into it
        if (!fat_full_path(fm, fat_path2, fat_path)) {
            result = FR_INVALID_NAME;
            break;
        }
        result = f_unlink(fat_path);
        fat_path2[length] = '\0';
        if (result != FR_OK)
            break;
    }
    free(dir);
    return result;
}

static bool fat_remove(struct FsMount* m, uint64_t dir, uint64_t node) {
    (void)dir;
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open || node == FAT_NODE_ROOT)
        return false;
    const char* path = fat_node_path(fm, node);
    if (path == NULL || strlen(path) >= sizeof(fat_path2))
        return false;
    strcpy(fat_path2, path);
    fat_changed(fm);

    if (!fat_full_path(fm, fat_path2, fat_path) || f_stat(fat_path, &fat_info) != FR_OK)
        return false;
    FRESULT result = (fat_info.fattrib & AM_DIR) ? fat_remove_tree(fm) : f_unlink(fat_path);

    // Whatever got deleted before a failure is gone either way
    strcpy(fat_path2, path);
    fat_nodes_forget(fm, fat_path2);
    return result == FR_OK;
}

static bool fat_rename(struct FsMount* m, uint64_t dir, uint64_t node, const char* name) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open || node == FAT_NODE_ROOT || !fat_name_ok(name))
        return false;
    const char* old_rel = fat_node_path(fm, node);
    if (old_rel == NULL || !fat_child_path(fm, dir, name, fat_path2))
        return false;

    // Both with the drive prefix (FatFs takes the drive from the old one)
    char old_full[FAT_PATH_MAX];
    if (!fat_full_path(fm, old_rel, old_full) || !fat_full_path(fm, fat_path2, fat_path))
        return false;
    fat_changed(fm);
    if (f_rename(old_full, fat_path) != FR_OK)
        return false;

    // The new name has a new 8.3 name: the new key
    if (f_stat(fat_path, &fat_info) != FR_OK || !fat_child_path(fm, dir, fat_key(), fat_path2))
        return false;
    fat_nodes_rename(fm, old_full + 2, fat_path2);
    return true;
}

// Set the size of an open file, through its handle. Growing writes zeroes:
// FatFs would otherwise hand out whatever the new clusters last held.
static bool fat_fil_resize(FIL* fil, FSIZE_t size) {
    FSIZE_t position = f_tell(fil);
    FSIZE_t current  = f_size(fil);
    bool ok = true;

    if (size < current) {
        ok = f_lseek(fil, size) == FR_OK && f_truncate(fil) == FR_OK;
    } else if (size > current) {
        static const uint8_t zeroes[512];
        ok = f_lseek(fil, current) == FR_OK;
        while (ok && current < size) {
            UINT run = (size - current > sizeof(zeroes)) ? sizeof(zeroes) : (UINT)(size - current);
            UINT written = 0;
            ok = f_write(fil, zeroes, run, &written) == FR_OK && written == run;
            current += written;
        }
    }
    if (position > f_size(fil))
        position = f_size(fil);
    f_lseek(fil, position);
    return ok && f_sync(fil) == FR_OK;
}

static bool fat_resize(struct FsMount* m, uint64_t node, uint64_t size) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open || node == FAT_NODE_ROOT || size > 0xFFFFFFFFULL)
        return false;       // FAT files stop at 4 GB - 1

    struct FatFile* open = fat_open_writer(fm, node);
    if (open != NULL)
        return fat_fil_resize(&open->fil, (FSIZE_t)size);

    if (!fat_stat(fm, node) || (fat_info.fattrib & AM_DIR))
        return false;
    FIL* fil = malloc(sizeof(FIL));
    if (fil == NULL)
        return false;
    bool ok = false;
    if (f_open(fil, fat_path, FA_WRITE) == FR_OK) {
        ok = fat_fil_resize(fil, (FSIZE_t)size);
        if (f_close(fil) != FR_OK)
            ok = false;
    }
    free(fil);
    return ok;
}

static uint8_t fat_permissions(BYTE attributes) {
    uint8_t permissions = VFS_PERMISSION_READ;
    if (!(attributes & AM_RDO))
        permissions |= VFS_PERMISSION_WRITE;
    if (FAT_ALLOW_EXECUTE && !(attributes & AM_DIR))
        permissions |= VFS_PERMISSION_EXECUTE;
    return permissions;
}

static bool fat_get_attr(struct FsMount* m, uint64_t node, FsNodeAttr* out) {
    struct FatMount* fm = fat_mount(m);
    memset(out, 0, sizeof(FsNodeAttr));
    if (fm == NULL || !m->is_open)
        return false;
    if (node == FAT_NODE_ROOT) {
        out->permissions = VFS_PERMISSION_READ | VFS_PERMISSION_WRITE;
        return true;
    }
    if (!fat_stat(fm, node))
        return false;

    out->permissions = fat_permissions(fat_info.fattrib);
    out->modified    = fat_time_to_unix(fat_info.fdate, fat_info.ftime);
    out->created     = fat_time_to_unix(fat_info.crdate, fat_info.crtime);
    if (out->created == 0)
        out->created = out->modified;
    if (!(fat_info.fattrib & AM_DIR)) {
        // The entry has the size as of the last sync; a writer knows better
        struct FatFile* open = fat_open_writer(fm, node);
        out->size = (open != NULL) ? f_size(&open->fil) : fat_info.fsize;
    }
    return true;
}

static bool fat_set_permissions(struct FsMount* m, uint64_t node, uint8_t permissions) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open || node == FAT_NODE_ROOT || !fat_node_full_path(fm, node, fat_path))
        return false;
    // Only "may write" has somewhere to go: the read-only attribute
    BYTE attributes = (permissions & VFS_PERMISSION_WRITE) ? 0 : AM_RDO;
    return f_chmod(fat_path, attributes, AM_RDO) == FR_OK;
}

//
// Open files
//

static void* fat_open(struct FsMount* m, uint64_t node, uint16_t mode) {
    struct FatMount* fm = fat_mount(m);
    if (fm == NULL || !m->is_open || node == FAT_NODE_ROOT)
        return NULL;
    struct FatNode* n = fat_node_find(fm, node);
    if (n == NULL || !fat_full_path(fm, n->path, fat_path))
        return NULL;

    struct FatFile* file = malloc(sizeof(struct FatFile));
    if (file == NULL)
        return NULL;
    memset(file, 0, sizeof(struct FatFile));

    BYTE fa = 0;
    if (mode & VFS_OPEN_READ)  fa |= FA_READ;
    if (mode & VFS_OPEN_WRITE) fa |= FA_WRITE;
    if (fa == 0)               fa = FA_READ;

    if (f_open(&file->fil, fat_path, fa | FA_OPEN_EXISTING) != FR_OK) {
        free(file);
        return NULL;
    }
    if ((mode & VFS_OPEN_APPEND) && f_lseek(&file->fil, f_size(&file->fil)) != FR_OK) {
        f_close(&file->fil);
        free(file);
        return NULL;
    }

    file->fm       = fm;
    file->node     = node;
    file->writable = (fa & FA_WRITE) != 0;
    file->next     = fm->files;
    fm->files      = file;
    n->open_count++;
    return file;
}

static void fat_close(void* handle) {
    struct FatFile* file = (struct FatFile*)handle;
    struct FatMount* fm = file->fm;

    f_close(&file->fil);        // Fails harmlessly if the volume went away

    for (struct FatFile** p = &fm->files; *p != NULL; p = &(*p)->next) {
        if (*p == file) {
            *p = file->next;
            break;
        }
    }
    for (uint32_t i = 0; i < FAT_NODE_MAX; i++) {
        struct FatNode* n = &fm->nodes[i];
        if (n->id == file->node && n->open_count > 0) {
            n->open_count--;
            // Detached meanwhile: the node outlived its volume only for this
            if (n->open_count == 0 && fm->drive < 0)
                fat_node_free(n);
            break;
        }
    }
    free(file);
}

static uint32_t fat_read(void* handle, void* buffer, uint32_t size) {
    struct FatFile* file = (struct FatFile*)handle;
    UINT read = 0;
    f_read(&file->fil, buffer, size, &read);
    return read;
}

static uint32_t fat_write(void* handle, const void* buffer, uint32_t size) {
    struct FatFile* file = (struct FatFile*)handle;
    UINT written = 0;
    if (file->writable)
        f_write(&file->fil, buffer, size, &written);
    return written;
}

// Seeking past the end of a writable file grows it (with zeroes); a
// read-only one stops at its end
static uint64_t fat_seek(void* handle, uint64_t position) {
    struct FatFile* file = (struct FatFile*)handle;
    if (position > 0xFFFFFFFFULL)
        position = 0xFFFFFFFFULL;
    if (file->writable && position > f_size(&file->fil)) {
        if (!fat_fil_resize(&file->fil, (FSIZE_t)position))
            return f_tell(&file->fil);
    }
    f_lseek(&file->fil, (FSIZE_t)position);
    return f_tell(&file->fil);
}

static uint64_t fat_tell(void* handle) {
    return f_tell(&((struct FatFile*)handle)->fil);
}

const struct FsType fs_type_fat = {
    .name               = "fat",
    .alias              = "fat32",
    // FAT32 (LBA, CHS), FAT16 (LBA, large, small), FAT12
    .mbr_types          = { 0x0C, 0x0B, 0x0E, 0x06, 0x04, 0x01 },
    .probe              = fat_probe,

    .attach             = fat_attach,
    .detach             = fat_detach,
    .release            = fat_release,
    .sync               = fat_sync,
    .volume_info        = fat_volume_info,
    .used_bytes         = fat_used_bytes,
    .max_capacity       = fat_max_capacity,
    .default_block_size = fat_default_block_size,
    .format_check       = fat_format_check,
    .format             = fat_format,
    .check              = NULL,

    .root               = fat_root,
    .node_type          = fat_node_type,
    .parent             = fat_parent,
    .lookup             = fat_lookup,
    .dir_count          = fat_dir_count,
    .dir_entry          = fat_dir_entry,
    .create             = fat_create,
    .remove             = fat_remove,
    .rename             = fat_rename,
    .resize             = fat_resize,
    .get_attr           = fat_get_attr,
    .set_permissions    = fat_set_permissions,
    .set_security       = NULL,
    .set_certificate    = NULL,

    .open               = fat_open,
    .close              = fat_close,
    .read               = fat_read,
    .write              = fat_write,
    .seek               = fat_seek,
    .tell               = fat_tell,
};
