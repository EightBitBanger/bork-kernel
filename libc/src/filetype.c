/*
 * File types from file headers (os/filetype.h).
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/filetype.h>
#include <os/os.h>
#include <os/process.h>
#include <os/registry.h>

/*
 * The system's associations, most specific first. Icons are names in
 * <home>/sys/images; every one falls back to an icon the desktop has
 * always shipped, so a missing image never shows as a blank square.
 */
#define IMAGE_OPEN  .open = "paint", .viewer = true
// Sound: the media player, which only reads the file
#define SOUND_OPEN  .open = "mediaplay", .viewer = true

static const filetype_assoc_t default_items[] = {
    // Programs and system files
    { .type = "Program",  .key = "elf",  .icon = "icon_executable", .fallback = "icon_system",   FT_AT(0, "\x7f" "ELF") },
    { .type = "Script",   .key = "sh",   .icon = "icon_script",     .fallback = "icon_document", .open = "console --script", FT_AT(0, "#!") },
    { .type = "Driver",   .key = "drv",  .icon = "icon_driver",     .fallback = "icon_system",   FT_AT(0, "kdrv") },
    { .type = "Library",  .key = "ulib", .icon = "icon_driver",     .fallback = "icon_system",   FT_AT(0, "ulib") },

    // Fonts: TrueType, OpenType (CFF), collections. (Apple's "true" is
    // left to the extension: a text file may start with it too.)
    { .type = "Font",     .key = "ttf",  .icon = "icon_font",       .fallback = "icon_file",     FT_AT(0, "\x00\x01\x00\x00") },
    { .type = "Font",     .key = "otf",  .icon = "icon_font",       .fallback = "icon_file",     FT_AT(0, "OTTO") },
    { .type = "Font",     .key = "ttc",  .icon = "icon_font",       .fallback = "icon_file",     FT_AT(0, "ttcf") },

    // Images: paint opens every format stb_image reads (IMAGE_OPEN)
    { .type = "PNG",      .key = "png",  .icon = "icon_image",      .fallback = "icon_file",     IMAGE_OPEN, FT_AT(0, "\x89" "PNG\r\n\x1a\n") },
    { .type = "JPEG",     .key = "jpg",  .icon = "icon_image",      .fallback = "icon_file",     IMAGE_OPEN, FT_AT(0, "\xff\xd8\xff") },
    { .type = "GIF",      .key = "gif",  .icon = "icon_image",      .fallback = "icon_file",     IMAGE_OPEN, FT_AT(0, "GIF8") },
    { .type = "PSD",      .key = "psd",  .icon = "icon_image",      .fallback = "icon_file",     IMAGE_OPEN, FT_AT(0, "8BPS") },
    { .type = "PIC",      .key = "pic",  .icon = "icon_image",      .fallback = "icon_file",     IMAGE_OPEN, FT_AT(0, "\x53\x80\xf6\x34") },

    // Archives and documents
    { .type = "ZIP",      .key = "zip",  .icon = "icon_archive",    .fallback = "icon_file",     FT_AT(0, "PK\x03\x04") },
    { .type = "gzip",     .key = "gz",   .icon = "icon_archive",    .fallback = "icon_file",     FT_AT(0, "\x1f\x8b") },
    { .type = "tar",      .key = "tar",  .icon = "icon_archive",    .fallback = "icon_file",     FT_AT(257, "ustar") },
    { .type = "PDF",      .key = "pdf",  .icon = "icon_document",   .fallback = "icon_file",     FT_AT(0, "%PDF-") },

    // Audio
    { .type = "WAV",      .key = "wav",  .icon = "icon_audio",      .fallback = "icon_file",     SOUND_OPEN, FT_AT(8, "WAVE") },
    { .type = "MP3",      .key = "mp3",  .icon = "icon_audio",      .fallback = "icon_file",     SOUND_OPEN, FT_AT(0, "ID3") },
    // An MP3 with no ID3 tag starts with a frame (not UTF-8, so never text): MPEG-1 and MPEG-2 layer 3
    { .type = "MP3",      .key = "mp3",  .icon = "icon_audio",      .fallback = "icon_file",     SOUND_OPEN, FT_AT(0, "\xff\xfb") },
    { .type = "MP3",      .key = "mp3",  .icon = "icon_audio",      .fallback = "icon_file",     SOUND_OPEN, FT_AT(0, "\xff\xf3") },

    // By content rather than signature
    { .type = "Empty",    .icon = "icon_file",       .match = FT_EMPTY },
    { .type = "Text",     .key = "txt",  .icon = "icon_document",   .fallback = "icon_file",     .match = FT_TEXT },
    // Weak signature: only binary files (a text file may start "BM" too)
    { .type = "BMP",      .key = "bmp",  .icon = "icon_image",      .fallback = "icon_file",     IMAGE_OPEN, FT_AT(0, "BM") },
    { .type = "File",     .icon = "icon_file",       .match = FT_ANY },
};

/*
 * Types known only by their extension (no signature worth trusting):
 * never matched by content, only consulted for their opener when a
 * registry key of theirs has no "open" value.
 */
static const filetype_assoc_t extension_items[] = {
    { .type = "JPEG",     .key = "jpeg", IMAGE_OPEN },
    { .type = "TGA",      .key = "tga",  IMAGE_OPEN },
    { .type = "PPM",      .key = "ppm",  IMAGE_OPEN },
    { .type = "PGM",      .key = "pgm",  IMAGE_OPEN },
    { .type = "PNM",      .key = "pnm",  IMAGE_OPEN },
    { .type = "MP2",      .key = "mp2",  SOUND_OPEN },
};

const filetype_table_t filetype_default = FILETYPE_TABLE(default_items);

// The header buffer has to reach every signature in the table above
#define DEFAULT_HEADER_NEED     (257 + 5)
_Static_assert(FT_HEADER_MAX >= DEFAULT_HEADER_NEED, "FT_HEADER_MAX too small for the default table");

static bool looks_like_text(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c >= 0x20 || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0x1b)
            continue;
        return false;           // NUL or another control byte: binary
    }
    return true;
}

static bool matches(const filetype_assoc_t* a, const uint8_t* header, size_t length) {
    switch (a->match) {
        case FT_MAGIC:
            if (a->length == 0 || a->length > FT_MAGIC_MAX || (size_t)a->offset + a->length > length)
                return false;
            return memcmp(header + a->offset, a->magic, a->length) == 0;
        case FT_EMPTY:
            return length == 0;
        case FT_TEXT:
            return length > 0 && looks_like_text(header, length);
        case FT_ANY:
            return true;
    }
    return false;
}

const filetype_assoc_t* filetype_identify(const filetype_table_t* table,
                                          const uint8_t* header, size_t length) {
    if (table == NULL)
        table = &filetype_default;
    if (header == NULL)
        length = 0;
    for (size_t i = 0; i < table->count; i++) {
        if (matches(&table->items[i], header, length))
            return &table->items[i];
    }
    return NULL;
}

int filetype_read_header(const char* path, uint8_t* out, size_t size) {
    if (path == NULL || (out == NULL && size > 0)) {
        errno = EFAULT;
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, out + got, size - got);
        if (r < 0) {
            int err = errno;
            close(fd);
            errno = err;
            return -1;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    return (int)got;
}

static const filetype_assoc_t* any_entry(const filetype_table_t* table) {
    for (size_t i = table->count; i-- > 0; )
        if (table->items[i].match == FT_ANY)
            return &table->items[i];
    return NULL;
}

const filetype_assoc_t* filetype_detect(const filetype_table_t* table, const char* path) {
    if (table == NULL)
        table = &filetype_default;

    uint8_t header[FT_HEADER_MAX];
    int n = filetype_read_header(path, header, sizeof(header));
    if (n < 0) {
        int err = errno;
        const filetype_assoc_t* a = any_entry(table);
        errno = err;
        return a;
    }
    return filetype_identify(table, header, (size_t)n);
}

//
// The registry (hkey_root/filetype)
//

_Static_assert(FT_KEY_MAX == OS_REG_NAME_MAX, "a type's key is one registry key name");

static void copy_text(char* out, size_t size, const char* text) {
    if (size == 0)
        return;
    size_t n = (text != NULL) ? strlen(text) : 0;
    if (n >= size)
        n = size - 1;
    if (n > 0)
        memcpy(out, text, n);
    out[n] = '\0';
}

size_t filetype_extension(const char* name, char* out, size_t size) {
    if (out != NULL && size > 0)
        out[0] = '\0';
    if (name == NULL || out == NULL || size == 0)
        return 0;
    const char* slash = strrchr(name, '/');
    if (slash != NULL)
        name = slash + 1;
    const char* dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0')
        return 0;                       // "hello", ".profile", "name."
    const char* ext = dot + 1;
    size_t n = strlen(ext);
    if (n >= size || n >= FT_KEY_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = ext[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[n] = '\0';
    return n;
}

// The built-in association for the type registered as `key`, or NULL
static const filetype_assoc_t* default_for_key(const char* key) {
    for (size_t i = 0; i < filetype_default.count; i++) {
        const filetype_assoc_t* a = &filetype_default.items[i];
        if (a->key != NULL && strcmp(a->key, key) == 0)
            return a;
    }
    for (size_t i = 0; i < sizeof(extension_items) / sizeof(extension_items[0]); i++) {
        if (strcmp(extension_items[i].key, key) == 0)
            return &extension_items[i];
    }
    return NULL;
}

// Fill *out from FILETYPE_REG_KEY/<key> when that key names an icon.
// `type` is the name to use when the key gives none.
static bool from_registry(const char* key, const char* type, filetype_info_t* out) {
    if (key == NULL || key[0] == '\0' || strchr(key, '/') != NULL)
        return false;
    char path[sizeof(FILETYPE_REG_KEY) + FT_KEY_MAX + 1];
    size_t base = sizeof(FILETYPE_REG_KEY) - 1;
    size_t klen = strlen(key);
    if (klen >= FT_KEY_MAX)
        return false;
    memcpy(path, FILETYPE_REG_KEY, base);
    path[base] = '/';
    memcpy(path + base + 1, key, klen + 1);

    int saved = errno;
    char icon[FT_NAME_MAX];
    if (os_reg_get_string(path, FILETYPE_REG_ICON, icon, sizeof(icon)) < 0 || icon[0] == '\0') {
        errno = saved;
        return false;
    }
    copy_text(out->icon, sizeof(out->icon), icon);
    if (os_reg_get_string(path, FILETYPE_REG_TYPE, out->type, sizeof(out->type)) < 0 || out->type[0] == '\0')
        copy_text(out->type, sizeof(out->type), type);
    if (os_reg_get_string(path, FILETYPE_REG_FALLBACK, out->fallback, sizeof(out->fallback)) < 0)
        out->fallback[0] = '\0';
    // No "open" at all: the built-in one (and whether it is a viewer);
    // "" says there is none. An opener from the registry is a viewer only
    // when the key says so.
    if (os_reg_get_string(path, FILETYPE_REG_OPEN, out->open, sizeof(out->open)) < 0) {
        const filetype_assoc_t* a = default_for_key(key);
        copy_text(out->open, sizeof(out->open), a != NULL ? a->open : NULL);
        out->viewer = (a != NULL && a->open != NULL && a->viewer);
    } else {
        char viewer[4];
        out->viewer = (os_reg_get_string(path, FILETYPE_REG_VIEWER, viewer, sizeof(viewer)) >= 0 &&
                       strcmp(viewer, "1") == 0);
    }
    copy_text(out->key, sizeof(out->key), key);
    errno = saved;
    return true;
}

// "ttf" -> "TTF", for a registered extension that names no type
static void upper_name(char* out, size_t size, const char* ext) {
    copy_text(out, size, ext);
    for (char* p = out; *p != '\0'; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');
}

static void from_assoc(const filetype_assoc_t* a, filetype_info_t* out) {
    copy_text(out->type, sizeof(out->type), a != NULL ? a->type : "File");
    copy_text(out->icon, sizeof(out->icon), a != NULL ? a->icon : "icon_file");
    copy_text(out->fallback, sizeof(out->fallback), a != NULL ? a->fallback : NULL);
    copy_text(out->open, sizeof(out->open), a != NULL ? a->open : NULL);
    out->viewer = (a != NULL && a->open != NULL && a->viewer);
    out->key[0] = '\0';
}

void filetype_lookup_header(const char* name, const uint8_t* header, size_t length,
                            filetype_info_t* out) {
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));

    char ext[FT_KEY_MAX], upper[FT_KEY_MAX];
    if (filetype_extension(name, ext, sizeof(ext)) > 0) {
        upper_name(upper, sizeof(upper), ext);
        if (from_registry(ext, upper, out))
            return;
    }

    const filetype_assoc_t* a = (header != NULL) ? filetype_identify(NULL, header, length)
                                                 : any_entry(&filetype_default);
    if (a != NULL && from_registry(a->key, a->type, out))
        return;
    from_assoc(a, out);
}

int filetype_lookup(const char* path, filetype_info_t* out) {
    if (path == NULL || out == NULL) {
        if (out != NULL)
            from_assoc(any_entry(&filetype_default), out);
        errno = EFAULT;
        return -1;
    }
    uint8_t header[FT_HEADER_MAX];
    int saved = errno;
    int n = filetype_read_header(path, header, sizeof(header));
    errno = saved;
    filetype_lookup_header(path, n >= 0 ? header : NULL, n >= 0 ? (size_t)n : 0, out);
    return 0;
}

//
// Opening a file with its type's program
//

pid_t filetype_open(const filetype_info_t* info, const char* path, const char* cwd,
                    sec_mask_t security) {
    if (info == NULL || path == NULL) {
        errno = EFAULT;
        return -1;
    }
    // An opener that runs the file: only one marked executable. A viewer
    // (paint for an image) only reads it, so any file will do.
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }
    if (!info->viewer && !(st.st_mode & S_IXUSR)) {
        errno = EACCES;
        return -1;
    }
    // "console --script" -> words in place
    char words[FT_OPEN_MAX];
    copy_text(words, sizeof(words), info->open);
    char* args[SPAWN_ARG_MAX + 1];
    size_t argc = 0;
    for (char* p = words; *p != '\0';) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (*p == '\0')
            break;
        if (argc == SPAWN_ARG_MAX - 1) {
            errno = E2BIG;
            return -1;
        }
        args[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
    }
    if (argc == 0) {
        errno = EINVAL;
        return -1;
    }

    char program[SYS_PATH_MAX];
    if (os_programpath(args[0], program, sizeof(program)) == NULL) {
        errno = ENOENT;
        return -1;
    }
    args[0] = program;
    args[argc++] = (char*)path;
    args[argc] = NULL;
    return spawn_program(program, args, cwd, security);
}
