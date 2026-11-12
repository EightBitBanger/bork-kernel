#ifndef DWM_DRAG_H
#define DWM_DRAG_H

/*
 * Drag and drop of files: between the desktop, its icons and program
 * windows (an explorer window to the desktop, desktop icons into a folder
 * window, one explorer window to another, ...).
 *
 * The DWM only carries the paths; whoever gets the drop moves (or copies)
 * the files itself. A drop holds one list of absolute paths, '\n' between
 * them (as DWM_CLIP_FILES), and lasts until the next drag starts.
 *
 * Starting one:
 *
 *   - desktop icons: dragging selected icons over a window, or onto an
 *     icon marked as a drop target (dwm_icon_set_drop_target: a folder or a
 *     drive), and letting go there puts the icons back where they were and
 *     sends the desktop procedure DWM_EVENT_ICONS_DROPPED (event.h). The
 *     desktop moves them into the folder itself, or hands the paths to the
 *     window with dwm_drop_files(). Anywhere else the icons just move, as
 *     before.
 *   - programs: while the left (or right) button is down in one of its
 *     windows, a program calls wm_drag_files() (os/wm.h) with the paths.
 *     The window gets its MOUSE_UP at once; the DWM has the pointer from
 *     then on and the drop happens where that button comes up. Escape or a
 *     menu opening cancels it.
 *
 * Right-button drags: icons and files drag with the right button too, and
 * no context menu follows. The drop (or ICONS_DROPPED) says ev->button ==
 * INPUT_BTN_RIGHT: whoever gets it opens a "Move here / Copy here / Cancel"
 * menu where it was let go instead of moving at once.
 *
 * While a drag is on, a small plate by the pointer says what will happen
 * ("Move 3 items", "Copy to Documents"). Holding Ctrl when letting go asks
 * for a copy instead of a move; ev->mods carries it. Files from another
 * drive than where they would land are copied, so the plate says "Copy"
 * for them: it knows where that is from dwm_set_drop_folder() (a window,
 * or DWM_DESKTOP) and dwm_icon_set_path() (a drop target icon). Where it
 * isn't known, it says "Move".
 *
 * The drop, DWM_EVENT_DROP:
 *
 *   - over a window (any part of it): to that window, x / y in client
 *     coordinates (negative over the titlebar). Program windows get it as
 *     WM_EVENT_DROP and fetch the paths with wm_drop_get().
 *   - over the desktop: to the desktop procedure, x / y on the screen,
 *     param[0] = the drop target icon under the pointer (0: none).
 *
 * param[1] is the drop's serial in both: read the paths with
 * dwm_drop_data(serial) (NULL once a newer drag replaced it).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clipboard.h"      // dwm_clip_info_t
#include "event.h"

// Hand `count` paths to `window` as a drop at (x, y), screen coordinates:
// it gets DWM_EVENT_DROP (client coordinates, `mods` and `button` as given)
// this round. For the desktop
// procedure, after DWM_EVENT_ICONS_DROPPED. False with errno (EINVAL bad
// paths, EBADF no such window, ENOMEM).
bool dwm_drop_files(dwm_handle_t window, int32_t x, int32_t y, uint8_t mods, uint8_t button,
                    const char* const* paths, size_t count);

// The paths of drop `serial` (size bytes, 0-terminated), or NULL when that
// drop is gone. `info` (may be NULL) gets its description.
const char* dwm_drop_data(uint32_t serial, dwm_clip_info_t* info);

// A drag is in progress (desktop icons or a program's)
bool dwm_dragging(void);

// The folder files let go over `window` go into (DWM_DESKTOP: on the
// desktop, off its icons), for the plate's Move / Copy. NULL or "": not
// known. Gone with the window. The drop itself still decides what happens.
// Set during a drag (answering DWM_EVENT_DRAG_OVER), the plate then says
// "Move to <its name>" / "Copy to <its name>".
#define DWM_DROP_FOLDER_MAX 400         // Bytes, terminator included (WM_PATH_MAX)
void        dwm_set_drop_folder(dwm_handle_t window, const char* folder);
const char* dwm_drop_folder(dwm_handle_t window);       // "": not known

// `a` and `b` are on the same drive ("/mnt/<drive>/..."), or both outside
// every drive
bool dwm_same_drive(const char* a, const char* b);

#endif
