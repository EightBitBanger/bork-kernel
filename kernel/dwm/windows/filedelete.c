#include <kernel/dwm/windows/filedelete.h>
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
//   DWM thread      creates jobs, draws the window, sets cancel.
//                   Holds the DWM lock, then takes fd_mutex briefly.
//   Delete thread   does the disk work. NEVER touches DWM state or takes the
//                   DWM lock. Everything visible is posted (dwm_post_message,
//                   dwm_invoke).
//
// fd_mutex rule: critical sections only read or write job fields. No calls
// out (no posting, no VFS, no string work beyond memcpy).
//
//   Written by the delete thread only:  phase, name, done, total
//   Written by the DWM thread:          cancel
//
// Lock order: DWM lock -> fd_mutex -> VFS lock. The delete thread never
// holds fd_mutex across a VFS call.

// ==========================================
// Layout (client coordinates, 6x8 font). Matches the copy window.
// ==========================================
#define FD_WIDTH           320
#define FD_HEIGHT          130     // Includes the 20px titlebar
#define FD_PAD             15
#define FD_STATUS_Y        12
#define FD_NAME_Y          26
#define FD_BAR_Y           46
#define FD_BAR_H           14
#define FD_PERCENT_Y       (FD_BAR_Y + FD_BAR_H + 8)
#define FD_BUTTON_W        64
#define FD_BUTTON_H        24
#define FD_FONT_W          6
#define FD_FONT_H          8
#define FD_KEY_ESCAPE      0x1B

#define FD_MAX_DEPTH       32      // Folder nesting the deleter will follow
#define FD_MESSAGE_MAX     160

// Posting to the DWM when its queues are full: retry this many times, 5 ms apart
#define FD_POST_RETRIES    400

typedef enum {
    FD_PHASE_WAITING,              // Queued behind another window
    FD_PHASE_PREPARING,            // Counting the tree (nothing deleted yet)
    FD_PHASE_DELETING,
} FileDeletePhase;

struct FileDeleteJob {
    // ---- Shared: written under fd_mutex ----
    struct FileDeleteJob* next;

    FileDeletePhase phase;
    char            name[VFS_NAME_MAX + 1];
    uint32_t        done;               // Items processed (deleted or failed)
    uint32_t        total;              // Items in the tree, root included

    volatile bool   cancel;

    // ---- Fixed before the job is published ----
    WindowHandle    window;
    char            path[FILEDELETE_PATH_MAX];

    // ---- Delete thread only ----
    int             drawn_percent;      // Last percent successfully posted
    uint32_t        deleted;
    uint32_t        failed;
    bool            cancelled;
    bool            refused;            // Stopped in Preparing, nothing touched
    const char*     last_error;
};

// What the DWM needs to draw a window, copied out under fd_mutex
typedef struct {
    FileDeletePhase phase;
    char            name[VFS_NAME_MAX + 1];
    uint32_t        done;
    uint32_t        total;
    bool            cancel;
} FdView;

static mutex_t               fd_mutex;
static ThreadBlock*          fd_thread = NULL;
static Event                 fd_event  = EVENT_INITIALIZER;
static struct FileDeleteJob* fd_jobs   = NULL;    // FIFO, head is running

static void fd_lock(void)   { mutex_lock(&fd_mutex); }
static void fd_unlock(void) { mutex_unlock(&fd_mutex); }

// ==========================================
// Small helpers
// ==========================================

static size_t fd_u32_to_str(uint32_t value, char* out) {
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

static const char* fd_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

static bool fd_join(char* out, size_t size, const char* dir, const char* name) {
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
static bool fd_path_is_within(const char* path, const char* base) {
    size_t base_len = strlen(base);
    while (base_len > 1 && base[base_len - 1] == '/') base_len--;

    if (strncmp(path, base, base_len) != 0) return false;
    return path[base_len] == '\0' || path[base_len] == '/';
}

static bool fd_skip_name(const char* name) {
    return name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

// Percent in 32-bit arithmetic only (no __udivdi3 on i386).
// Preparing has no bar, so it reports 0.
static int fd_percent_of(FileDeletePhase phase, uint32_t done, uint32_t total) {
    if (phase != FD_PHASE_DELETING) return 0;
    if (total == 0 || done >= total) return 100;

    uint32_t percent;
    if (total <= 0xFFFFFFFFu / 100u)
        percent = (done * 100u) / total;
    else
        percent = done / (total / 100u);

    return (percent > 99) ? 99 : (int)percent;
}

// fd_mutex held
static struct FileDeleteJob* fd_find(WindowHandle handle) {
    for (struct FileDeleteJob* job = fd_jobs; job != NULL; job = job->next) {
        if (job->window == handle) return job;
    }
    return NULL;
}

// ==========================================
// Work posted to the DWM thread
// ==========================================

struct FdBoxMsg {
    char title[16];
    char message[FD_MESSAGE_MAX];
};

static void fd_ui_message_box(void* arg) {
    struct FdBoxMsg* m = (struct FdBoxMsg*)arg;
    dwm_notify(m->title, m->message, DWM_NOTIFY_LONG_MS);
    free(m);
}

// Drop desktop icons for anything under the deleted path that is gone (a
// partial delete can leave the root and some children behind), then let
// every window re-read its folder. DWM thread, DWM lock held; the VFS lock
// comes after the DWM lock, so the existence checks are fine here.
static void fd_ui_cleanup(void* arg) {
    const char* root = (const char*)arg;

    struct list_node* node = workspace.icon_head;
    while (node != NULL) {
        struct list_node* next = node->next;   // dwm_destroy_icon frees `node`
        struct IconObject* icon = (struct IconObject*)node->data;

        if (icon != NULL && fd_path_is_within(icon->path, root) && !vfs_exists(icon->path))
            dwm_destroy_icon(icon);

        node = next;
    }

    dwm_refresh_all_windows();
    free(arg);
}

// Delete thread: queue fn(arg) on the DWM thread, retrying while the queue
// is full instead of silently dropping it
static void fd_post(DWMInvokeFn fn, void* arg) {
    for (int i = 0; i < FD_POST_RETRIES; i++) {
        if (dwm_invoke(fn, arg)) return;
        wd_pet();
        thread_sleep(5);
    }
    if (arg != NULL) free(arg);
}

// ==========================================
// Window updates (delete thread, fd_mutex NOT held)
// ==========================================

// Post a repaint when the percent moved (or always, with force). The
// percent is only recorded once the post succeeded, so a dropped REDRAW is
// retried on the next update.
static void fd_redraw(struct FileDeleteJob* job, bool force) {
    int percent = fd_percent_of(job->phase, job->done, job->total);
    if (!force && percent == job->drawn_percent) return;

    if (job->window != 0 && dwm_post_message(job->window, DWM_EVENT_REDRAW, 0, 0))
        job->drawn_percent = percent;
    else
        job->drawn_percent = -1;
}

// New phase: always repaint
static void fd_set_phase(struct FileDeleteJob* job, FileDeletePhase phase) {
    wd_pet();

    fd_lock();
    job->phase = phase;
    job->done  = 0;
    fd_unlock();

    fd_redraw(job, true);
}

// One more item processed. The name is updated every time but only drawn
// when the percent moves, so a big folder doesn't flood the DWM.
static void fd_item_done(struct FileDeleteJob* job, const char* path) {
    wd_pet();

    char name[VFS_NAME_MAX + 1];
    memset(name, '\0', sizeof(name));
    strncpy(name, fd_basename(path), sizeof(name) - 1);

    fd_lock();
    job->done++;
    memcpy(job->name, name, sizeof(job->name));
    fd_unlock();

    fd_redraw(job, false);
}

// ==========================================
// Deleting (delete thread, no locks held on entry or exit)
// ==========================================

// Preparing: count every item and check the whole tree can be deleted
// before anything is touched. Returns false (with last_error set, unless
// cancelled) when it cannot.
static bool fd_count(struct FileDeleteJob* job, const char* path, uint32_t depth, uint32_t* count) {
    if (job->cancel) return false;
    wd_pet();

    // Never descend into another device mounted somewhere inside the tree
    if (vfs_directory_check_mounted(path)) {
        job->last_error = "storage devices cannot be deleted";
        return false;
    }

    (*count)++;
    if (!vfs_directory_check(path)) return true;

    if (depth >= FD_MAX_DEPTH) {
        job->last_error = "folders are nested too deeply";
        return false;
    }

    // On the heap: the delete thread's stack is small and this recurses
    char* child = (char*)malloc(FILEDELETE_PATH_MAX);
    if (child == NULL) {
        job->last_error = "out of memory";
        return false;
    }

    bool ok = true;
    uint32_t items = vfs_directory_get_item_count(path);

    for (uint32_t i = 0; i < items && ok; i++) {
        char name[VFS_NAME_MAX + 1];
        memset(name, '\0', sizeof(name));
        if (!vfs_directory_get_item(path, i, name, sizeof(name))) continue;
        if (fd_skip_name(name)) continue;

        if (!fd_join(child, FILEDELETE_PATH_MAX, path, name)) {
            job->last_error = "a path was too long";
            ok = false;
            break;
        }
        ok = fd_count(job, child, depth + 1, count);
    }

    free(child);
    return ok;
}

// Deleting: children first, then the item itself. Failures are counted and
// skipped. Returns false only when the job was cancelled.
static bool fd_delete_any(struct FileDeleteJob* job, const char* path, uint32_t depth) {
    if (job->cancel) return false;

    bool is_dir = vfs_directory_check(path);

    if (is_dir && depth < FD_MAX_DEPTH) {
        char* child = (char*)malloc(FILEDELETE_PATH_MAX);
        if (child == NULL) {
            job->last_error = "out of memory";
            job->failed++;
            fd_item_done(job, path);
            return true;
        }

        // Deleting shifts the listing, so always look at index i again: it
        // only moves on past items that could not be removed (or skipped
        // names). Every pass either removes an item or advances i, so this
        // always ends.
        uint32_t i = 0;
        while (1) {
            if (job->cancel) {
                free(child);
                return false;
            }
            if (i >= vfs_directory_get_item_count(path)) break;

            char name[VFS_NAME_MAX + 1];
            memset(name, '\0', sizeof(name));
            if (!vfs_directory_get_item(path, i, name, sizeof(name)) || fd_skip_name(name) ||
                !fd_join(child, FILEDELETE_PATH_MAX, path, name)) {
                i++;
                continue;
            }

            if (!fd_delete_any(job, child, depth + 1)) {
                free(child);
                return false;
            }

            if (vfs_exists(child)) i++;
        }

        free(child);
    }

    if (vfs_remove(path) && !vfs_exists(path)) {
        job->deleted++;
    } else {
        job->failed++;
        job->last_error = is_dir ? "a folder could not be removed" : "a file could not be deleted";
    }
    fd_item_done(job, path);
    return true;
}

static void fd_run_job(struct FileDeleteJob* job) {
    if (!vfs_exists(job->path)) {
        job->last_error = "the item no longer exists";
        job->failed = 1;
        job->refused = true;
        return;
    }

    // ---- Preparing ----
    uint32_t total = 0;
    if (!fd_count(job, job->path, 0, &total)) {
        if (job->cancel) {
            job->cancelled = true;
        } else {
            job->failed = 1;
            job->refused = true;
        }
        return;
    }

    fd_lock();
    job->total = total;
    fd_unlock();

    // ---- Deleting ----
    fd_set_phase(job, FD_PHASE_DELETING);
    if (!fd_delete_any(job, job->path, 0) || job->cancel)
        job->cancelled = true;
}

static void fd_report(struct FileDeleteJob* job) {
    struct FdBoxMsg* m = NULL;

    if (job->failed > 0 && job->last_error != NULL) {
        m = (struct FdBoxMsg*)malloc(sizeof(struct FdBoxMsg));
        if (m == NULL) return;
        memset(m, 0, sizeof(*m));

        char* message = m->message;
        if (job->refused) {
            strncpy(message, "Nothing was deleted: ", FD_MESSAGE_MAX - 1);
        } else {
            size_t n = fd_u32_to_str(job->failed, message);
            message[n] = '\0';
            strncat(message, (job->failed == 1) ? " item" : " items", FD_MESSAGE_MAX - strlen(message) - 1);
            strncat(message, " could not be deleted: ", FD_MESSAGE_MAX - strlen(message) - 1);
        }
        strncat(message, job->last_error, FD_MESSAGE_MAX - strlen(message) - 1);
    } else if (job->cancelled && job->deleted > 0) {
        // Deletion can't be undone: say what is already gone
        m = (struct FdBoxMsg*)malloc(sizeof(struct FdBoxMsg));
        if (m == NULL) return;
        memset(m, 0, sizeof(*m));

        char* message = m->message;
        strncpy(message, "Cancelled. ", FD_MESSAGE_MAX - 1);
        size_t len = strlen(message);
        fd_u32_to_str(job->deleted, &message[len]);
        strncat(message, (job->deleted == 1) ? " item was" : " items were", FD_MESSAGE_MAX - strlen(message) - 1);
        strncat(message, " already deleted", FD_MESSAGE_MAX - strlen(message) - 1);
    }

    if (m == NULL) return;
    strncpy(m->title, "Delete", sizeof(m->title) - 1);
    fd_post(fd_ui_message_box, m);
}

static void fd_post_cleanup(struct FileDeleteJob* job) {
    size_t len = strnlen(job->path, FILEDELETE_PATH_MAX - 1);
    char* copy = (char*)malloc(len + 1);
    if (copy == NULL) return;
    memcpy(copy, job->path, len);
    copy[len] = '\0';
    fd_post(fd_ui_cleanup, copy);
}

static void fd_thread_main(void) {
    while (1) {
        event_wait(&fd_event, 0);           // 0 = until signaled
        wd_pet();

        while (1) {
            fd_lock();
            struct FileDeleteJob* job = fd_jobs;
            fd_unlock();

            if (job == NULL) break;

            fd_run_job(job);

            // Icons and explorer windows catch up even after a partial delete
            if (!job->refused) fd_post_cleanup(job);
            fd_report(job);

            // Off the list first: from here on the window's handler finds
            // nothing and ignores whatever events are still queued for it
            fd_lock();
            fd_jobs = job->next;
            struct FileDeleteJob* next_job = fd_jobs;
            if (next_job != NULL) next_job->phase = FD_PHASE_PREPARING;
            fd_unlock();

            // Only this thread frees jobs, so next_job stays valid here
            if (next_job != NULL) fd_redraw(next_job, true);

            // The window has no close box: never give up on closing it
            if (job->window != 0) {
                while (!dwm_post_message(job->window, DWM_EVENT_CLOSE, 0, 0)) {
                    wd_pet();
                    thread_sleep(5);
                }
            }
            free(job);
        }
    }
}

// ==========================================
// Drawing (DWM thread, fd_mutex NOT held)
// ==========================================

static bool fd_snapshot(WindowHandle handle, FdView* view) {
    bool found = false;

    fd_lock();
    struct FileDeleteJob* job = fd_find(handle);
    if (job != NULL) {
        view->phase  = job->phase;
        view->done   = job->done;
        view->total  = job->total;
        view->cancel = job->cancel;
        memcpy(view->name, job->name, sizeof(view->name));
        found = true;
    }
    fd_unlock();

    view->name[sizeof(view->name) - 1] = '\0';
    return found;
}

static void fd_button_rect(struct WindowObject* window, int* x, int* y) {
    int client_h = (int)window->h - (int)window->titlebar_height;
    *x = (int)window->w - FD_PAD - FD_BUTTON_W;
    *y = client_h - FD_PAD - FD_BUTTON_H + 4;
}

static void fd_draw(struct WindowObject* window, const FdView* v) {
    dwm_draw_rect_filled(0, 0, window->w, window->h, theme.client.background_danger);

    // Status line
    const char* status = "Deleting";
    if (v->phase == FD_PHASE_WAITING)   status = "Waiting for another delete";
    if (v->phase == FD_PHASE_PREPARING) status = "Preparing";
    dwm_draw_text(FD_PAD, FD_STATUS_Y, status, theme.client.text);
    dwm_draw_text(FD_PAD, FD_NAME_Y, v->name, theme.client.text_danger);

    // Bar (none while preparing or waiting)
    int percent = fd_percent_of(v->phase, v->done, v->total);
    if (v->phase == FD_PHASE_DELETING) {
        int bar_w = (int)window->w - (FD_PAD * 2);

        dwm_draw_rect_filled(FD_PAD, FD_BAR_Y, bar_w, FD_BAR_H, theme.edit.background);
        if (percent > 0) {
            dwm_draw_rect_filled(FD_PAD, FD_BAR_Y, (bar_w * percent) / 100, FD_BAR_H, theme.client.text_danger);
        }
        dwm_draw_rect(FD_PAD - 1, FD_BAR_Y - 1, bar_w + 2, FD_BAR_H + 2, theme.edit.border);
    }

    // Percent, or the cancel acknowledgement
    if (v->cancel) {
        dwm_draw_text(FD_PAD, FD_PERCENT_Y, "Cancelling...", theme.client.text_muted);
    } else if (v->phase == FD_PHASE_DELETING) {
        char pct[8];
        size_t n = fd_u32_to_str((uint32_t)percent, pct);
        pct[n]     = '%';
        pct[n + 1] = '\0';
        dwm_draw_text(FD_PAD, FD_PERCENT_Y, pct, theme.client.text_muted);
    }

    // Cancel button (bottom right of the client area)
    int btn_x, btn_y;
    fd_button_rect(window, &btn_x, &btn_y);

    const char* label = "Cancel";
    int label_w = (int)strlen(label) * FD_FONT_W;

    dwm_draw_rect_filled(btn_x, btn_y, FD_BUTTON_W, FD_BUTTON_H, theme.button.fill);
    dwm_draw_rect(btn_x, btn_y, FD_BUTTON_W, FD_BUTTON_H, theme.button.border);
    dwm_draw_text(btn_x + (FD_BUTTON_W - label_w) / 2, btn_y + (FD_BUTTON_H - FD_FONT_H) / 2,
                  label, v->cancel ? theme.client.text_muted : theme.button.text);
}

// ==========================================
// Event handler (DWM thread, DWM lock held)
// ==========================================

static void fd_request_cancel(WindowHandle handle) {
    bool changed = false;

    fd_lock();
    struct FileDeleteJob* job = fd_find(handle);
    if (job != NULL && !job->cancel) {
        job->cancel = true;
        changed = true;
    }
    fd_unlock();

    if (changed) dwm_post_message(handle, DWM_EVENT_REDRAW, 0, 0);
}

void callback_filedelete_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    struct WindowObject* window = dwm_get_window_by_id(handle);
    if (window == NULL) return;

    switch (event) {
        case DWM_EVENT_REDRAW: {
            FdView view;
            if (fd_snapshot(handle, &view)) fd_draw(window, &view);
            break;
        }

        case DWM_EVENT_MOUSE: {
            if (!(lparam & DWM_STATE_MOUSE_BTN_LEFT)) break;

            int click_x = (int16_t)(wparam & 0xFFFF);
            int click_y = (int16_t)((wparam >> 16) & 0xFFFF);

            int btn_x, btn_y;
            fd_button_rect(window, &btn_x, &btn_y);

            if (click_x >= btn_x && click_x < btn_x + FD_BUTTON_W &&
                click_y >= btn_y && click_y < btn_y + FD_BUTTON_H) {
                fd_request_cancel(handle);
            }
            break;
        }

        case DWM_EVENT_KEYBOARD:
            if ((wparam & 0xFF) == FD_KEY_ESCAPE) fd_request_cancel(handle);
            break;

        default:
            break;
    }
}

// ==========================================
// Public API (DWM lock, then fd_mutex)
// ==========================================

static bool fd_start_thread(void) {
    if (fd_thread != NULL) return true;

    mutex_init(&fd_mutex);
    fd_thread = thread_create(fd_thread_main, PRIORITY_NORMAL);
    if (fd_thread == NULL) return false;

    // Report-only: a slow disk can legitimately go a while between items
    wd_watch(fd_thread, "dwm-deleteop", 10000, WD_NEVER);
    wd_register_lock("filedelete", &fd_mutex);
    return true;
}

WindowHandle dwm_summon_filedelete(const char* path) {
    DWM_GUARD();

    if (path == NULL || path[0] == '\0') return 0;
    if (strnlen(path, FILEDELETE_PATH_MAX) >= FILEDELETE_PATH_MAX) return 0;

    // Refused up front: no window for something that can never be deleted
    if (vfs_directory_check_mounted(path)) {
        dwm_notify("Delete", "Storage devices cannot be deleted", DWM_NOTIFY_DEFAULT_MS);
        return 0;
    }

    struct FileDeleteJob* job = (struct FileDeleteJob*)malloc(sizeof(struct FileDeleteJob));
    if (job == NULL) return 0;

    memset(job, 0, sizeof(struct FileDeleteJob));
    job->drawn_percent = -1;
    strncpy(job->path, path, FILEDELETE_PATH_MAX - 1);
    strncpy(job->name, fd_basename(path), sizeof(job->name) - 1);

    // The delete thread is started the first time it is needed
    if (!fd_start_thread()) {
        free(job);
        return 0;
    }

    WindowClass wclass;
    memset(&wclass, 0, sizeof(WindowClass));
    wclass.width      = FD_WIDTH;
    wclass.height     = FD_HEIGHT;
    wclass.max_width  = FD_WIDTH;
    wclass.max_height = FD_HEIGHT;

    // Centered above the taskbar, nudged down for each window already open
    uint32_t open_jobs = 0;
    fd_lock();
    for (struct FileDeleteJob* j = fd_jobs; j != NULL; j = j->next) open_jobs++;
    fd_unlock();
    wclass.x = (uint16_t)((display_get_width() - FD_WIDTH) / 2 + (open_jobs % 5) * 20);
    wclass.y = (uint16_t)((display_get_height() - taskbar.height - FD_HEIGHT) / 2 + (open_jobs % 5) * 20);

    strncpy(wclass.title, "Deleting files", DWM_MAX_TITLE_LEN - 1);

    // No close box: Cancel is the way out, so a job always ends cleanly.
    // The window cannot be drawn before we return (we hold the DWM lock),
    // so publishing the job right after creating it is early enough.
    WindowHandle window = dwm_create_window(wclass, DWM_WSTYLE_NOCLOSEBOX, callback_filedelete_handler);
    if (window == 0) {
        free(job);
        return 0;
    }

    // Publish only once job->window is set
    job->window = window;
    fd_lock();
    job->phase = (fd_jobs != NULL) ? FD_PHASE_WAITING : FD_PHASE_PREPARING;
    struct FileDeleteJob** tail = &fd_jobs;
    while (*tail != NULL) tail = &(*tail)->next;
    *tail = job;
    fd_unlock();

    event_signal(&fd_event);
    return window;
}

bool dwm_filedelete_busy(void) {
    // A single aligned pointer read; a stale answer is harmless
    return fd_jobs != NULL;
}
