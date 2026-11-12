#ifndef DWM_ICONS_H
#define DWM_ICONS_H

/*
 * Desktop icons: an image from the image library with a label, drawn on
 * the desktop below every window.
 *
 *     dwm_icon_t docs = dwm_icon_add("Documents", "icon_folder", DWM_POS_DEFAULT, 0);
 *
 * With the mouse (handled by the DWM):
 *
 *   click             select that icon only
 *   Ctrl + click      add it to the selection, or take it out
 *   drag an icon      move every selected icon together; let go over a
 *                     window or a drop target icon and they go back, the
 *                     desktop procedure getting DWM_EVENT_ICONS_DROPPED
 *                     (drag.h) to move the files there
 *   drag the desktop  selection rectangle: selects what it touches
 *                     (with Ctrl: added to the current selection)
 *   click the desktop deselect everything (not with Ctrl)
 *   double-click      DWM_EVENT_ICON_OPEN to the desktop procedure
 *
 * And with the keys, while the desktop has the focus and its procedure
 * leaves them: Enter opens the selected icons, Ctrl+A selects them all.
 *
 * A label too long for the cell ends in ".."; while the icon is selected
 * all of it shows, wrapped, on a plate below the cell.
 *
 * Renaming: dwm_icon_edit(icon) puts an edit box over the label, the name
 * selected so typing replaces it (Backspace, an arrow or a click into the
 * box keeps it to edit). Every key goes to the box. Enter, or a click
 * anywhere else, keeps the text: the desktop procedure gets
 * DWM_EVENT_ICON_RENAMED and reads it with dwm_icon_edit_result(); the
 * label is left as it was, for the procedure to change once the rename
 * really happened. Escape (or removing the icon) drops it.
 *
 * The desktop procedure hears about it through DWM_EVENT_ICON_OPEN,
 * DWM_EVENT_SELECTION_CHANGED, DWM_EVENT_ICONS_MOVED and
 * DWM_EVENT_ICON_RENAMED (event.h).
 *
 * Handles work like window handles: a removed icon's handle stays dead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "window.h"         // DWM_POS_DEFAULT

typedef uint32_t dwm_icon_t;

#define DWM_MAX_ICONS       128
#define DWM_ICON_LABEL_MAX  256     // Bytes, terminator included (a whole file name)
#define DWM_ICON_WIDTH      72      // Cell width; the label is cut to fit
#define DWM_ICON_GRID_X     84      // Spacing of DWM_POS_DEFAULT positions
#define DWM_ICON_GRID_Y     76

// Add an icon. `image` names an image-library image (a plain square is
// drawn when there is none by that name). x, y is the cell's top-left;
// DWM_POS_DEFAULT (for either) takes the next free spot of the grid,
// filling columns from the top-left. Returns 0 when DWM_MAX_ICONS exist.
dwm_icon_t dwm_icon_add(const char* label, const char* image, int32_t x, int32_t y);
void       dwm_icon_remove(dwm_icon_t icon);
bool       dwm_icon_exists(dwm_icon_t icon);

void        dwm_icon_set_label(dwm_icon_t icon, const char* label);
const char* dwm_icon_label(dwm_icon_t icon);        // "" if no such icon
void        dwm_icon_set_image(dwm_icon_t icon, const char* image);
const char* dwm_icon_image(dwm_icon_t icon);        // Its image's name, "" if no such icon

void dwm_icon_set_user(dwm_icon_t icon, void* user);
void* dwm_icon_user(dwm_icon_t icon);

// The file, folder or drive the icon stands for (NULL or "": none), so the
// drag plate can say "Copy" for icons dragged to another drive, or files
// dragged onto this one from another drive (drag.h)
#define DWM_ICON_PATH_MAX   400     // Bytes, terminator included
void        dwm_icon_set_path(dwm_icon_t icon, const char* path);
const char* dwm_icon_path(dwm_icon_t icon);        // "" if none

// A drop target (a folder or a drive): dragged icons or files let go on it
// are for it (DWM_EVENT_ICONS_DROPPED, DWM_EVENT_DROP's param[0])
void dwm_icon_set_drop_target(dwm_icon_t icon, bool target);
bool dwm_icon_drop_target(dwm_icon_t icon);

void dwm_icon_move(dwm_icon_t icon, int32_t x, int32_t y);
bool dwm_icon_get_rect(dwm_icon_t icon, gfx_rect_t* rect);     // Cell, screen coordinates

// Selection
bool   dwm_icon_selected(dwm_icon_t icon);
void   dwm_icon_select(dwm_icon_t icon, bool selected);
void   dwm_icon_select_all(bool selected);
size_t dwm_icon_selection(dwm_icon_t* out, size_t max);     // How many are selected; fills out

// Every icon, in drawing order (the last is on top)
size_t     dwm_icon_count(void);
dwm_icon_t dwm_icon_at_index(size_t index);

// The topmost icon whose cell contains (x, y), or 0
dwm_icon_t dwm_icon_at(int32_t x, int32_t y);

// Renaming (see above). dwm_icon_edit selects the icon alone and gives the
// desktop the focus; false if there is no such icon. dwm_icon_edit_end
// finishes the open edit (keep: as Enter does). The result stays readable
// until the next rename is kept.
bool        dwm_icon_edit(dwm_icon_t icon);
dwm_icon_t  dwm_icon_editing(void);             // 0: none
void        dwm_icon_edit_end(bool keep);
const char* dwm_icon_edit_result(void);

#endif
