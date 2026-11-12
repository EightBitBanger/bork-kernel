#ifndef _OS_FILETYPE_H
#define _OS_FILETYPE_H

/*
 * File types from file contents: the first bytes of a file (its header,
 * "magic number") say what it is, whatever it is called.
 *
 *     const filetype_assoc_t* t = filetype_detect(NULL, "/mnt/ata0p1/bin/hello");
 *     t->type      "ELF"
 *     t->icon      "icon_executable"       (an image in <home>/sys/images)
 *
 * A table (filetype_table_t) is a list of associations, tried in order:
 * the first that matches wins, so specific signatures go before general
 * ones. Each association matches one of:
 *
 *   FT_MAGIC   `length` bytes equal to `magic` at `offset` in the header
 *   FT_EMPTY   a file with no bytes at all
 *   FT_TEXT    a header with no NUL and no control characters other than
 *              tab, newline, carriage return, form feed and escape
 *              (bytes 0x80 and up pass, so UTF-8 text counts)
 *   FT_ANY     anything: the table's fallback, put it last
 *
 *     static const filetype_assoc_t mine[] = {
 *         { .type = "Cells", .icon = "icon_sheet", FT_AT(0, "CELL\x01") },
 *         { .type = "File",  .icon = "icon_file",  .match = FT_ANY },
 *     };
 *     static const filetype_table_t my_types = FILETYPE_TABLE(mine);
 *     filetype_detect(&my_types, path);
 *
 * filetype_default is the system's table. It ends with FT_ANY ("File",
 * "icon_file"), so detecting against it always has an answer.
 *
 * `fallback` (may be NULL) names a second icon to use when `icon` is not
 * installed, so a type newer than the system images never shows up blank.
 *
 * Detection reads at most FT_HEADER_MAX bytes of the file and needs
 * SEC_FILE_READ. Folders are not detected: callers know those already.
 *
 * The registry: hkey_root/filetype
 * --------------------------------
 * The system's file types can be set in the registry, one key per type
 * under FILETYPE_REG_KEY, named by the type's extension in lower case:
 *
 *     hkey_root/filetype/ttf     type "Font"      icon "icon_font"
 *     hkey_root/filetype/drv     type "Driver"    icon "icon_driver"   fallback "icon_system"
 *     hkey_root/filetype/ulib    type "Library"   icon "icon_driver"   fallback "icon_system"
 *     hkey_root/filetype/elf     type "Program"   icon "icon_executable"
 *
 * Each value is a string; only "icon" is needed. filetype_lookup() asks
 * the registry about a file in this order:
 *
 *   1. its extension ("opensans.ttf" -> filetype/ttf), when that key has an
 *      icon. A name with no dot, or only a leading one, has none.
 *   2. what its header says it is (filetype_detect), through that type's
 *      `key` ("\x7fELF" -> filetype/elf, "kdrv" -> filetype/drv, "ulib" -> filetype/ulib), so a
 *      program called "hello" still finds filetype/elf.
 *   3. the built-in association from step 2 as it is.
 *
 * Opening a file
 * --------------
 * A type's "open" value (optional) names the program that runs its files,
 * with any arguments that go before the file. The explorer and the desktop
 * use it first for a file marked executable, then run a program as itself.
 * A file without the execute attribute is never run: it opens in notepad,
 * whatever its type, unless the type's opener is a viewer (below):
 *
 *     hkey_root/filetype/sh      open "console --script"
 *
 * so a script "setup.sh" opens as `<home>/bin/console --script setup.sh`
 * (filetype_open()). The program is looked for as os_programpath() does
 * (<home>/sys, then <home>/bin); the words are split at spaces, with no
 * quoting. A key with no "open" value gets the built-in table's (the `open`
 * field below), so a hive written before there were openers still opens
 * scripts; an empty "open" value means the type has no opener.
 *
 * An opener that only reads the file (an image in paint) says so with
 * "viewer" "1". It then opens every file of the type, marked executable or
 * not, since nothing in the file is run:
 *
 *     hkey_root/filetype/png     open "paint"   viewer "1"
 *
 * Without "viewer" an opener from the registry counts as one that runs the
 * file. A key with no "open" at all gets the built-in opener with the
 * built-in `viewer` as well.
 *
 * "boot install" and "boot repair" write the defaults (kernel/console/
 * commands/boot_registry.h); they match the built-in table, so a system
 * without them looks the same. Add a key to give a new extension an icon,
 * or change one to change what a type shows.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <os/security.h>

#define FILETYPE_REG_KEY    "hkey_root/filetype"
#define FILETYPE_REG_TYPE   "type"          // Value: what the type is called ("Font")
#define FILETYPE_REG_ICON   "icon"          // Value: image name ("icon_font")
#define FILETYPE_REG_FALLBACK "fallback"    // Value (optional): image when `icon` isn't installed
#define FILETYPE_REG_OPEN   "open"          // Value (optional): what opens it ("console --script")
#define FILETYPE_REG_VIEWER "viewer"        // Value (optional): "1" when that only reads the file ("paint")

#define FT_HEADER_MAX   512         // Bytes read from the start of a file (one sector)
#define FT_MAGIC_MAX    16          // Bytes one signature can hold

// filetype_assoc_t.match
#define FT_MAGIC        1
#define FT_EMPTY        2
#define FT_TEXT         3
#define FT_ANY          4

typedef struct {
    const char* type;               // Short name: "ELF", "PNG", "Text"
    const char* icon;               // Image name: "icon_executable"
    const char* fallback;           // When `icon` isn't installed (NULL: none)
    const char* key;                // Its key under FILETYPE_REG_KEY ("elf"), NULL: none
    const char* open;               // What opens it ("console --script"), NULL: nothing special
    bool        viewer;             // `open` only reads the file: no execute attribute needed
    uint8_t     match;              // FT_*
    uint8_t     length;             // FT_MAGIC: bytes of `magic` compared
    uint16_t    offset;             // FT_MAGIC: where in the header
    uint8_t     magic[FT_MAGIC_MAX];
} filetype_assoc_t;

// A signature from a string literal (NUL bytes allowed) at byte `off`.
// Mind hex escapes running on: "\x7f" "ELF", not "\x7fELF".
#define FT_AT(off, lit) \
    .match = FT_MAGIC, .offset = (off), .length = (uint8_t)(sizeof(lit) - 1), .magic = lit

// A list of associations
typedef struct {
    const filetype_assoc_t* items;
    size_t                  count;
} filetype_table_t;

#define FILETYPE_TABLE(array)   { (array), sizeof(array) / sizeof((array)[0]) }

extern const filetype_table_t filetype_default;

// The first association in `table` (NULL: filetype_default) that matches
// `length` bytes of header. NULL when none does (never for the default).
const filetype_assoc_t* filetype_identify(const filetype_table_t* table,
                                          const uint8_t* header, size_t length);

// Read `path`'s header and identify it. A file that can't be read gets the
// table's FT_ANY entry (errno says why); NULL only when there is none.
const filetype_assoc_t* filetype_detect(const filetype_table_t* table, const char* path);

// What filetype_lookup() found. Strings are always terminated; fallback
// is "" when there is none.
#define FT_NAME_MAX     32
#define FT_KEY_MAX      16          // OS_REG_NAME_MAX
#define FT_OPEN_MAX     64
typedef struct {
    char type[FT_NAME_MAX];         // "Font", "Program", "Text"
    char icon[FT_NAME_MAX];         // "icon_font"
    char fallback[FT_NAME_MAX];     // "icon_file", or ""
    char key[FT_KEY_MAX];           // The registry key used ("ttf"), "" when none was
    char open[FT_OPEN_MAX];         // The program that opens it and its first arguments, or ""
    bool viewer;                    // `open` only reads the file (an image viewer or editor)
} filetype_info_t;

// The file type of `path`: from the registry by extension, else by
// content (see "The registry" at the top). Always fills *out: a file that
// can't be read and has no registered extension is "File", "icon_file".
// Returns 0, or -1 when path or out is NULL (EFAULT).
int filetype_lookup(const char* path, filetype_info_t* out);

// The same from a name and a header already read (header NULL: content
// unknown, only the extension is looked at, then "File")
void filetype_lookup_header(const char* name, const uint8_t* header, size_t length,
                            filetype_info_t* out);

// Start the program `info->open` names on `path` (see "Opening a file"),
// in `cwd`, with `security` as for spawn_program() (os/process.h). Its
// pid, or -1 with errno: EACCES when `path` isn't marked executable and
// the opener isn't a viewer (it is never run then), EINVAL when the type has no opener, ENOENT when that
// program isn't installed, else spawn_program()'s.
pid_t filetype_open(const filetype_info_t* info, const char* path, const char* cwd,
                    sec_mask_t security);

// `name`'s extension in lower case ("Font.TTF" -> "ttf") into out. Returns
// its length, 0 when it has none or it does not fit a registry key name.
size_t filetype_extension(const char* name, char* out, size_t size);

// Up to `size` bytes from the start of `path` into out. How many were read
// (0: empty file), or -1 with errno.
int filetype_read_header(const char* path, uint8_t* out, size_t size);

#endif
