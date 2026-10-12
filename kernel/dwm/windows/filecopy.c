#include <kernel/dwm/windows/filecopy.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/kernel.h>
#include <kernel/events.h>
#include <kernel/mutex.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/watchdog.h>
#include <kernel/malloc.h>
#include <kernel/vfs/vfs.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/string.h>

// ==========================================
// Threading
// ==========================================
//
//   DWM thread    creates jobs, adds items, draws the window, sets cancel.
//                 Holds the DWM lock, then takes fc_mutex briefly.
//   Copy thread   does the disk work. NEVER touches DWM state or takes the
//                 DWM lock. Everything visible is posted (dwm_post_message,
//                 dwm_invoke).
//
// fc_mutex rule: critical sections only read or write job fields. No calls
// out (no posting, no string work beyond memcpy, no 64-bit division, no
// VFS). Plain 64-bit loads and stores are fine. The watchdog caught the copy
// thread spinning while holding fc_mutex with the DWM blocked behind it;
// with this rule a stall in the copy thread can no longer freeze the desktop.
//
//   Written by the copy thread only:  phase, done, total, name, file_index,
//                                     file_total, drawn_percent, result,
//                                     buffers, item weights
//   Written by the DWM thread:        items (append), held, cancel,
//                                     desktop origin, done callback
//
// The copy thread may READ its own fields without the lock; every write to
// a field the DWM reads is done under fc_mutex.
//
// Lock order: DWM lock -> fc_mutex -> VFS lock. The copy thread never holds
// fc_mutex across a VFS call.

// ==========================================
// Progress
// ==========================================
//
// The bar covers the WHOLE job once, 0 to 100%, no matter how many files or
// folders it holds. Before anything is copied the job is counted (phase
// COUNTING): every item's tree is walked and its work added up in units:
//
//   file     size * FC_PASSES + 1   (copy pass, plus the verify pass)
//   folder   1
//
// The extra unit per entry lets empty files and folders move the bar too.
// While copying, `done` grows by the bytes copied/verified plus one unit per
// finished entry; percent = done / total. When a top-level item ends early
// (failed, skipped, a name taken) `done` jumps to the end of that item's
// share, so the bar still lands on 100%.
//
// Items added to a job that is already running (no FILECOPY_HOLD) are
// counted when they are reached; the total grows then, so the bar can step
// back a little. Use FILECOPY_HOLD for multi-item jobs to avoid that.

// ==========================================
// Layout (client coordinates, 6x8 font)
// ==========================================
#define FC_WIDTH           320
#define FC_HEIGHT          130     // Includes the 20px titlebar
#define FC_PAD             15
#define FC_STATUS_Y        12
#define FC_NAME_Y          26
#define FC_BAR_Y           46
#define FC_BAR_H           14
#define FC_PERCENT_Y       (FC_BAR_Y + FC_BAR_H + 8)
#define FC_BUTTON_W        64
#define FC_BUTTON_H        24
#define FC_FONT_W          6
#define FC_FONT_H          8
#define FC_KEY_ESCAPE      0x1B

#define FC_MAX_DEPTH       8       // Folder nesting the copier will follow
#define FC_MAX_ATTEMPTS    99      // "name (2)" ... "name (99)"
#define FC_CELL_W          90
#define FC_CELL_H          80
#define FC_GRID_ORIGIN     30
#define FC_MESSAGE_MAX     160

// Space is reserved in steps of this size, so the VFS lock is released
// between steps and Cancel is honoured while preparing
#define FC_RESERVE_STEP    (256u * 1024u)

// Posting to the DWM when its queues are full: retry this many times, 5 ms apart
#define FC_POST_RETRIES    400

// Times every byte goes through the bar: copied, then (optionally) verified
#if FILECOPY_VERIFY
#define FC_PASSES          2u
#else
#define FC_PASSES          1u
#endif

typedef enum {
    FC_PHASE_WAITING,              // Queued behind another window
    FC_PHASE_COUNTING,             // Sizing the whole job (no bar yet)
    FC_PHASE_PREPARING,            // Reserving space for the current file
    FC_PHASE_COPYING,
    FC_PHASE_VERIFYING,
} FileCopyPhase;

struct FileCopyItem {
    struct FileCopyItem* next;
    char src[FILECOPY_PATH_MAX];
    char dest[FILECOPY_PATH_MAX];

    // Copy thread only
    bool     counted;
    uint64_t weight;               // Work units for this whole item
};

struct FileCopyJob {
    // ---- Shared: written under fc_mutex ----
    struct FileCopyJob*  next;
    uint32_t             flags;

    struct FileCopyItem* items_head;
    struct FileCopyItem* items_tail;
    uint32_t             item_count;
    bool                 finishing;         // No more items are picked up
    bool                 held;              // FILECOPY_HOLD: waits for dwm_filecopy_start

    FileCopyPhase        phase;
    char                 name[VFS_NAME_MAX + 1];
    uint64_t             done;              // Work units finished (whole job)
    uint64_t             total;             // Work units counted (whole job)
    uint32_t             file_index;        // 1-based file being worked on
    uint32_t             file_total;        // Files counted

    int                  desktop_x;
    int                  desktop_y;

    FileCopyDoneCallback done_callback;
    void*                done_user;

    volatile bool        cancel;

    // ---- Fixed before the job is published ----
    WindowHandle         window;
    char                 desktop_dir[FILECOPY_PATH_MAX];
    bool                 has_desktop;

    // ---- Copy thread only ----
    int                  drawn_percent;     // Last percent successfully posted
    bool                 used_origin;
    FileCopyResult       result;
    const char*          last_error;
    uint8_t*             buffer;
    uint8_t*             verify_buffer;
};

// What the DWM needs to draw a window, copied out under fc_mutex
typedef struct {
    uint32_t      flags;
    FileCopyPhase phase;
    char          name[VFS_NAME_MAX + 1];
    uint64_t      done;
    uint64_t      total;
    uint32_t      file_index;
    uint32_t      file_total;
    bool          cancel;
} FcView;

static mutex_t             fc_mutex;
static ThreadBlock*        fc_thread   = NULL;
static Event               fc_event    = EVENT_INITIALIZER;
static struct FileCopyJob* fc_jobs     = NULL;    // FIFO, head is running

static void fc_unlock(void) { mutex_unlock(&fc_mutex); }
static void fc_lock(void)   { mutex_lock(&fc_mutex); }

// ==========================================
// Small helpers
// ==========================================

static size_t fc_u32_to_str(uint32_t value, char* out) {
    char digits[11];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    for (size_t i = 0; i < n; i++) out[i] = digits[n - 1 - i];
    out[n] = '\0';
    return n;
}

static const char* fc_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

static void fc_parent(const char* path, char* out, size_t size) {
    memset(out, '\0', size);
    strncpy(out, path, size - 1);

    char* slash = strrchr(out, '/');
    if (slash == NULL)      out[0] = '\0';
    else if (slash == out)  out[1] = '\0';
    else                    *slash = '\0';
}

static bool fc_join(char* out, size_t size, const char* dir, const char* name) {
    size_t dir_len  = strlen(dir);
    size_t name_len = strlen(name);
    bool   slash    = (dir_len == 0 || dir[dir_len - 1] != '/');

    if (dir_len + (slash ? 1 : 0) + name_len + 1 > size) return false;

    memcpy(out, dir, dir_len);
    if (slash) out[dir_len++] = '/';
    memcpy(&out[dir_len], name, name_len + 1);
    return true;
}

// True when `path` is `base` itself or somewhere below it
static bool fc_path_is_within(const char* path, const char* base) {
    size_t base_len = strlen(base);
    while (base_len > 1 && base[base_len - 1] == '/') base_len--;

    if (strncmp(path, base, base_len) != 0) return false;
    return path[base_len] == '\0' || path[base_len] == '/';
}

// done / size scaled to 0..span, in 32-bit arithmetic only (no __udivdi3
// on i386). Returns span only once done >= size, never earlier.
static uint32_t fc_scale(uint32_t done, uint32_t size, uint32_t span) {
    if (done >= size) return span;

    uint32_t scaled;
    if (size <= 0xFFFFFFFFu / span)
        scaled = (done * span) / size;
    else
        scaled = done / (size / span);      // size > 0xFFFFFFFF / span, so size / span > 0

    return (scaled >= span) ? span - 1 : scaled;
}

// Whole-job bar position. The 64-bit counters are shifted down until the
// total fits 32 bits (shifts and compares only, no 64-bit division), then
// scaled. 100% only once everything is done. Waiting and counting have no
// bar, so they report 0.
static int fc_percent_of(FileCopyPhase phase, uint64_t done, uint64_t total) {
    if (phase == FC_PHASE_WAITING || phase == FC_PHASE_COUNTING)
        return 0;
    if (total == 0 || done >= total)
        return 100;

    uint64_t d = done;
    uint64_t t = total;
    while (t > 0xFFFFFFFFull) {
        t >>= 1;
        d >>= 1;
    }

    uint32_t percent = fc_scale((uint32_t)d, (uint32_t)t, 100u);
    return (percent > 99) ? 99 : (int)percent;
}

// Work units for one file (see "Progress" above)
static uint64_t fc_file_weight(uint32_t size) {
    return (uint64_t)size * FC_PASSES + 1;
}

// fc_mutex held
static struct FileCopyJob* fc_find(WindowHandle handle) {
    for (struct FileCopyJob* job = fc_jobs; job != NULL; job = job->next) {
        if (job->window == handle) return job;
    }
    return NULL;
}

// ==========================================
// Shared helpers (declared in dwm_core_internal.h)
// ==========================================

// Name for attempt n: n == 1 is the name itself, n >= 2 is "stem (n).ext",
// with the stem shortened so the result fits VFS_NAME_MAX. Short extensions
// (".txt") are kept, longer ones are treated as part of the stem.
static void fc_candidate_name(const char* name, uint32_t n, char* out /* VFS_NAME_MAX + 1 */) {
    size_t len = strnlen(name, VFS_NAME_MAX);

    if (n <= 1) {
        memcpy(out, name, len);
        out[len] = '\0';
        return;
    }

    char suffix[8] = " (";
    size_t suffix_len = 2 + fc_u32_to_str(n, &suffix[2]);
    suffix[suffix_len++] = ')';
    suffix[suffix_len] = '\0';

    const char* dot = strrchr(name, '.');
    size_t ext_len = (dot != NULL && dot != name) ? strnlen(dot, VFS_NAME_MAX) : 0;
    if (ext_len > 5 || ext_len >= len) ext_len = 0;

    size_t stem_len = len - ext_len;
    size_t room = VFS_NAME_MAX - suffix_len - ext_len;
    if (stem_len > room) stem_len = room;
    while (stem_len > 1 && name[stem_len - 1] == ' ') stem_len--;   // No "abc  (2)"

    size_t o = 0;
    memcpy(&out[o], name, stem_len);                o += stem_len;
    memcpy(&out[o], suffix, suffix_len);            o += suffix_len;
    memcpy(&out[o], &name[len - ext_len], ext_len); o += ext_len;
    out[o] = '\0';
}

bool dwm_path_unique(const char* dir, const char* name, char* out_path, size_t path_size, char* out_name) {
    for (uint32_t n = 1; n <= FC_MAX_ATTEMPTS; n++) {
        fc_candidate_name(name, n, out_name);
        if (!fc_join(out_path, path_size, dir, out_name)) return false;
        if (!vfs_exists(out_path)) return true;
    }
    return false;
}

bool dwm_desktop_get_directory(char* out, size_t size) {
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);
    if (paths.home[0] == '\0') return false;

    memset(out, '\0', size);
    strncpy(out, paths.home, size - 1);
    strncat(out, "/usr/desktop", size - strlen(out) - 1);
    return true;
}

static bool fc_cell_is_free(int x, int y) {
    for (struct list_node* node = workspace.icon_head; node != NULL; node = node->next) {
        struct IconObject* icon = (struct IconObject*)node->data;
        if ((int)icon->x < x + FC_CELL_W && (int)icon->x + FC_CELL_W > x &&
            (int)icon->y < y + FC_CELL_H && (int)icon->y + FC_CELL_H > y)
            return false;
    }
    return true;
}

// DWM thread / DWM lock (the copy thread reaches it through dwm_invoke)
void dwm_desktop_add_item_icon(const char* path, const char* name, int x, int y) {
    DWM_GUARD();
    int max_x = display_get_width()  - FC_CELL_W;
    int max_y = display_get_height() - taskbar.height - FC_CELL_H;

    if (x >= 0 && y >= 0) {
        // Requested spot, kept on screen above the taskbar
        if (x > max_x) x = max_x;
        if (y > max_y) y = max_y;
    } else {
        // Next free grid cell, row by row like the boot-time layout
        x = FC_GRID_ORIGIN;
        y = FC_GRID_ORIGIN;
        bool found = false;
        for (int cy = FC_GRID_ORIGIN; cy <= max_y && !found; cy += FC_CELL_H) {
            for (int cx = FC_GRID_ORIGIN; cx <= max_x; cx += FC_CELL_W) {
                if (fc_cell_is_free(cx, cy)) {
                    x = cx;
                    y = cy;
                    found = true;
                    break;
                }
            }
        }
    }

    if (vfs_directory_check(path)) {
        dwm_create_folder((uint16_t)x, (uint16_t)y, name, path);
    } else {
        dwm_create_file((uint16_t)x, (uint16_t)y, name, path);
    }
}

void dwm_desktop_forget_path(const char* removed_path) {
    DWM_GUARD();
    struct list_node* node = workspace.icon_head;
    while (node != NULL) {
        struct list_node* next = node->next;   // dwm_destroy_icon frees `node`
        struct IconObject* icon = (struct IconObject*)node->data;

        if (icon != NULL && fc_path_is_within(icon->path, removed_path))
            dwm_destroy_icon(icon);

        node = next;
    }
}

// ==========================================
// Desktop work posted from the copy thread (runs on the DWM thread)
// ==========================================

struct FcIconMsg {
    char path[FILECOPY_PATH_MAX];
    char name[VFS_NAME_MAX + 1];
    int  x, y;
};

struct FcBoxMsg {
    char title[16];
    char message[FC_MESSAGE_MAX];
};

struct FcDoneMsg {
    FileCopyDoneCallback callback;
    void*                user;
    FileCopyResult       result;
};

static void fc_ui_add_icon(void* arg) {
    struct FcIconMsg* m = (struct FcIconMsg*)arg;
    dwm_desktop_add_item_icon(m->path, m->name, m->x, m->y);
    free(m);
}

static void fc_ui_forget_path(void* arg) {
    dwm_desktop_forget_path((const char*)arg);
    free(arg);
}

static void fc_ui_refresh(void* arg) {
    (void)arg;
    dwm_refresh_all_windows();
}

static void fc_ui_message_box(void* arg) {
    struct FcBoxMsg* m = (struct FcBoxMsg*)arg;
    dwm_notify(m->title, m->message, DWM_NOTIFY_LONG_MS);
    free(m);
}

static void fc_ui_done(void* arg) {
    struct FcDoneMsg* m = (struct FcDoneMsg*)arg;
    m->callback(&m->result, m->user);
    free(m);
}

// Copy thread: queue fn(arg) on the DWM thread. dwm_invoke fails when its
// queue is full or out of memory; retry for a while instead of silently
// dropping (a dropped done callback used to leave a cut unfinished).
static void fc_post(DWMInvokeFn fn, void* arg) {
    for (int i = 0; i < FC_POST_RETRIES; i++) {
        if (dwm_invoke(fn, arg)) return;
        wd_pet();
        thread_sleep(5);
    }
    if (arg != NULL) free(arg);
}

static void fc_post_add_icon(const char* path, const char* name, int x, int y) {
    struct FcIconMsg* m = (struct FcIconMsg*)malloc(sizeof(struct FcIconMsg));
    if (m == NULL) return;
    memset(m, 0, sizeof(*m));
    strncpy(m->path, path, sizeof(m->path) - 1);
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->x = x;
    m->y = y;
    fc_post(fc_ui_add_icon, m);
}

static void fc_post_forget_path(const char* path) {
    size_t len = strnlen(path, FILECOPY_PATH_MAX - 1);
    char* copy = (char*)malloc(len + 1);
    if (copy == NULL) return;
    memcpy(copy, path, len);
    copy[len] = '\0';
    fc_post(fc_ui_forget_path, copy);
}

// ==========================================
// Window updates (copy thread, fc_mutex NOT held)
// ==========================================

// Post a repaint when the percent moved (or always, with force). The
// percent is only recorded once the post succeeded: a REDRAW dropped
// because the queue was full is retried on the next update instead of
// leaving the bar stuck.
static void fc_redraw(struct FileCopyJob* job, bool force) {
    int percent = fc_percent_of(job->phase, job->done, job->total);
    if (!force && percent == job->drawn_percent) return;

    if (job->window != 0 && dwm_post_message(job->window, DWM_EVENT_REDRAW, 0, 0))
        job->drawn_percent = percent;
    else
        job->drawn_percent = -1;
}

// Copy `path`'s last segment into `name` (outside the lock)
static void fc_name_of(const char* path, char* name /* VFS_NAME_MAX + 1 */) {
    memset(name, '\0', VFS_NAME_MAX + 1);
    strncpy(name, fc_basename(path), VFS_NAME_MAX);
}

// New phase (and optionally a new name to show): always repaint.
// Progress is NOT reset: the bar belongs to the whole job.
static void fc_set_stage(struct FileCopyJob* job, FileCopyPhase phase, const char* path) {
    wd_pet();

    char name[VFS_NAME_MAX + 1];
    if (path != NULL) fc_name_of(path, name);

    fc_lock();
    job->phase = phase;
    if (path != NULL) memcpy(job->name, name, sizeof(job->name));
    fc_unlock();

    fc_redraw(job, true);
}

// Next file starts (reserving its space first): "n of N" moves on
static void fc_begin_file(struct FileCopyJob* job, const char* path) {
    wd_pet();

    char name[VFS_NAME_MAX + 1];
    fc_name_of(path, name);

    fc_lock();
    job->phase = FC_PHASE_PREPARING;
    job->file_index++;
    memcpy(job->name, name, sizeof(job->name));
    fc_unlock();

    fc_redraw(job, true);
}

// Whole-job progress: repaint only when the percent moves
static void fc_set_done(struct FileCopyJob* job, uint64_t done) {
    wd_pet();

    fc_lock();
    job->done = done;
    fc_unlock();

    fc_redraw(job, false);
}

static void fc_add_done(struct FileCopyJob* job, uint64_t units) {
    fc_set_done(job, job->done + units);
}

// ==========================================
// Counting (copy thread, no locks held on entry or exit)
// ==========================================

// Work units for `src` and everything below it. Follows the same rules as
// fc_copy_any (mounted devices and too-deep folders are not entered, the
// copy will refuse them), so the count matches what the copy will process.
static uint64_t fc_count_any(struct FileCopyJob* job, const char* src, uint32_t depth, uint32_t* files) {
    if (job->cancel) return 1;
    wd_pet();

    if (vfs_directory_check_mounted(src)) return 1;

    if (!vfs_directory_check(src)) {
        uint32_t size = 0;
        File file = vfs_open(src, VFS_OPEN_READ);
        if (file != VFS_INVALID_FILE) {
            size = vfs_get_size(file);
            vfs_close(file);
        }
        (*files)++;
        return fc_file_weight(size);
    }

    if (depth >= FC_MAX_DEPTH) return 1;

    // On the heap: the copy thread's stack is 16 KB and this recurses
    char* child = (char*)malloc(FILECOPY_PATH_MAX);
    if (child == NULL) return 1;

    uint64_t weight = 1;    // The folder itself
    uint32_t count = vfs_directory_get_item_count(src);

    for (uint32_t i = 0; i < count && !job->cancel; i++) {
        char name[VFS_NAME_MAX + 1];
        memset(name, '\0', sizeof(name));
        if (!vfs_directory_get_item(src, i, name, VFS_NAME_MAX)) continue;
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!fc_join(child, FILECOPY_PATH_MAX, src, name)) continue;

        weight += fc_count_any(job, child, depth + 1, files);
    }

    free(child);
    return weight;
}

// Count one top-level item (once) and add it to the job's totals
static void fc_count_item(struct FileCopyJob* job, struct FileCopyItem* item) {
    if (item->counted) return;

    uint32_t files = 0;
    uint64_t weight = vfs_exists(item->src) ? fc_count_any(job, item->src, 0, &files) : 1;

    item->weight  = weight;
    item->counted = true;

    uint64_t total      = job->total + weight;
    uint32_t file_total = job->file_total + files;

    fc_lock();
    job->total      = total;
    job->file_total = file_total;
    fc_unlock();
}

// fc_mutex taken inside: the DWM thread may be appending
static struct FileCopyItem* fc_item_after(struct FileCopyJob* job, struct FileCopyItem* item) {
    fc_lock();
    struct FileCopyItem* next = (item == NULL) ? job->items_head : item->next;
    fc_unlock();
    return next;
}

// Size the whole job before copying anything
static void fc_count_job(struct FileCopyJob* job) {
    for (struct FileCopyItem* item = fc_item_after(job, NULL);
         item != NULL && !job->cancel;
         item = fc_item_after(job, item)) {
        fc_set_stage(job, FC_PHASE_COUNTING, item->src);
        fc_count_item(job, item);
    }
}

// ==========================================
// Copying (copy thread, no locks held on entry or exit)
// ==========================================

// vfs_read / vfs_write may move fewer bytes than asked; loop until done
static bool fc_read_full(File file, uint8_t* buffer, uint32_t count) {
    uint32_t got = 0;
    while (got < count) {
        int32_t n = vfs_read(file, buffer + got, count - got);
        if (n <= 0) return false;
        got += (uint32_t)n;
    }
    return true;
}

static bool fc_write_full(File file, const uint8_t* buffer, uint32_t count) {
    uint32_t put = 0;
    while (put < count) {
        int32_t n = vfs_write(file, buffer + put, count - put);
        if (n <= 0) return false;
        put += (uint32_t)n;
    }
    return true;
}

// Every chunk seeks to its own offset instead of trusting the file position
// left by the last call: other threads use the VFS between our chunks.
static bool fc_seek(File file, uint32_t offset) {
    return vfs_seek(file, offset) == offset;
}

// Grow `dest` to `size` in FC_RESERVE_STEP increments. One vfs_truncate for
// the whole file held the VFS lock for as long as the reservation took
// (minutes for a large file with the old per-sector ATA flush), with no
// watchdog pet and no way to cancel. The window says "Preparing" while this
// runs (the bar holds still), so steps only pet the watchdog and check Cancel.
//
// NOTE: assumes growing a file costs time proportional to the growth. If
// the file system rewrites the whole chain on every resize, raise the step.
static bool fc_reserve(struct FileCopyJob* job, const char* dest, uint32_t size) {
    for (uint32_t reserved = 0; reserved < size; ) {
        if (job->cancel) return false;

        uint32_t step = size - reserved;
        if (step > FC_RESERVE_STEP) step = FC_RESERVE_STEP;
        reserved += step;

        if (!vfs_truncate(dest, reserved)) {
            job->last_error = "there was not enough space for a copy";
            return false;
        }
        wd_pet();
    }

    // vfs_truncate reports success even when the file system could not
    // resize (do_truncate ignores fs_file_resize's result), so check the
    // size the file really has. Writing past it would silently lose data.
    FSFileStats stats;
    if (!vfs_stat(dest, &stats) || stats.size != size) {
        job->last_error = "there was not enough space for a copy";
        return false;
    }
    return !job->cancel;
}

static bool fc_copy_file(struct FileCopyJob* job, const char* src, const char* dest) {
    fc_begin_file(job, src);

    File in = vfs_open(src, VFS_OPEN_READ);
    if (in == VFS_INVALID_FILE) {
        job->last_error = "an original could not be opened";
        return false;
    }
    uint32_t size = vfs_get_size(in);

    File out = vfs_open(dest, VFS_OPEN_CREATE | VFS_OPEN_READ | VFS_OPEN_WRITE);
    if (out == VFS_INVALID_FILE) {
        vfs_close(in);
        job->last_error = "a copy could not be created";
        return false;
    }
    vfs_close(out);
    out = VFS_INVALID_FILE;

    // Reserve the space first: writes don't reliably extend a file
    bool ok = fc_reserve(job, dest, size);

    if (ok && size > 0) {
        out = vfs_open(dest, VFS_OPEN_READ | VFS_OPEN_WRITE);
        if (out == VFS_INVALID_FILE) {
            ok = false;
            job->last_error = "a copy could not be opened for writing";
        }
    }

    // ---- Copy: every byte moves the whole-job bar ----
    if (ok && size > 0) {
        fc_set_stage(job, FC_PHASE_COPYING, NULL);
        uint64_t base = job->done;

        for (uint32_t offset = 0; offset < size; ) {
            uint32_t n = size - offset;
            if (n > FILECOPY_CHUNK) n = FILECOPY_CHUNK;

            ok = fc_seek(in, offset)  && fc_read_full(in, job->buffer, n) &&
                 fc_seek(out, offset) && fc_write_full(out, job->buffer, n);

            if (!ok) {
                job->last_error = "reading or writing failed";
                break;
            }

            offset += n;
            fc_set_done(job, base + offset);

            if (job->cancel) {
                ok = false;
                break;
            }
        }
    }

#if FILECOPY_VERIFY
    // ---- Verify: read the copy back and compare (second pass of its bytes) ----
    if (ok && size > 0) {
        fc_set_stage(job, FC_PHASE_VERIFYING, NULL);
        uint64_t base = job->done;

        for (uint32_t offset = 0; offset < size; ) {
            uint32_t n = size - offset;
            if (n > FILECOPY_CHUNK) n = FILECOPY_CHUNK;

            bool same = fc_seek(in, offset)  && fc_read_full(in, job->buffer, n) &&
                        fc_seek(out, offset) && fc_read_full(out, job->verify_buffer, n);
            if (same && memcmp(job->buffer, job->verify_buffer, n) != 0)
                same = false;

            if (!same) {
                ok = false;
                job->last_error = "a copy did not match its original";
                break;
            }

            offset += n;
            fc_set_done(job, base + offset);

            if (job->cancel) {
                ok = false;
                break;
            }
        }
    }
#endif

    if (out != VFS_INVALID_FILE) vfs_close(out);
    vfs_close(in);

    // Same permissions as the original
    if (ok) {
        uint8_t perm = 0;
        if (vfs_get_permissions(src, &perm))
            vfs_set_permissions(dest, perm);
    }

    // The file's own unit (empty files move the bar this way)
    if (ok) fc_add_done(job, 1);

    return ok;
}

static bool fc_copy_any(struct FileCopyJob* job, const char* src, const char* dest, uint32_t depth);

static bool fc_copy_directory(struct FileCopyJob* job, const char* src, const char* dest, uint32_t depth) {
    if (depth >= FC_MAX_DEPTH) {
        job->last_error = "folders are nested too deeply";
        return false;
    }

    if (!vfs_mkdir(dest) || !vfs_directory_check(dest)) {
        job->last_error = "a folder could not be created";
        return false;
    }

    // The folder's own unit
    fc_add_done(job, 1);

    // On the heap: the copy thread's stack is 16 KB and this recurses
    char* child_src  = (char*)malloc(FILECOPY_PATH_MAX);
    char* child_dest = (char*)malloc(FILECOPY_PATH_MAX);
    if (child_src == NULL || child_dest == NULL) {
        if (child_src)  free(child_src);
        if (child_dest) free(child_dest);
        job->last_error = "out of memory";
        return false;
    }

    bool ok = true;
    uint32_t count = vfs_directory_get_item_count(src);

    for (uint32_t i = 0; i < count && ok; i++) {
        char name[VFS_NAME_MAX + 1];
        memset(name, '\0', sizeof(name));
        if (!vfs_directory_get_item(src, i, name, sizeof(name))) continue;
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        if (!fc_join(child_src,  FILECOPY_PATH_MAX, src,  name) ||
            !fc_join(child_dest, FILECOPY_PATH_MAX, dest, name)) {
            job->last_error = "a path was too long";
            ok = false;
            break;
        }

        ok = fc_copy_any(job, child_src, child_dest, depth + 1);
    }

    free(child_src);
    free(child_dest);

    if (ok) {
        uint8_t perm = 0;
        if (vfs_get_permissions(src, &perm))
            vfs_set_permissions(dest, perm);
    }

    return ok;
}

static bool fc_copy_any(struct FileCopyJob* job, const char* src, const char* dest, uint32_t depth) {
    if (job->cancel) return false;

    // Never descend into another device mounted somewhere inside the tree
    if (vfs_directory_check_mounted(src)) {
        job->last_error = "storage devices cannot be copied";
        return false;
    }

    if (vfs_directory_check(src))
        return fc_copy_directory(job, src, dest, depth);
    return fc_copy_file(job, src, dest);
}

// One top-level item. Returns false when the job should stop (cancelled).
static bool fc_run_item_body(struct FileCopyJob* job, struct FileCopyItem* item) {
    char dest[FILECOPY_PATH_MAX];
    char dest_dir[FILECOPY_PATH_MAX];
    char dest_name[VFS_NAME_MAX + 1];
    bool move = (job->flags & FILECOPY_MOVE) != 0;

    fc_set_stage(job, FC_PHASE_PREPARING, item->src);

    if (!vfs_exists(item->src)) {
        job->last_error = "an original no longer exists";
        job->result.failed++;
        return true;
    }

    // Moving something onto itself: nothing to do
    if (move && strcmp(item->src, item->dest) == 0) {
        job->result.skipped++;
        return true;
    }

    // Settle the final destination name
    fc_parent(item->dest, dest_dir, sizeof(dest_dir));
    strncpy(dest, item->dest, sizeof(dest) - 1);
    dest[sizeof(dest) - 1] = '\0';
    strncpy(dest_name, fc_basename(item->dest), sizeof(dest_name) - 1);
    dest_name[sizeof(dest_name) - 1] = '\0';

    if (vfs_exists(dest)) {
        if (!(job->flags & FILECOPY_RENAME) ||
            !dwm_path_unique(dest_dir, fc_basename(item->dest), dest, sizeof(dest), dest_name)) {
            job->last_error = "the destination name is taken";
            job->result.failed++;
            return true;
        }
    }

    // Checked on the final name: pasting a folder next to itself becomes
    // "dir (2)", which is fine, but "dir/inner" never is
    if (fc_path_is_within(dest, item->src)) {
        job->last_error = "a folder cannot be copied into itself";
        job->result.failed++;
        return true;
    }

    // Nothing exists at `dest`, so anything there after a failure is ours
    if (!fc_copy_any(job, item->src, dest, 0)) {
        if (vfs_exists(dest)) vfs_remove(dest);

        if (job->cancel) {
            job->result.cancelled = true;
            return false;
        }
        job->result.failed++;
        return true;
    }

    job->result.copied++;

    if (job->has_desktop && strcmp(dest_dir, job->desktop_dir) == 0) {
        int x = -1, y = -1;

        fc_lock();
        if (!job->used_origin && job->desktop_x >= 0 && job->desktop_y >= 0) {
            job->used_origin = true;
            x = job->desktop_x;
            y = job->desktop_y;
        }
        fc_unlock();

        fc_post_add_icon(dest, dest_name, x, y);
    }

    if (move) {
        // A move is a copy plus a delete. If the delete fails the item
        // exists twice, which is safer than losing it.
        if (vfs_remove(item->src)) {
            fc_post_forget_path(item->src);
        } else {
            job->last_error = "an original could not be removed after copying";
            job->result.failed++;
        }
    }

    // Let open explorer windows show each item as it lands
    fc_post(fc_ui_refresh, NULL);
    return true;
}

// Run one item, then make sure the bar sits at the end of its share. An
// item that failed or was skipped part way leaves its remaining units
// unprocessed; without this the bar would stop short of 100%.
static bool fc_run_item(struct FileCopyJob* job, struct FileCopyItem* item) {
    uint64_t start = job->done;
    bool keep_going = fc_run_item_body(job, item);

    uint64_t end = start + item->weight;
    if (keep_going && job->done < end)
        fc_set_done(job, end);

    return keep_going;
}

// Failures, or (like the delete window) what already landed when cancelled.
// For a move that matters: those originals are already gone.
static void fc_report(struct FileCopyJob* job) {
    bool move = (job->flags & FILECOPY_MOVE) != 0;
    struct FcBoxMsg* m = NULL;

    if (job->result.failed > 0 && job->last_error != NULL) {
        m = (struct FcBoxMsg*)malloc(sizeof(struct FcBoxMsg));
        if (m == NULL) return;
        memset(m, 0, sizeof(*m));

        char* message = m->message;
        size_t n = fc_u32_to_str(job->result.failed, message);
        message[n] = '\0';
        strncat(message, (job->result.failed == 1) ? " item" : " items", FC_MESSAGE_MAX - strlen(message) - 1);
        strncat(message, " could not be ", FC_MESSAGE_MAX - strlen(message) - 1);
        strncat(message, move ? "moved: " : "copied: ", FC_MESSAGE_MAX - strlen(message) - 1);
        strncat(message, job->last_error, FC_MESSAGE_MAX - strlen(message) - 1);
    } else if (job->result.cancelled && job->result.copied > 0) {
        m = (struct FcBoxMsg*)malloc(sizeof(struct FcBoxMsg));
        if (m == NULL) return;
        memset(m, 0, sizeof(*m));

        char* message = m->message;
        strncpy(message, "Cancelled. ", FC_MESSAGE_MAX - 1);
        size_t len = strlen(message);
        fc_u32_to_str(job->result.copied, &message[len]);
        strncat(message, (job->result.copied == 1) ? " item was already " : " items were already ",
                FC_MESSAGE_MAX - strlen(message) - 1);
        strncat(message, move ? "moved" : "copied", FC_MESSAGE_MAX - strlen(message) - 1);
    }

    if (m == NULL) return;
    strncpy(m->title, move ? "Move" : "Copy", sizeof(m->title) - 1);
    fc_post(fc_ui_message_box, m);
}

static void fc_free_job(struct FileCopyJob* job) {
    struct FileCopyItem* item = job->items_head;
    while (item != NULL) {
        struct FileCopyItem* next = item->next;
        free(item);
        item = next;
    }
    if (job->buffer != NULL)        free(job->buffer);
    if (job->verify_buffer != NULL) free(job->verify_buffer);
    free(job);
}

// Next item to copy (first when `item` is NULL). When there is none the job
// is marked finishing in the same critical section, so dwm_filecopy_add can
// never append an item that would silently be skipped.
static struct FileCopyItem* fc_next_item(struct FileCopyJob* job, struct FileCopyItem* item) {
    fc_lock();
    struct FileCopyItem* next = (item == NULL) ? job->items_head : item->next;
    if (next == NULL || job->cancel) {
        job->finishing = true;
        next = NULL;
    }
    fc_unlock();
    return next;
}

static void fc_run_job(struct FileCopyJob* job) {
    job->buffer        = (uint8_t*)malloc(FILECOPY_CHUNK);
    job->verify_buffer = (uint8_t*)malloc(FILECOPY_CHUNK);
    if (job->buffer == NULL || job->verify_buffer == NULL) {
        fc_lock();
        job->finishing = true;
        uint32_t count = job->item_count;
        fc_unlock();
        job->result.failed = count;
        job->last_error = "out of memory";
        return;
    }

    // ---- Counting: size the whole job so the bar runs 0-100% once ----
    fc_count_job(job);

    for (struct FileCopyItem* item = fc_next_item(job, NULL); item != NULL;
         item = fc_next_item(job, item)) {
        // Added after counting (job without FILECOPY_HOLD): count it now
        fc_count_item(job, item);
        if (!fc_run_item(job, item)) break;
    }

    if (job->cancel) job->result.cancelled = true;
}

static void fc_thread_main(void) {
    while (1) {
        event_wait(&fc_event, 0);           // 0 = until signaled
        wd_pet();

        while (1) {
            fc_lock();
            struct FileCopyJob* job = fc_jobs;
            bool held = (job != NULL) && job->held;
            fc_unlock();

            // A held job is still being filled; dwm_filecopy_start signals
            if (job == NULL || held) break;

            fc_run_job(job);

            // Done callback runs on the DWM thread (it touches the clipboard
            // and the desktop), with a copy of the result
            fc_lock();
            FileCopyDoneCallback callback = job->done_callback;
            void* user = job->done_user;
            fc_unlock();

            if (callback != NULL) {
                struct FcDoneMsg* m = (struct FcDoneMsg*)malloc(sizeof(struct FcDoneMsg));
                if (m != NULL) {
                    m->callback = callback;
                    m->user     = user;
                    m->result   = job->result;
                    fc_post(fc_ui_done, m);
                }
            }
            fc_report(job);

            // Off the list first: from here on the window's handler finds
            // nothing and ignores whatever events are still queued for it
            fc_lock();
            fc_jobs = job->next;
            struct FileCopyJob* next_job = fc_jobs;
            if (next_job != NULL) next_job->phase = FC_PHASE_COUNTING;
            fc_unlock();

            // Only this thread frees jobs, so next_job stays valid here
            if (next_job != NULL) fc_redraw(next_job, true);

            // The window has no close box, so a dropped CLOSE used to leave
            // it on screen forever with nothing behind it. Never give up.
            if (job->window != 0) {
                while (!dwm_post_message(job->window, DWM_EVENT_CLOSE, 0, 0)) {
                    wd_pet();
                    thread_sleep(5);
                }
            }
            fc_free_job(job);
        }
    }
}

// ==========================================
// Drawing (DWM thread, fc_mutex NOT held)
// ==========================================

static bool fc_snapshot(WindowHandle handle, FcView* view) {
    bool found = false;

    fc_lock();
    struct FileCopyJob* job = fc_find(handle);
    if (job != NULL) {
        view->flags      = job->flags;
        view->phase      = job->phase;
        view->done       = job->done;
        view->total      = job->total;
        view->file_index = job->file_index;
        view->file_total = job->file_total;
        view->cancel     = job->cancel;
        memcpy(view->name, job->name, sizeof(view->name));
        found = true;
    }
    fc_unlock();

    view->name[sizeof(view->name) - 1] = '\0';
    return found;
}

static void fc_draw(struct WindowObject* window, const FcView* v) {
    bool move = (v->flags & FILECOPY_MOVE) != 0;

    // The bar exists once the job has been counted
    bool has_bar = (v->phase != FC_PHASE_WAITING && v->phase != FC_PHASE_COUNTING);

    dwm_draw_rect_filled(0, 0, window->w, window->h, theme.client.background);

    // Status line
    char status[48];
    status[0] = '\0';
    switch (v->phase) {
    case FC_PHASE_WAITING:   strncpy(status, "Waiting for another copy", sizeof(status) - 1); break;
    case FC_PHASE_COUNTING:  strncpy(status, "Counting items", sizeof(status) - 1); break;
    case FC_PHASE_PREPARING: //strncpy(status, "Preparing", sizeof(status) - 1); break;
    case FC_PHASE_VERIFYING: //strncpy(status, "Verifying", sizeof(status) - 1); break;
    case FC_PHASE_COPYING:   strncpy(status, move ? "Moving" : "Copying", sizeof(status) - 1); break;
    }
    status[sizeof(status) - 1] = '\0';

    // "file n of N" across the whole job (folders included)
    if (has_bar && v->file_total > 1 && v->file_index > 0 && v->file_index <= v->file_total) {
        char num[12];
        strncat(status, " ", sizeof(status) - strlen(status) - 1);
        fc_u32_to_str(v->file_index, num);
        strncat(status, num, sizeof(status) - strlen(status) - 1);
        strncat(status, " of ", sizeof(status) - strlen(status) - 1);
        fc_u32_to_str(v->file_total, num);
        strncat(status, num, sizeof(status) - strlen(status) - 1);
    }
    dwm_draw_text(FC_PAD, FC_STATUS_Y, status, theme.client.text);
    dwm_draw_text(FC_PAD, FC_NAME_Y, v->name, theme.client.text_value);

    // One bar for the whole job
    int bar_w   = (int)window->w - (FC_PAD * 2);
    int percent = fc_percent_of(v->phase, v->done, v->total);

    if (has_bar) {
        dwm_draw_rect_filled(FC_PAD, FC_BAR_Y, bar_w, FC_BAR_H, theme.edit.background);
        if (percent > 0) {
            dwm_draw_rect_filled(FC_PAD, FC_BAR_Y, (bar_w * percent) / 100, FC_BAR_H, theme.client.accent);
        }
        dwm_draw_rect(FC_PAD - 1, FC_BAR_Y - 1, bar_w + 2, FC_BAR_H + 2, theme.edit.border);
    }

    // Percent, or the cancel acknowledgement
    if (v->cancel) {
        dwm_draw_text(FC_PAD, FC_PERCENT_Y, "Cancelling...", theme.client.text_muted);
    } else if (has_bar) {
        char pct[8];
        size_t n = fc_u32_to_str((uint32_t)percent, pct);
        pct[n]     = '%';
        pct[n + 1] = '\0';
        dwm_draw_text(FC_PAD, FC_PERCENT_Y, pct, theme.client.text_muted);
    }

    // Cancel button (bottom right of the client area)
    int client_h = (int)window->h - (int)window->titlebar_height;
    int btn_x = (int)window->w - FC_PAD - FC_BUTTON_W;
    int btn_y = client_h - FC_PAD - FC_BUTTON_H + 4;

    const char* label = "Cancel";
    int label_w = (int)strlen(label) * FC_FONT_W;

    dwm_draw_rect_filled(btn_x, btn_y, FC_BUTTON_W, FC_BUTTON_H, theme.button.fill);
    dwm_draw_rect(btn_x, btn_y, FC_BUTTON_W, FC_BUTTON_H, theme.button.border);
    dwm_draw_text(btn_x + (FC_BUTTON_W - label_w) / 2, btn_y + (FC_BUTTON_H - FC_FONT_H) / 2,
                  label, v->cancel ? theme.client.text_muted : theme.button.text);
}

// ==========================================
// Event handler (DWM thread, DWM lock held)
// ==========================================

static void fc_request_cancel(WindowHandle handle) {
    bool changed = false;

    fc_lock();
    struct FileCopyJob* job = fc_find(handle);
    if (job != NULL && !job->cancel) {
        job->cancel = true;
        changed = true;
    }
    fc_unlock();

    if (changed) dwm_post_message(handle, DWM_EVENT_REDRAW, 0, 0);
}

void callback_filecopy_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return;

    switch (event) {
        case DWM_EVENT_REDRAW: {
            FcView view;
            if (fc_snapshot(handle, &view)) fc_draw(window, &view);
            break;
        }

        case DWM_EVENT_MOUSE: {
            if (!(lparam & DWM_STATE_MOUSE_BTN_LEFT)) break;

            int click_x = (int16_t)(wparam & 0xFFFF);
            int click_y = (int16_t)((wparam >> 16) & 0xFFFF);

            int client_h = (int)window->h - (int)window->titlebar_height;
            int btn_x = (int)window->w - FC_PAD - FC_BUTTON_W;
            int btn_y = client_h - FC_PAD - FC_BUTTON_H + 4;

            if (click_x >= btn_x && click_x < btn_x + FC_BUTTON_W &&
                click_y >= btn_y && click_y < btn_y + FC_BUTTON_H) {
                fc_request_cancel(handle);
            }
            break;
        }

        case DWM_EVENT_KEYBOARD:
            if ((wparam & 0xFF) == FC_KEY_ESCAPE) fc_request_cancel(handle);
            break;

        default:
            break;
    }
}

// ==========================================
// Public API (DWM lock, then fc_mutex)
// ==========================================

static struct FileCopyItem* fc_new_item(const char* src, const char* dest) {
    if (src == NULL || dest == NULL || src[0] == '\0' || dest[0] == '\0') return NULL;
    if (strnlen(src,  FILECOPY_PATH_MAX) >= FILECOPY_PATH_MAX) return NULL;
    if (strnlen(dest, FILECOPY_PATH_MAX) >= FILECOPY_PATH_MAX) return NULL;

    struct FileCopyItem* item = (struct FileCopyItem*)malloc(sizeof(struct FileCopyItem));
    if (item == NULL) return NULL;

    memset(item, 0, sizeof(struct FileCopyItem));
    strncpy(item->src,  src,  FILECOPY_PATH_MAX - 1);
    strncpy(item->dest, dest, FILECOPY_PATH_MAX - 1);
    return item;
}

// fc_mutex held (or job not yet visible to the copy thread)
static void fc_append_item(struct FileCopyJob* job, struct FileCopyItem* item) {
    if (job->items_tail != NULL) job->items_tail->next = item;
    else                         job->items_head = item;
    job->items_tail = item;
    job->item_count++;
}

static bool fc_start_thread(void) {
    if (fc_thread != NULL) return true;

    mutex_init(&fc_mutex);
    fc_thread = thread_create(fc_thread_main, PRIORITY_NORMAL);
    if (fc_thread == NULL) return false;

    // Report-only: a slow disk can legitimately go a while between chunks
    wd_watch(fc_thread, "dwm-copyop", 10000, WD_NEVER);
    wd_register_lock("filecopy", &fc_mutex);
    return true;
}

WindowHandle dwm_summon_filecopy(const char* src, const char* dest, uint32_t flags) {
    DWM_GUARD();

    struct FileCopyJob* job = (struct FileCopyJob*)malloc(sizeof(struct FileCopyJob));
    if (job == NULL) return 0;

    memset(job, 0, sizeof(struct FileCopyJob));
    job->flags         = flags;
    job->held          = (flags & FILECOPY_HOLD) != 0;
    job->desktop_x     = -1;
    job->desktop_y     = -1;
    job->drawn_percent = -1;
    job->has_desktop   = dwm_desktop_get_directory(job->desktop_dir, sizeof(job->desktop_dir));

    struct FileCopyItem* item = fc_new_item(src, dest);
    if (item == NULL) {
        fc_free_job(job);
        return 0;
    }
    fc_append_item(job, item);
    strncpy(job->name, fc_basename(src), sizeof(job->name) - 1);

    // The copy thread is started the first time it is needed
    if (!fc_start_thread()) {
        fc_free_job(job);
        return 0;
    }

    WindowClass wclass;
    memset(&wclass, 0, sizeof(WindowClass));
    wclass.width      = FC_WIDTH;
    wclass.height     = FC_HEIGHT;
    wclass.max_width  = FC_WIDTH;
    wclass.max_height = FC_HEIGHT;

    // Centered above the taskbar, nudged down for each window already open
    uint32_t open_jobs = 0;
    fc_lock();
    for (struct FileCopyJob* j = fc_jobs; j != NULL; j = j->next) open_jobs++;
    fc_unlock();
    wclass.x = (uint16_t)((display_get_width() - FC_WIDTH) / 2 + (open_jobs % 5) * 20);
    wclass.y = (uint16_t)((display_get_height() - taskbar.height - FC_HEIGHT) / 2 + (open_jobs % 5) * 20);

    strncpy(wclass.title, (flags & FILECOPY_MOVE) ? "Moving files" : "Copying files", DWM_MAX_TITLE_LEN - 1);

    // No close box: Cancel is the way out, so a job always ends cleanly.
    // The window cannot be drawn before we return (we hold the DWM lock),
    // so publishing the job right after creating it is early enough.
    WindowHandle window = dwm_create_window(wclass, DWM_WSTYLE_NOCLOSEBOX, callback_filecopy_handler);
    if (window == 0) {
        fc_free_job(job);
        return 0;
    }

    // Publish only once job->window is set
    job->window = window;
    fc_lock();
    job->phase  = (fc_jobs != NULL) ? FC_PHASE_WAITING : FC_PHASE_COUNTING;
    struct FileCopyJob** tail = &fc_jobs;
    while (*tail != NULL) tail = &(*tail)->next;
    *tail = job;
    fc_unlock();

    if (!(flags & FILECOPY_HOLD))
        event_signal(&fc_event);
    return window;
}

bool dwm_filecopy_add(WindowHandle handle, const char* src, const char* dest) {
    DWM_GUARD();

    struct FileCopyItem* item = fc_new_item(src, dest);
    if (item == NULL) return false;

    fc_lock();
    struct FileCopyJob* job = fc_find(handle);
    if (job == NULL || job->finishing) {
        fc_unlock();
        free(item);
        return false;
    }
    fc_append_item(job, item);
    fc_unlock();

    // The copy thread counts the new item before (or when) it gets to it
    dwm_post_message(handle, DWM_EVENT_REDRAW, 0, 0);
    return true;
}

void dwm_filecopy_start(WindowHandle handle) {
    DWM_GUARD();

    fc_lock();
    struct FileCopyJob* job = fc_find(handle);
    if (job != NULL) job->held = false;
    fc_unlock();

    event_signal(&fc_event);
}

void dwm_filecopy_set_desktop_origin(WindowHandle handle, int x, int y) {
    DWM_GUARD();

    fc_lock();
    struct FileCopyJob* job = fc_find(handle);
    if (job != NULL) {
        job->desktop_x = x;
        job->desktop_y = y;
    }
    fc_unlock();
}

void dwm_filecopy_on_done(WindowHandle handle, FileCopyDoneCallback callback, void* user) {
    DWM_GUARD();

    fc_lock();
    struct FileCopyJob* job = fc_find(handle);
    if (job != NULL) {
        job->done_callback = callback;
        job->done_user     = user;
    }
    fc_unlock();
}

bool dwm_filecopy_busy(void) {
    // A single aligned pointer read; a stale answer is harmless
    return fc_jobs != NULL;
}
