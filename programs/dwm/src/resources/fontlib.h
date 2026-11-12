#ifndef FONTLIB_H
#define FONTLIB_H

/*
 * Font library: the font files in a folder, opened once (libc's os/font.h,
 * which checks each file before stb_truetype reads it) and kept by name.
 *
 *     fontlib_load_dir(fonts_dir);            // every font file in the folder
 *     font_t* f = fontlib_get("LiberationSans-Regular");
 *
 * Font files are found by their header, not their name: a file whose
 * first four bytes are a TrueType (00 01 00 00 or "true"), OpenType ("OTTO")
 * or collection ("ttcf") signature is tried, whatever it is called; other
 * files are left alone. Its reference name is its file name without the
 * extension, if it has one: "<dir>/dejavu_sans.ttf" and "<dir>/dejavu_sans"
 * are both "dejavu_sans". For a collection the first font is used.
 * Fonts stay open until fontlib_free(). Not thread-safe.
 */

#include <stddef.h>
#include <os/font.h>

#define FONT_NAME_MAX   64      // Bytes, terminator included

// Load every font file in `dir` (see above). Files that fail are skipped
// (see fontlib_skipped). Returns how many were added, or -1 with errno when
// the folder cannot be opened.
int fontlib_load_dir(const char* dir);

// How many files the last fontlib_load_dir() skipped; *first (may be NULL)
// gets the first one's name ("" when none)
size_t fontlib_skipped(const char** first);

// Load one font file under `name`. 0, or -1 with errno: as font_open_file,
// or EEXIST (name in use), ENAMETOOLONG (name), ENOMEM.
int fontlib_load_file(const char* path, const char* name);

// The font loaded under `name`, or NULL
font_t*     fontlib_get(const char* name);

// Every font, sorted by name: index 0 .. fontlib_count() - 1
size_t      fontlib_count(void);
font_t*     fontlib_at(size_t index);
const char* fontlib_name(size_t index);

// The reference name of a loaded font (NULL: not one of them)
const char* fontlib_name_of(const font_t* font);

void fontlib_free(void);

#endif
