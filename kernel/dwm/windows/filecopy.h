#ifndef _FILECOPY_WINDOW_H_
#define _FILECOPY_WINDOW_H_

#include <kernel/dwm/dwm.h>
#include <kernel/mutex.h>

// File copy / move window.
//
// Give it a source and a destination path and it does the rest: the window
// opens right away, a background thread (owned by this module, started on
// first use) copies the file or folder tree, and the window closes itself
// when the job is done. Several windows can be open; their jobs run one at a
// time, in order, and a waiting window says so.
//
// Responsiveness: the copy thread never takes the DWM lock and never touches
// DWM state. It holds the module's own lock only to copy job fields in or
// out (never across a call), and hands everything visible to the DWM thread
// through dwm_post_message / dwm_invoke. Disk I/O happens with no lock held.
// Space for a file is reserved in steps, so even a huge file shows progress
// and can be cancelled while it is being prepared.
//
// Correctness: after reserving space the copier checks the file really got
// that size, and (FILECOPY_VERIFY) reads the copy back and compares it with
// the original. A copy that fails either check is deleted and reported,
// never left behind looking complete.
//
// Threading: thread context. These take the DWM lock themselves; calling
// them from a window procedure (where it is already held) is fine.

// Read the copy back and compare it with the original (doubles the reading)
#ifndef FILECOPY_VERIFY
#define FILECOPY_VERIFY         1
#endif

// Bytes per read / write
#ifndef FILECOPY_CHUNK
#define FILECOPY_CHUNK          4096
#endif

// Longest source / destination path
#define FILECOPY_PATH_MAX       256

// Flags for dwm_summon_filecopy
#define FILECOPY_MOVE           0x01    // Delete each original once its copy succeeded
#define FILECOPY_RENAME         0x02    // Destination taken: use "name (2)" etc. instead of failing
#define FILECOPY_HOLD           0x04    // Don't start until dwm_filecopy_start (add items first)

typedef struct {
    uint32_t copied;        // Items that arrived at their destination
    uint32_t failed;        // Items that did not (nothing is left behind for these)
    uint32_t skipped;       // Moves whose source and destination were the same
    bool     cancelled;     // The user pressed Cancel
} FileCopyResult;

// Called on the DWM thread, DWM lock held, when the job ends (right before
// the window closes). `result` is only valid during the call.
typedef void (*FileCopyDoneCallback)(const FileCopyResult* result, void* user);

// Open a copy window for one item: `src` is a file or folder, `dest` the
// full path it should end up at (a folder is copied with everything in it).
// Returns 0 if the window or the job could not be created.
//
// For several items, pass FILECOPY_HOLD, add the rest with
// dwm_filecopy_add, then call dwm_filecopy_start. Without HOLD the copy can
// finish item 1 before item 2 is added, and later adds are then refused.
WindowHandle dwm_summon_filecopy(const char* src, const char* dest, uint32_t flags);

// Add another item to the same window. Always succeeds on a held job.
// Returns false if the window is gone or the job has already finished.
bool dwm_filecopy_add(WindowHandle handle, const char* src, const char* dest);

// Let a job created with FILECOPY_HOLD run. Must be called once for every
// held job, or it blocks every copy queued behind it.
void dwm_filecopy_start(WindowHandle handle);

// Where the first desktop icon goes when items land in the desktop folder
// (the default is the next free spot on the desktop grid)
void dwm_filecopy_set_desktop_origin(WindowHandle handle, int x, int y);

// Get told how the job went (e.g. to finish a clipboard cut)
void dwm_filecopy_on_done(WindowHandle handle, FileCopyDoneCallback callback, void* user);

// True while any copy window has work queued or running (e.g. hold off shutting down)
bool dwm_filecopy_busy(void);

#endif
