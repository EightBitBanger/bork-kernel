#ifndef DWM_CLIPBOARD_H
#define DWM_CLIPBOARD_H

/*
 * The system clipboard. One item at a time, owned by the DWM, shared with
 * every program (os/wm.h: wm_clip_*) and with the DWM's own code (here).
 *
 * Every item carries a type, so a program pasting knows what it is getting
 * and can refuse what it can't use:
 *
 *   DWM_CLIP_TEXT     bytes of text, no terminator needed
 *   DWM_CLIP_IMAGE    width x height 0xAARRGGBB pixels, row after row
 *   DWM_CLIP_BINARY   raw bytes; `format` names them ("audio/wav", "app.cells")
 *   DWM_CLIP_FILES    absolute paths, one per line ('\n' between them).
 *                     DWM_CLIP_CUT: a move; whoever pastes clears the item.
 *   DWM_CLIP_USER     and up: any type programs agree on (use `format` too)
 *
 * The DWM keeps its own copy of the data: changing or freeing what was set
 * afterwards does not change the clipboard, and the item stays when the
 * program that set it exits. At most DWM_CLIP_MAX bytes.
 *
 * Each change (set or clear) gets a new serial number. Programs hear about
 * it as WM_EVENT_CLIP_CHANGED; a paste that should only clear "the item I
 * pasted" (a cut) passes that item's serial to dwm_clip_clear.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DWM_CLIP_NONE           0
#define DWM_CLIP_TEXT           1
#define DWM_CLIP_IMAGE          2
#define DWM_CLIP_BINARY         3
#define DWM_CLIP_FILES          4
#define DWM_CLIP_USER           0x100

#define DWM_CLIP_CUT            0x0001      // FILES: move them on paste

#define DWM_CLIP_FORMAT_MAX     32          // Bytes, terminator included
#define DWM_CLIP_MAX            (4u << 20)  // Bytes of data
#define DWM_CLIP_IMAGE_MAX_DIM  4096

typedef struct {
    uint32_t type;                          // DWM_CLIP_*
    uint32_t flags;                         // DWM_CLIP_CUT
    uint32_t size;                          // Bytes of data
    uint32_t width, height;                 // IMAGE: pixels; 0 otherwise
    uint32_t serial;                        // Set by the DWM (ignored when setting)
    uint32_t owner;                         // Who set it: a pid, 0 for the DWM (ignored when setting)
    char     format[DWM_CLIP_FORMAT_MAX];   // Optional name for the data
} dwm_clip_info_t;

// Replace the clipboard with `size` bytes of `data` (copied), described by
// `info` (type, flags, size, width, height, format). False with errno:
// EINVAL (no type, size over DWM_CLIP_MAX, image size not width*height*4),
// ENOMEM.
bool dwm_clip_set(const dwm_clip_info_t* info, const void* data);

// Shortcuts: text (its strlen), a list of absolute paths
bool dwm_clip_set_text(const char* text);
bool dwm_clip_set_files(const char* const* paths, size_t count, bool cut);

// What is on the clipboard. False (out->type DWM_CLIP_NONE) when empty.
bool dwm_clip_info(dwm_clip_info_t* out);

// The data (size bytes, plus a terminating 0 for convenience); valid until
// the clipboard next changes. NULL when empty.
const void* dwm_clip_data(void);

// Empty the clipboard, but only if it still holds the item with this
// serial (0: whatever it holds)
void dwm_clip_clear(uint32_t serial);

// The current serial (changes with every set and clear)
uint32_t dwm_clip_serial(void);

#endif
