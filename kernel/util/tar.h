#ifndef KERNEL_UTIL_TAR_H
#define KERNEL_UTIL_TAR_H

#include <stdint.h>
#include <stdbool.h>

// Read-only walk over a POSIX ustar archive held in memory.
//
// Only what build-assets.sh writes is understood: 512-byte headers with the
// "ustar" magic, regular files ('0' or NUL) and directories ('5'), names up
// to 255 bytes through the prefix field. Any other entry type (links, pax
// or GNU long-name headers, devices) is returned with its type so the
// caller can skip it. Nothing is copied: an entry's data points into the
// archive.
//
// Every header is checksummed and bounds-checked against the archive size,
// and names with a ".." component are rejected, so a damaged or hostile
// archive can only stop the walk early, never read past the buffer or name
// a path outside the folder it is extracted into.

#define TAR_BLOCK_SIZE  512
#define TAR_NAME_MAX    256     // prefix + '/' + name + NUL

#define TAR_TYPE_FILE   '0'     // A NUL type byte is reported as '0' too
#define TAR_TYPE_DIR    '5'

#define TAR_MODE_EXEC   0111    // Any execute bit

typedef enum {
    TAR_END = 0,                // End of archive (two zero blocks, or no room left)
    TAR_ENTRY,                  // *entry is filled in
    TAR_BROKEN,                 // Bad magic, checksum, size or name: stop here
} TarResult;

struct TarEntry {
    char           name[TAR_NAME_MAX];  // "sys/dwm": no leading "./" or '/', no trailing '/'
    char           type;                // TAR_TYPE_*, or another ustar type byte
    uint32_t       mode;                // Permission bits (e.g. 0755)
    const uint8_t* data;                // size bytes inside the archive
    uint32_t       size;
};

typedef struct {
    const uint8_t* base;
    uint32_t       size;
    uint32_t       offset;              // Next header
} TarReader;

void tar_open(TarReader* reader, const void* base, uint32_t size);

TarResult tar_next(TarReader* reader, struct TarEntry* entry);

// True if `base` starts with a valid ustar header (a cheap sanity check
// before trusting a block of memory to be an archive)
bool tar_check(const void* base, uint32_t size);

#endif
