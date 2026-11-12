#include <stddef.h>
#include <kernel/util/tar.h>

// ustar header layout (POSIX.1-1988), offsets into the 512-byte block
#define TAR_OFF_NAME        0
#define TAR_LEN_NAME        100
#define TAR_OFF_MODE        100
#define TAR_LEN_MODE        8
#define TAR_OFF_SIZE        124
#define TAR_LEN_SIZE        12
#define TAR_OFF_CHKSUM      148
#define TAR_LEN_CHKSUM      8
#define TAR_OFF_TYPE        156
#define TAR_OFF_MAGIC       257     // "ustar" then NUL (POSIX) or ' ' (old GNU)
#define TAR_OFF_PREFIX      345
#define TAR_LEN_PREFIX      155

// Octal number field: optional leading spaces, digits, then NUL or space.
// False on anything else, including the base-256 form (high bit set) and
// values that do not fit in 32 bits.
static bool tar_octal(const uint8_t* field, uint32_t len, uint32_t* out) {
    uint64_t value = 0;
    uint32_t i = 0;

    while (i < len && field[i] == ' ')
        i++;

    bool digits = false;
    for (; i < len; i++) {
        uint8_t c = field[i];
        if (c == '\0' || c == ' ')
            break;
        if (c < '0' || c > '7')
            return false;
        value = (value << 3) | (uint64_t)(c - '0');
        if (value > 0xFFFFFFFFULL)
            return false;
        digits = true;
    }

    *out = digits ? (uint32_t)value : 0;
    return true;
}

static bool tar_zero_block(const uint8_t* block) {
    for (uint32_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (block[i] != 0)
            return false;
    }
    return true;
}

// Unsigned sum of the header with the checksum field counted as spaces
static bool tar_checksum_ok(const uint8_t* block) {
    uint32_t stored = 0;
    if (!tar_octal(block + TAR_OFF_CHKSUM, TAR_LEN_CHKSUM, &stored))
        return false;

    uint32_t sum = 0;
    for (uint32_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (i >= TAR_OFF_CHKSUM && i < TAR_OFF_CHKSUM + TAR_LEN_CHKSUM)
            sum += ' ';
        else
            sum += block[i];
    }
    return sum == stored;
}

static bool tar_magic_ok(const uint8_t* block) {
    const char* magic = "ustar";
    for (uint32_t i = 0; i < 5; i++) {
        if (block[TAR_OFF_MAGIC + i] != (uint8_t)magic[i])
            return false;
    }
    return true;
}

// Append up to `len` bytes of a possibly unterminated field to out[*pos]
static bool tar_append(char* out, uint32_t* pos, const uint8_t* field, uint32_t len) {
    for (uint32_t i = 0; i < len && field[i] != '\0'; i++) {
        if (*pos + 1 >= TAR_NAME_MAX)
            return false;
        out[(*pos)++] = (char)field[i];
    }
    out[*pos] = '\0';
    return true;
}

// "prefix/name" with leading "./" and '/' and trailing '/' removed. False
// if it is too long or has a ".." component. May leave `out` empty (the
// "./" entry tar writes for the folder it was run in).
static bool tar_build_name(const uint8_t* block, char* out) {
    char raw[TAR_NAME_MAX];
    uint32_t pos = 0;
    raw[0] = '\0';

    if (block[TAR_OFF_PREFIX] != '\0') {
        if (!tar_append(raw, &pos, block + TAR_OFF_PREFIX, TAR_LEN_PREFIX))
            return false;
        if (pos + 1 >= TAR_NAME_MAX)
            return false;
        raw[pos++] = '/';
        raw[pos] = '\0';
    }
    if (!tar_append(raw, &pos, block + TAR_OFF_NAME, TAR_LEN_NAME))
        return false;

    // Copy component by component, dropping empty and "." ones
    const char* p = raw;
    uint32_t o = 0;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        const char* start = p;
        while (*p != '\0' && *p != '/')
            p++;
        uint32_t len = (uint32_t)(p - start);

        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.')
            return false;

        if (o > 0)
            out[o++] = '/';
        for (uint32_t i = 0; i < len; i++)
            out[o++] = start[i];
    }
    out[o] = '\0';
    return true;
}

void tar_open(TarReader* reader, const void* base, uint32_t size) {
    reader->base   = (const uint8_t*)base;
    reader->size   = (base != NULL) ? size : 0;
    reader->offset = 0;
}

TarResult tar_next(TarReader* reader, struct TarEntry* entry) {
    for (;;) {
        if (reader->offset > reader->size || reader->size - reader->offset < TAR_BLOCK_SIZE)
            return TAR_END;

        const uint8_t* block = reader->base + reader->offset;
        if (tar_zero_block(block))
            return TAR_END;

        if (!tar_magic_ok(block) || !tar_checksum_ok(block))
            return TAR_BROKEN;

        uint32_t size = 0;
        uint32_t mode = 0;
        if (!tar_octal(block + TAR_OFF_SIZE, TAR_LEN_SIZE, &size) ||
            !tar_octal(block + TAR_OFF_MODE, TAR_LEN_MODE, &mode))
            return TAR_BROKEN;

        // Data blocks must lie inside the archive (64-bit: no wrap-around)
        uint64_t data_start = (uint64_t)reader->offset + TAR_BLOCK_SIZE;
        uint64_t data_end   = data_start + (((uint64_t)size + TAR_BLOCK_SIZE - 1) & ~(uint64_t)(TAR_BLOCK_SIZE - 1));
        if (data_start + size > reader->size)
            return TAR_BROKEN;

        char type = (char)block[TAR_OFF_TYPE];
        if (type == '\0')
            type = TAR_TYPE_FILE;

        if (!tar_build_name(block, entry->name))
            return TAR_BROKEN;

        // The last entry's padding may be cut off; the next call then ends
        reader->offset = (data_end > reader->size) ? reader->size : (uint32_t)data_end;

        // "./" names the extraction folder itself: nothing to report
        if (entry->name[0] == '\0') {
            if (type != TAR_TYPE_DIR)
                return TAR_BROKEN;
            continue;
        }

        entry->type = type;
        entry->mode = mode & 07777;
        entry->data = reader->base + (uint32_t)data_start;
        entry->size = size;
        return TAR_ENTRY;
    }
}

bool tar_check(const void* base, uint32_t size) {
    if (base == NULL || size < TAR_BLOCK_SIZE)
        return false;
    const uint8_t* block = (const uint8_t*)base;
    return tar_magic_ok(block) && tar_checksum_ok(block);
}
