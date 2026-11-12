#ifndef _OS_FILEDLG_H
#define _OS_FILEDLG_H

/*
 * The standard "Open" / "Save as" dialog: a window of the program's own
 * (os/wm.h) with the current folder, its folders and files, a name field
 * and the Open / Save and Cancel buttons.
 *
 * It does not block. It lives in the program's event loop next to the
 * program's own windows, the same way as an edit box (os/ui.h):
 *
 *     filedlg_desc_t d = { .mode = FILEDLG_OPEN, .path = doc_path, .owner = main_win };
 *     filedlg_t* dlg = filedlg_open(&d);              // NULL: errno says why
 *
 *     // The event loop: paint it when the queue runs dry, like the other windows
 *     int r = wm_wait_event(&ev, (main_dirty || filedlg_dirty(dlg)) ? 0 : -1);
 *     if (r == 0) { ...; filedlg_paint(dlg); continue; }
 *
 *     switch (filedlg_event(dlg, &ev)) {              // NULL dlg: FILEDLG_IGNORED
 *         case FILEDLG_ACCEPT:
 *             if (open_document(filedlg_path(dlg))) { filedlg_close(dlg); dlg = NULL; }
 *             break;                                  // Else: it stays up, try again
 *         case FILEDLG_CANCEL:  filedlg_close(dlg); dlg = NULL; break;
 *         case FILEDLG_HANDLED: break;
 *         case FILEDLG_IGNORED: ... the program's own event ...
 *     }
 *
 * While it is up the program keeps its other windows from taking input:
 * on a key or click there, wm_window_focus(filedlg_window(dlg)).
 *
 * Using it: a click selects (a file's name goes into the name field), a
 * double click opens a folder or picks a file. Up / Down, Page Up / Down
 * move through the list (Home / End too after using it), Enter opens the
 * selected folder or accepts the name, Backspace in an empty name field
 * (or right after using the list) goes up a folder, Tab moves between the
 * list and the name field, Escape cancels. The name may also be a path,
 * relative to the folder shown or absolute: a folder is opened, a file is
 * picked. "Up" (and "..") goes to the parent folder.
 *
 * FILEDLG_ACCEPT means: the user picked filedlg_path(), absolute. For Open
 * it exists and is a file. For Save it is not a folder, and if a file is
 * already there the user has been asked to Save a second time to replace
 * it. The dialog stays up until filedlg_close(), so a program that can't
 * use the path tells the user why and leaves it up for another try.
 *
 * Events for window 0 (WM_EVENT_THEME_CHANGED, WM_EVENT_FS_CHANGED) are
 * used (new colors, the folder read again) and still returned as
 * FILEDLG_IGNORED, so the program sees them too.
 *
 * Needs what os/wm.h needs, and SEC_FILE_READ to list folders. Not
 * thread-safe.
 */

#include <stdbool.h>
#include <stdint.h>
#include <os/wm.h>

typedef enum {
    FILEDLG_OPEN = 0,               // Pick an existing file
    FILEDLG_SAVE,                   // Pick a name to save under
} filedlg_mode_t;

typedef enum {
    FILEDLG_IGNORED = 0,            // Not the dialog's event
    FILEDLG_HANDLED,                // Used; nothing for the program to do
    FILEDLG_ACCEPT,                 // A path was picked: filedlg_path()
    FILEDLG_CANCEL,                 // Cancel, Escape or the close box: filedlg_close()
} filedlg_result_t;

typedef struct {
    filedlg_mode_t mode;
    const char*    title;           // Window title (NULL: "Open" / "Save as")
    const char*    path;            // Where to start: a folder, or a file (its folder,
                                    // and its name in the field). NULL / "": the cwd
    const char*    name;            // Save: the name offered when `path` is a folder
                                    // (NULL: none)
    wm_window_t    owner;           // Focused again by filedlg_close() (0: none)
} filedlg_desc_t;

typedef struct filedlg filedlg_t;

// Make the window and show it, focused. NULL with errno (as
// wm_window_create, ENOMEM).
filedlg_t* filedlg_open(const filedlg_desc_t* desc);

// Destroy the window and free everything; focuses `owner`. NULL is fine.
void filedlg_close(filedlg_t* dlg);

filedlg_result_t filedlg_event(filedlg_t* dlg, const wm_event_t* ev);

// Something changed since the last paint
bool filedlg_dirty(const filedlg_t* dlg);

// Draw and commit the window if it is dirty
void filedlg_paint(filedlg_t* dlg);

// The path picked (after FILEDLG_ACCEPT): absolute
const char* filedlg_path(const filedlg_t* dlg);

filedlg_mode_t filedlg_mode(const filedlg_t* dlg);
wm_window_t    filedlg_window(const filedlg_t* dlg);

#endif
