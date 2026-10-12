#ifndef _FILEDELETE_WINDOW_H_
#define _FILEDELETE_WINDOW_H_

#include <kernel/dwm/dwm.h>

// File / folder delete window.
//
// Give it a path and it does the rest: the window opens right away, a
// background thread (owned by this module, started on first use) deletes the
// file or the whole folder tree, and the window closes itself when it is
// done. Several windows can be open; their jobs run one at a time, in order,
// and a waiting window says so.
//
// Phases:
//   Preparing   The tree is walked and counted (no bar). Nothing is deleted
//               yet: a tree that holds a mounted device, is nested too deeply
//               or has paths that are too long is refused here, untouched.
//   Deleting    Items are removed deepest first; the bar counts items.
//
// Cancel stops before the next item. Deletion cannot be undone, so whatever
// was already removed stays removed, and the user is told how much that was.
// Items that could not be removed are reported in a message box.
//
// When the job ends, desktop icons for anything that is gone are removed and
// every open window is refreshed (explorers re-read their folder).
//
// Threading: same model as the copy window (filecopy.h). The delete thread
// never takes the DWM lock; it posts to the DWM thread. These functions take
// the DWM lock themselves; calling them from a window procedure is fine.
//
// Lock order: DWM lock -> filedelete lock -> VFS lock.

// Longest path
#define FILEDELETE_PATH_MAX     256

// Open a delete window for `path` (a file or a folder, folders are deleted
// with everything in them). Storage devices are refused.
// Returns 0 if the window or the job could not be created.
WindowHandle dwm_summon_filedelete(const char* path);

// True while any delete window has work queued or running
// (e.g. hold off shutting down)
bool dwm_filedelete_busy(void);

#endif
