#ifndef DESKTOP_LAYOUT_H
#define DESKTOP_LAYOUT_H

/*
 * Desktop icon layout: where each icon was left, kept in <home>/usr/icons.
 *
 * The file is plain text, one icon per line, keyed by the path the icon
 * stands for:
 *
 *     # Desktop icon positions: x y path
 *     12 12 /mnt/ata0p1
 *     96 12 /mnt/ata0p1/usr/desktop/notes.txt
 *
 * Lines that don't parse are skipped, so a hand-edited or damaged file
 * costs only those icons their place. Icons that aren't on the desktop
 * right now but may come back (a drive that is unplugged, say) keep their
 * line, so they return where they were, up to LAYOUT_MAX entries. Lines for
 * paths that are gone from a device that is present (deleted or moved away)
 * are dropped at the next capture.
 *
 * The file is written to "icons.tmp" first and renamed over "icons", so a
 * save that fails half way leaves the old layout intact.
 */

#include <stdbool.h>
#include <stdint.h>

#define LAYOUT_SUB      "usr"           // Under the home device
#define LAYOUT_FILE     "icons"
#define LAYOUT_MAX      256             // Entries kept, icons showing first

// Read the saved layout (replacing what was loaded before). No file yet is
// not an error. 0, or -1 with errno.
int  layout_load(void);

// The saved position for `path`. False when there is none.
bool layout_find(const char* path, int32_t* x, int32_t* y);

// Take every icon's current position (their userdata is the path) into the
// layout. Call before the icons are removed (refresh) or saved.
void layout_capture(void);

// layout_capture(), then write the file. 0, or -1 with errno.
int  layout_save(void);

// `old_path` (and everything below it) is now called `new_path`: saved
// places move along
void layout_rename(const char* old_path, const char* new_path);

// Free everything
void layout_free(void);

#endif
