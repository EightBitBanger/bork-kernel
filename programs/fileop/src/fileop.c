/*
 * fileop: copy, move and delete files and folders, with a progress window.
 *
 *     fileop copy   [-r] <src> <dst> [<src> <dst> ...]
 *     fileop move   [-r] <src> <dst> [<src> <dst> ...]
 *     fileop delete [-i] <path> [<path> ...]
 *     fileop paste  <dir>
 *
 * Installed as <home>/sys/fileop. The kernel DWM's copy and delete windows,
 * as one program:
 *
 *   copy / move   <dst> is the full path each <src> should end up at (a
 *                 folder goes with everything in it). With -r a taken name
 *                 becomes "name (1)" and so on instead of failing. A move
 *                 on one volume relinks the item (os_move): nothing is
 *                 copied, so it is instant whatever the size, and no window
 *                 opens unless something is left to copy. Across volumes it
 *                 is a copy, then the original is deleted once its copy is
 *                 good.
 *   delete        each path, folders with everything in them. Storage
 *                 devices are refused. With -i it asks first ("Delete ...?
 *                 This can't be undone.", Delete / Cancel) and deletes
 *                 nothing unless the answer is Delete, or when it can't ask.
 *   paste         the clipboard's files (os/wm.h) into <dir>: moved after a
 *                 cut (and the clipboard cleared once they all arrived),
 *                 copied otherwise; taken names get "name (1)".
 *
 * Copying: the job is counted first, so one bar runs 0-100% for all of it.
 * Space for each file is reserved before writing (in steps, so Cancel
 * works), the size is checked, and the copy is read back and compared with
 * the original (FILEOP_VERIFY). A copy that fails is removed, never left
 * looking complete.
 *
 * Deleting: the tree is walked and counted first; a tree holding a device,
 * nested too deeply or with paths too long is refused untouched. Items then
 * go deepest first. Deletion can't be undone: Cancel stops before the next
 * item and says how much was already gone.
 *
 * Cancel (or Escape) stops the job; the window has no close box, so a job
 * always ends cleanly. Problems are reported as notifications. Explorer
 * windows and the desktop hear about every change (wm_fs_changed).
 *
 * Exit code: 0 done, 1 something failed, 2 bad arguments, 3 cancelled.
 *
 * Needs SEC_FILE_READ and SEC_FILE_WRITE, SEC_GFX and SEC_IPC.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/security.h>
#include <os/syscall.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

// Read the copy back and compare it with the original (doubles the reading)
#ifndef FILEOP_VERIFY
#define FILEOP_VERIFY       1
#endif

#define CHUNK               32768               // Bytes per read / write
#define RESERVE_STEP        (256u * 1024u)      // Space is reserved in steps this big
#define COPY_MAX_DEPTH      16
#define DELETE_MAX_DEPTH    32
#define MAX_ATTEMPTS        100                 // The name, "name (1)" ... "name (99)"
#define PUMP_MS             15                  // Look at the window this often
#define DRAW_MS             100                 // Repaint at most this often (unless the percent moved)
#define ANNOUNCE_MS         1000                // Tell explorers about changes at most this often

#if FILEOP_VERIFY
#define PASSES              2u
#else
#define PASSES              1u
#endif

// Layout (client coordinates; the kernel windows' values)
#define WIN_W               320
#define WIN_H               110
#define PAD                 15
#define STATUS_Y            12
#define NAME_Y              26
#define BAR_Y               46
#define BAR_H               14
#define PERCENT_Y           (BAR_Y + BAR_H + 8)
#define BUTTON_W            64
#define BUTTON_H            24

typedef enum { OP_COPY, OP_MOVE, OP_DELETE } op_t;

typedef enum {
    PH_COUNTING,            // Sizing the job (no bar yet)
    PH_PREPARING,           // Reserving space for a file / checking a tree
    PH_COPYING,
    PH_VERIFYING,
    PH_DELETING,
} phase_t;

typedef struct {
    char     src[PATH_MAX];
    char     dst[PATH_MAX];     // Unused for delete
    uint64_t weight;            // Work units for the whole item
    bool     settled;           // Move: relinked (or failed) already, nothing to copy
} item_t;

static op_t     op;
static bool     rename_taken;   // -r
static bool     ask_first;      // delete -i
static bool     confirming;     // The window shows the question, not progress
static item_t*  items;
static size_t   item_count;

// Progress, as the window shows it
static phase_t  phase = PH_COUNTING;
static char     shown_name[NAME_MAX + 1];
static uint64_t done, total;
static uint32_t file_index, file_total;
static bool     cancel;

// Results
static uint32_t    ok_count, failed, skipped;
static bool        cancelled, refused;
static const char* last_error;

// Window
static bool        ui;
static wm_window_t win;
static wm_theme_t  theme;
static int32_t     cw = 6, lh = 8;
static uint64_t    last_pump, last_draw, last_announce;
static int         drawn_percent = -1;
static phase_t     drawn_phase;
static char        announce_dir[PATH_MAX];

static uint8_t* buffer;
static uint8_t* verify_buffer;

//
// Small helpers
//

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

static void parent_of(const char* path, char* out, size_t size) {
    strlcpy(out, path, size);
    char* slash = strrchr(out, '/');
    if (slash == NULL)      strlcpy(out, "/", size);
    else if (slash == out)  out[1] = '\0';
    else                    *slash = '\0';
}

static bool join(char* out, size_t size, const char* dir, const char* name) {
    int n = snprintf(out, size, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
    return n >= 0 && (size_t)n < size;
}

// `path` is `base` itself or somewhere below it
static bool within(const char* path, const char* base) {
    size_t n = strlen(base);
    while (n > 1 && base[n - 1] == '/') n--;
    return strncmp(path, base, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static bool skip_name(const char* name) {
    return name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

// SYS_STAT directly: the libc's stat() folds device roots into directories.
// SYS_TYPE_*, or -1.
static int kind_of(const char* path, SysStat* st) {
    SysStat local;
    int32_t r = syscall2(SYS_STAT, (uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)(st ? st : &local));
    return SYS_IS_ERROR(r) ? -1 : (int)(st ? st : &local)->type;
}

static bool exists(const char* path) {
    return kind_of(path, NULL) >= 0;
}

// Entry `index` of `dir` into name. 1: got one, 0: past the end, -1: that
// entry can't be read (skip it)
static int entry_at(const char* dir, uint32_t index, char* name, size_t size) {
    int32_t r = syscall4(SYS_READDIR, (uint32_t)(uintptr_t)dir, index, (uint32_t)(uintptr_t)name, (uint32_t)size);
    if (r == 0) return 0;
    return SYS_IS_ERROR(r) ? -1 : 1;
}

// Name for attempt n: n == 1 is the name itself, n >= 2 "stem (n - 1).ext"
// (as the explorer and the desktop number them)
static void candidate_name(const char* name, uint32_t n, char out[NAME_MAX + 1]) {
    size_t len = strnlen(name, NAME_MAX);
    if (n <= 1) {
        memcpy(out, name, len);
        out[len] = '\0';
        return;
    }
    char suffix[8];
    size_t suffix_len = (size_t)snprintf(suffix, sizeof(suffix), " (%u)", (unsigned)(n - 1));

    const char* dot = strrchr(name, '.');
    size_t ext_len = (dot != NULL && dot != name) ? strnlen(dot, NAME_MAX) : 0;
    if (ext_len > 5 || ext_len >= len) ext_len = 0;

    size_t stem_len = len - ext_len;
    size_t room = NAME_MAX - suffix_len - ext_len;
    if (stem_len > room) stem_len = room;
    while (stem_len > 1 && name[stem_len - 1] == ' ') stem_len--;

    size_t o = 0;
    memcpy(out + o, name, stem_len);              o += stem_len;
    memcpy(out + o, suffix, suffix_len);          o += suffix_len;
    memcpy(out + o, name + len - ext_len, ext_len); o += ext_len;
    out[o] = '\0';
}

// A free path in `dir` for `name` ("name (1)" ...). False when none.
static bool unique_path(const char* dir, const char* name, char* out, size_t size) {
    char candidate[NAME_MAX + 1];
    for (uint32_t n = 1; n <= MAX_ATTEMPTS; n++) {
        candidate_name(name, n, candidate);
        if (!join(out, size, dir, candidate))
            return false;
        if (!exists(out))
            return true;
    }
    return false;
}

//
// The window
//

static int percent(void) {
    if (phase == PH_COUNTING || (op == OP_DELETE && phase != PH_DELETING))
        return 0;
    if (total == 0 || done >= total)
        return 100;
    uint64_t p = done * 100 / total;
    return p > 99 ? 99 : (int)p;
}

static bool has_bar(void) {
    return op == OP_DELETE ? phase == PH_DELETING : phase != PH_COUNTING;
}

static gfx_rect_t button_rect(void) {
    return gfx_rect(WIN_W - PAD - BUTTON_W, WIN_H - PAD - BUTTON_H + 4, BUTTON_W, BUTTON_H);
}

// The question's Delete button, left of Cancel
static gfx_rect_t delete_button_rect(void) {
    gfx_rect_t r = button_rect();
    r.x -= BUTTON_W + 8;
    return r;
}

static bool in_rect(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void draw_button(gfx_surface_t* s, gfx_rect_t b, const char* label, uint32_t color) {
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, theme.button.fill);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, theme.button.border);
    gfx_draw_text(s, b.x + (b.w - (int32_t)strlen(label) * cw) / 2, gfx_text_y(s, b.y, b.h), label, color);
}

// delete -i: "Delete ...?" before anything is touched
static void draw_question(gfx_surface_t* s) {
    char line[NAME_MAX + 32];
    size_t cols = (size_t)((WIN_W - PAD * 2) / cw);
    if (item_count == 1) {
        const char* name = base_name(items[0].src);
        if (strlen(name) + 10 > cols && cols > 13)
            snprintf(line, sizeof(line), "Delete \"%.*s...\"?", (int)(cols - 13), name);
        else
            snprintf(line, sizeof(line), "Delete \"%s\"?", name);
    } else {
        snprintf(line, sizeof(line), "Delete these %u items?", (unsigned)item_count);
    }
    gfx_draw_text(s, PAD, STATUS_Y, line, theme.client.text);

    bool folder = false;
    for (size_t i = 0; i < item_count && !folder; i++)
        folder = kind_of(items[i].src, NULL) == SYS_TYPE_DIRECTORY;
    int32_t y = NAME_Y + 4;
    if (folder) {
        gfx_draw_text(s, PAD, y, item_count == 1 ? "The folder and everything in it" : "Folders go with everything in them",
                      theme.client.text_soft);
        y += lh + 4;
    }
    gfx_draw_text(s, PAD, y, "This can't be undone.", theme.client.text_danger);

    draw_button(s, delete_button_rect(), "Delete", theme.client.text_danger);
    draw_button(s, button_rect(), "Cancel", theme.button.text);
}

static void draw(void) {
    if (!ui)
        return;
    gfx_surface_t* s = wm_window_surface(win);
    if (s == NULL)
        return;
    cw = gfx_char_width(s);
    lh = gfx_line_height(s);

    bool del = (op == OP_DELETE);
    gfx_clear(s, del ? theme.client.background_danger : theme.client.background);
    if (confirming) {
        draw_question(s);
        wm_window_commit(win, NULL);
        return;
    }

    char status[64];
    switch (phase) {
        case PH_COUNTING:  strlcpy(status, del ? "Preparing" : "Counting items", sizeof(status)); break;
        case PH_PREPARING: strlcpy(status, del ? "Preparing" : (op == OP_MOVE ? "Moving" : "Copying"), sizeof(status)); break;
        case PH_COPYING:
        case PH_VERIFYING: strlcpy(status, op == OP_MOVE ? "Moving" : "Copying", sizeof(status)); break;
        case PH_DELETING:  strlcpy(status, "Deleting", sizeof(status)); break;
    }
    if (!del && has_bar() && file_total > 1 && file_index > 0 && file_index <= file_total) {
        size_t n = strlen(status);
        snprintf(status + n, sizeof(status) - n, " %u of %u", (unsigned)file_index, (unsigned)file_total);
    }
    gfx_draw_text(s, PAD, STATUS_Y, status, theme.client.text);

    // The name, cut to the window
    char name[NAME_MAX + 1];
    size_t cols = (size_t)((WIN_W - PAD * 2) / cw);
    if (strlen(shown_name) > cols && cols > 3)
        snprintf(name, sizeof(name), "%.*s...", (int)(cols - 3), shown_name);
    else
        strlcpy(name, shown_name, sizeof(name));
    gfx_draw_text(s, PAD, NAME_Y, name, del ? theme.client.text_danger : theme.client.text_value);

    int pct = percent();
    if (has_bar()) {
        int32_t bar_w = WIN_W - PAD * 2;
        gfx_fill_rect(s, PAD, BAR_Y, bar_w, BAR_H, theme.edit.background);
        if (pct > 0)
            gfx_fill_rect(s, PAD, BAR_Y, bar_w * pct / 100, BAR_H, del ? theme.client.text_danger : theme.client.accent);
        gfx_draw_rect(s, PAD - 1, BAR_Y - 1, bar_w + 2, BAR_H + 2, theme.edit.border);
    }

    if (cancel) {
        gfx_draw_text(s, PAD, PERCENT_Y, "Cancelling...", theme.client.text_muted);
    } else if (has_bar()) {
        char p[8];
        snprintf(p, sizeof(p), "%d%%", pct);
        gfx_draw_text(s, PAD, PERCENT_Y, p, theme.client.text_muted);
    }

    gfx_rect_t b = button_rect();
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, theme.button.fill);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, theme.button.border);
    gfx_draw_text(s, b.x + (b.w - 6 * cw) / 2, gfx_text_y(s, b.y, b.h), "Cancel",
                  cancel ? theme.client.text_muted : theme.button.text);

    wm_window_commit(win, NULL);
    drawn_percent = pct;
    drawn_phase = phase;
    last_draw = os_time_ms();
}

static void request_cancel(void) {
    if (!cancel) {
        cancel = true;
        draw();
    }
}

static void announce(const char* dir, bool now);

// Look at the window (Cancel, repaints) and keep explorers current. Called
// all through the disk loops; cheap when nothing is due.
static void pump(bool force) {
    uint64_t t = os_time_ms();
    if (!force && t - last_pump < PUMP_MS)
        return;
    last_pump = t;

    if (ui) {
        wm_event_t ev;
        int r;
        while ((r = wm_wait_event(&ev, 0)) > 0) {
            if (ev.type == WM_EVENT_KEY && ev.key == KEY_ESCAPE)
                request_cancel();
            else if (ev.type == WM_EVENT_MOUSE_DOWN && ev.button == INPUT_BTN_LEFT) {
                if (in_rect(button_rect(), ev.x, ev.y))
                    request_cancel();
            } else if (ev.type == WM_EVENT_THEME_CHANGED && wm_get_theme(&theme) == 0) {
                force = true;
            }
        }
        if (r < 0)
            ui = false;     // The dwm went away: finish the job without a window
    }

    if (ui && (force || percent() != drawn_percent || phase != drawn_phase || t - last_draw >= DRAW_MS))
        draw();
    announce(NULL, false);
}

static void set_stage(phase_t p, const char* path) {
    phase = p;
    if (path != NULL)
        strlcpy(shown_name, base_name(path), sizeof(shown_name));
    pump(true);
}

static void add_done(uint64_t units) {
    done += units;
    pump(false);
}

// Explorers and the desktop re-read their folders: `dir` changed (NULL:
// just flush a pending one when it is due)
static void announce(const char* dir, bool now) {
    if (dir != NULL && announce_dir[0] == '\0')
        strlcpy(announce_dir, dir, sizeof(announce_dir));
    else if (dir != NULL && strcmp(dir, announce_dir) != 0)
        strlcpy(announce_dir, "/", sizeof(announce_dir));       // Several places: say "everywhere"
    if (announce_dir[0] == '\0')
        return;
    uint64_t t = os_time_ms();
    if (!now && t - last_announce < ANNOUNCE_MS)
        return;
    wm_fs_changed(announce_dir);
    announce_dir[0] = '\0';
    last_announce = t;
}

//
// Deleting (also the second half of a move, and the cleanup of a failed copy)
//

// Count every item and check the whole tree can go before touching it
static bool delete_count(const char* path, uint32_t depth, uint32_t* count) {
    if (cancel) return false;
    pump(false);

    int kind = kind_of(path, NULL);
    if (kind == SYS_TYPE_MOUNT) {
        last_error = "storage devices can't be deleted";
        return false;
    }
    (*count)++;
    if (kind != SYS_TYPE_DIRECTORY)
        return true;
    if (depth >= DELETE_MAX_DEPTH) {
        last_error = "folders are nested too deeply";
        return false;
    }

    char* child = (char*)malloc(PATH_MAX);
    if (child == NULL) {
        last_error = "out of memory";
        return false;
    }
    char name[NAME_MAX + 1];
    bool ok = true;
    int r;
    for (uint32_t i = 0; ok && (r = entry_at(path, i, name, sizeof(name))) != 0; i++) {
        if (r < 0 || skip_name(name))
            continue;
        if (!join(child, PATH_MAX, path, name)) {
            last_error = "a path was too long";
            ok = false;
            break;
        }
        ok = delete_count(child, depth + 1, count);
    }
    free(child);
    return ok;
}

// Children first, then the item. Failures are counted and skipped. False
// only when cancelled. With `progress`, each item moves the bar.
static bool delete_any(const char* path, uint32_t depth, bool progress, uint32_t* removed, uint32_t* lost) {
    if (cancel) return false;

    int kind = kind_of(path, NULL);
    if (kind == SYS_TYPE_MOUNT) {           // Never: checked before, but not trusted
        (*lost)++;
        last_error = "storage devices can't be deleted";
        return true;
    }
    if (kind == SYS_TYPE_DIRECTORY && depth < DELETE_MAX_DEPTH) {
        char* child = (char*)malloc(PATH_MAX);
        if (child == NULL) {
            (*lost)++;
            last_error = "out of memory";
            return true;
        }
        // Deleting shifts the listing: look at index i again, and only move
        // on past entries that stay (could not be removed, or skipped names)
        char name[NAME_MAX + 1];
        uint32_t i = 0;
        int r;
        while ((r = entry_at(path, i, name, sizeof(name))) != 0) {
            if (cancel) {
                free(child);
                return false;
            }
            if (r < 0 || skip_name(name) || !join(child, PATH_MAX, path, name)) {
                i++;
                continue;
            }
            if (!delete_any(child, depth + 1, progress, removed, lost)) {
                free(child);
                return false;
            }
            if (exists(child))
                i++;
        }
        free(child);
    }

    if (remove(path) == 0 && !exists(path)) {
        (*removed)++;
    } else {
        (*lost)++;
        last_error = (kind == SYS_TYPE_DIRECTORY) ? "a folder could not be removed" : "a file could not be deleted";
    }
    if (progress) {
        strlcpy(shown_name, base_name(path), sizeof(shown_name));
        add_done(1);
    } else {
        pump(false);
    }
    return true;
}

// Remove a tree without moving the bar (a failed copy, a moved original).
// True when it is all gone.
static bool delete_quietly(const char* path) {
    uint32_t removed = 0, lost = 0;
    bool was_cancelled = cancel;
    cancel = false;                 // Cleanup finishes even after Cancel
    const char* saved_error = last_error;
    delete_any(path, 0, false, &removed, &lost);
    last_error = saved_error;
    cancel = was_cancelled;
    return lost == 0 && !exists(path);
}

// delete -i: wait for the answer. True only for Delete (button or Enter).
static bool ask_to_delete(void) {
    if (!ui)
        return false;           // Nobody to ask: delete nothing
    confirming = true;
    draw();
    wm_window_focus(win);

    bool answer = false, decided = false;
    wm_event_t ev;
    while (!decided && wm_wait_event(&ev, -1) > 0) {
        switch (ev.type) {
            case WM_EVENT_KEY:
                if (ev.key == KEY_ESCAPE) decided = true;
                else if (ev.key == KEY_ENTER || ev.key == '\r' || ev.key == '\n') answer = decided = true;
                break;
            case WM_EVENT_MOUSE_DOWN:
                if (ev.button != INPUT_BTN_LEFT) break;
                if (in_rect(delete_button_rect(), ev.x, ev.y)) answer = decided = true;
                else if (in_rect(button_rect(), ev.x, ev.y)) decided = true;
                break;
            case WM_EVENT_THEME_CHANGED:
                if (wm_get_theme(&theme) == 0) draw();
                break;
        }
    }
    confirming = false;
    if (answer)
        draw();                 // Straight to "Preparing"
    return answer;
}

static void run_delete(void) {
    // Preparing: everything counted and checked before anything goes
    uint32_t count = 0;
    for (size_t i = 0; i < item_count; i++) {
        set_stage(PH_COUNTING, items[i].src);
        if (!exists(items[i].src)) {
            last_error = "it no longer exists";
            refused = true;
            failed = 1;
            return;
        }
        if (!delete_count(items[i].src, 0, &count)) {
            if (cancel) {
                cancelled = true;
            } else {
                refused = true;
                failed = 1;
            }
            return;
        }
    }
    total = count;

    phase = PH_DELETING;
    for (size_t i = 0; i < item_count; i++) {
        char dir[PATH_MAX];
        parent_of(items[i].src, dir, sizeof(dir));
        bool go_on = delete_any(items[i].src, 0, true, &ok_count, &failed);
        announce(dir, false);
        if (!go_on || cancel) {
            cancelled = true;
            break;
        }
    }
}

//
// Copying
//

static uint64_t file_weight(uint32_t size) {
    return (uint64_t)size * PASSES + 1;
}

// Work units for `src` and everything below it, by the same rules the copy follows
static uint64_t copy_count(const char* src, uint32_t depth) {
    if (cancel) return 1;
    pump(false);

    SysStat st;
    int kind = kind_of(src, &st);
    if (kind == SYS_TYPE_MOUNT || kind < 0)
        return 1;
    if (kind == SYS_TYPE_FILE) {
        file_total++;
        return file_weight(st.size);
    }
    if (depth >= COPY_MAX_DEPTH)
        return 1;

    char* child = (char*)malloc(PATH_MAX);
    if (child == NULL)
        return 1;
    uint64_t weight = 1;
    char name[NAME_MAX + 1];
    int r;
    for (uint32_t i = 0; !cancel && (r = entry_at(src, i, name, sizeof(name))) != 0; i++) {
        if (r < 0 || skip_name(name) || !join(child, PATH_MAX, src, name))
            continue;
        weight += copy_count(child, depth + 1);
    }
    free(child);
    return weight;
}

static bool read_exact(int fd, uint8_t* p, uint32_t n) {
    while (n > 0) {
        ssize_t got = read(fd, p, n);
        if (got <= 0) return false;
        p += got;
        n -= (uint32_t)got;
    }
    return true;
}

static bool write_exact(int fd, const uint8_t* p, uint32_t n) {
    while (n > 0) {
        ssize_t put = write(fd, p, n);
        if (put <= 0) return false;
        p += put;
        n -= (uint32_t)put;
    }
    return true;
}

// Grow `dst` to `size` in RESERVE_STEP steps, then check it really is that big
static bool reserve(const char* dst, uint32_t size) {
    for (uint32_t reserved = 0; reserved < size; ) {
        if (cancel) return false;
        uint32_t step = size - reserved;
        if (step > RESERVE_STEP) step = RESERVE_STEP;
        reserved += step;
        if (truncate(dst, (off_t)reserved) < 0) {
            last_error = "not enough space for a copy";
            return false;
        }
        pump(false);
    }
    SysStat st;
    if (kind_of(dst, &st) != SYS_TYPE_FILE || st.size != size) {
        last_error = "not enough space for a copy";
        return false;
    }
    return !cancel;
}

static bool copy_file(const char* src, const char* dst) {
    phase = PH_PREPARING;
    file_index++;
    strlcpy(shown_name, base_name(src), sizeof(shown_name));
    pump(true);

    SysStat st;
    if (kind_of(src, &st) != SYS_TYPE_FILE) {
        last_error = "an original could not be read";
        return false;
    }
    uint32_t size = st.size;

    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (out < 0) {
        last_error = (errno == EPERM) ? "no file write clearance" : "a copy could not be created";
        return false;
    }
    close(out);

    // Space first: a write past the end grows the file a step at a time
    if (!reserve(dst, size))
        return false;
    if (size == 0) {
        os_set_permissions(dst, st.permissions & (SYS_PERM_READ | SYS_PERM_WRITE | SYS_PERM_EXECUTE));
        add_done(1);
        return true;
    }

    int in = open(src, O_RDONLY);
    out = open(dst, O_RDWR);
    if (in < 0 || out < 0) {
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        last_error = "a file could not be opened";
        return false;
    }

    bool ok = true;
    set_stage(PH_COPYING, NULL);
    for (uint32_t offset = 0; ok && offset < size; ) {
        uint32_t n = size - offset;
        if (n > CHUNK) n = CHUNK;
        ok = lseek(in, (off_t)offset, SEEK_SET) == (off_t)offset && read_exact(in, buffer, n) &&
             lseek(out, (off_t)offset, SEEK_SET) == (off_t)offset && write_exact(out, buffer, n);
        if (!ok) {
            last_error = "reading or writing failed";
            break;
        }
        offset += n;
        add_done(n);
        if (cancel) ok = false;
    }

#if FILEOP_VERIFY
    if (ok) {
        set_stage(PH_VERIFYING, NULL);
        for (uint32_t offset = 0; ok && offset < size; ) {
            uint32_t n = size - offset;
            if (n > CHUNK) n = CHUNK;
            ok = lseek(in, (off_t)offset, SEEK_SET) == (off_t)offset && read_exact(in, buffer, n) &&
                 lseek(out, (off_t)offset, SEEK_SET) == (off_t)offset && read_exact(out, verify_buffer, n) &&
                 memcmp(buffer, verify_buffer, n) == 0;
            if (!ok) {
                last_error = "a copy did not match its original";
                break;
            }
            offset += n;
            add_done(n);
            if (cancel) ok = false;
        }
    }
#endif

    close(out);
    close(in);
    if (ok) {
        // Same permissions as the original (best effort)
        os_set_permissions(dst, st.permissions & (SYS_PERM_READ | SYS_PERM_WRITE | SYS_PERM_EXECUTE));
        add_done(1);
    }
    return ok;
}

static bool copy_any(const char* src, const char* dst, uint32_t depth);

static bool copy_dir(const char* src, const char* dst, uint32_t depth) {
    if (depth >= COPY_MAX_DEPTH) {
        last_error = "folders are nested too deeply";
        return false;
    }
    if (mkdir(dst, 0777) < 0 || kind_of(dst, NULL) != SYS_TYPE_DIRECTORY) {
        last_error = (errno == EPERM) ? "no file write clearance" : "a folder could not be created";
        return false;
    }
    add_done(1);

    char* child_src = (char*)malloc(PATH_MAX);
    char* child_dst = (char*)malloc(PATH_MAX);
    if (child_src == NULL || child_dst == NULL) {
        free(child_src);
        free(child_dst);
        last_error = "out of memory";
        return false;
    }

    bool ok = true;
    char name[NAME_MAX + 1];
    int r;
    for (uint32_t i = 0; ok && (r = entry_at(src, i, name, sizeof(name))) != 0; i++) {
        if (r < 0 || skip_name(name))
            continue;
        if (!join(child_src, PATH_MAX, src, name) || !join(child_dst, PATH_MAX, dst, name)) {
            last_error = "a path was too long";
            ok = false;
            break;
        }
        ok = copy_any(child_src, child_dst, depth + 1);
    }
    free(child_src);
    free(child_dst);

    if (ok) {
        SysStat st;
        if (kind_of(src, &st) >= 0)
            os_set_permissions(dst, st.permissions & (SYS_PERM_READ | SYS_PERM_WRITE | SYS_PERM_EXECUTE));
    }
    return ok;
}

static bool copy_any(const char* src, const char* dst, uint32_t depth) {
    if (cancel) return false;
    int kind = kind_of(src, NULL);
    if (kind == SYS_TYPE_MOUNT) {
        last_error = "storage devices can't be copied";
        return false;
    }
    if (kind == SYS_TYPE_DIRECTORY)
        return copy_dir(src, dst, depth);
    return copy_file(src, dst);
}

// Where `item` goes: its final path (a taken name becomes "name (1)" with
// -r) and that path's folder. False when it can't go anywhere; the item is
// then counted (failed or skipped) and done with.
static bool destination(const item_t* item, char dst[PATH_MAX], char dst_dir[PATH_MAX]) {
    bool move = (op == OP_MOVE);
    if (!exists(item->src)) {
        last_error = "an original no longer exists";
        failed++;
        return false;
    }
    if (move && strcmp(item->src, item->dst) == 0) {
        skipped++;          // Moving something onto itself: nothing to do
        return false;
    }

    strlcpy(dst, item->dst, PATH_MAX);
    parent_of(item->dst, dst_dir, PATH_MAX);
    if (exists(dst) && (!rename_taken || !unique_path(dst_dir, base_name(item->dst), dst, PATH_MAX))) {
        last_error = "the destination name is taken";
        failed++;
        return false;
    }
    // On the final name: "dir" pasted next to itself is "dir (1)", fine;
    // "dir/inner" never is
    if (within(dst, item->src)) {
        last_error = move ? "a folder can't be moved into itself" : "a folder can't be copied into itself";
        failed++;
        return false;
    }
    return true;
}

// Move: relink the item where it is (os_move). Only its directory entry
// changes, so this is instant whatever the size. True when the item is
// settled (moved, or failed and counted); false when it is on another
// volume and has to be copied and deleted instead.
static bool relink_item(item_t* item) {
    set_stage(PH_PREPARING, item->src);

    char dst[PATH_MAX], dst_dir[PATH_MAX];
    if (!destination(item, dst, dst_dir))
        return true;

    if (os_move(item->src, dst) == 0) {
        char src_dir[PATH_MAX];
        parent_of(item->src, src_dir, sizeof(src_dir));
        ok_count++;
        announce(dst_dir, false);
        announce(src_dir, false);
        return true;
    }
    switch (errno) {
        case EXDEV:                 // Another volume (or one that can't relink)
        case ENOSYS:                // A kernel without SYS_MOVE
            return false;
        case EPERM:  last_error = "no file write clearance"; break;
        case EACCES: last_error = "permission denied"; break;
        case EBUSY:  last_error = "it is in use"; break;
        case EINVAL: last_error = "a folder can't be moved into itself"; break;
        case EEXIST: last_error = "the destination name is taken"; break;
        case ENOENT: last_error = "an original no longer exists"; break;
        default:     last_error = "it could not be moved"; break;
    }
    failed++;
    return true;
}

// One top-level item. False when the job should stop (cancelled).
static bool copy_item(item_t* item) {
    bool move = (op == OP_MOVE);
    set_stage(PH_PREPARING, item->src);

    char dst[PATH_MAX], dst_dir[PATH_MAX];
    if (!destination(item, dst, dst_dir))
        return true;

    // Nothing was at `dst`, so anything there after a failure is ours to remove
    if (!copy_any(item->src, dst, 0)) {
        if (exists(dst))
            delete_quietly(dst);
        if (cancel) {
            cancelled = true;
            return false;
        }
        failed++;
        return true;
    }
    ok_count++;
    announce(dst_dir, false);

    if (move) {
        // A copy plus a delete. If the delete fails the item exists twice,
        // which is safer than losing it.
        char src_dir[PATH_MAX];
        parent_of(item->src, src_dir, sizeof(src_dir));
        if (!delete_quietly(item->src)) {
            last_error = "an original could not be removed after copying";
            failed++;
        }
        announce(src_dir, false);
    }
    return true;
}

// Move: relink every item that is on the same volume as its destination.
// True if something is left to copy (another volume).
static bool relink_all(void) {
    bool left = false;
    for (size_t i = 0; i < item_count; i++) {
        items[i].settled = relink_item(&items[i]);
        if (!items[i].settled)
            left = true;
    }
    return left;
}

static void run_copy(void) {
    // Counting: size the whole job so the bar runs 0-100% once. Items a move
    // already relinked are not part of it.
    for (size_t i = 0; i < item_count && !cancel; i++) {
        if (items[i].settled)
            continue;
        set_stage(PH_COUNTING, items[i].src);
        items[i].weight = exists(items[i].src) ? copy_count(items[i].src, 0) : 1;
        total += items[i].weight;
    }
    if (cancel) {
        cancelled = true;
        return;
    }

    for (size_t i = 0; i < item_count; i++) {
        if (items[i].settled)
            continue;
        uint64_t start = done;
        if (!copy_item(&items[i]))
            break;
        // Failed or skipped part way: the bar still moves to the end of its share
        if (done < start + items[i].weight)
            done = start + items[i].weight;
        pump(false);
    }
    if (cancel)
        cancelled = true;
}

//
// Reporting
//

static void report(void) {
    const char* title = (op == OP_DELETE) ? "Delete" : (op == OP_MOVE) ? "Move" : "Copy";
    const char* verb  = (op == OP_DELETE) ? "deleted" : (op == OP_MOVE) ? "moved" : "copied";
    char message[WM_NOTIFY_TEXT_MAX];

    if (failed > 0 && last_error != NULL) {
        if (refused)
            snprintf(message, sizeof(message), "Nothing was %s: %s", verb, last_error);
        else
            snprintf(message, sizeof(message), "%u item%s not %s: %s", (unsigned)failed,
                     failed == 1 ? "" : "s", verb, last_error);
        wm_notify_ex(WM_NOTIFY_ERROR, title, message, WM_NOTIFY_STICKY);
    } else if (cancelled && ok_count > 0 && (op != OP_COPY)) {
        // Deletes and moves can't be undone: say what is already done
        snprintf(message, sizeof(message), "Cancelled. %u item%s already %s", (unsigned)ok_count,
                 ok_count == 1 ? " was" : "s were", verb);
        wm_notify_ex(WM_NOTIFY_INFO, title, message, WM_NOTIFY_DEFAULT_MS * 2);
    }
}

//
// Start-up
//

static int usage(void) {
    wm_notify_ex(WM_NOTIFY_ERROR, "fileop",
                 "copy|move [-r] SRC DST ..., delete [-i] PATH ..., paste DIR", WM_NOTIFY_DEFAULT_MS);
    return 2;
}

static bool add_item(const char* src, const char* dst) {
    item_t* grown = (item_t*)realloc(items, (item_count + 1) * sizeof(item_t));
    if (grown == NULL)
        return false;
    items = grown;
    item_t* it = &items[item_count];
    memset(it, 0, sizeof(*it));
    if (os_abspath(src, it->src, sizeof(it->src)) == NULL)
        return false;
    if (dst != NULL && os_abspath(dst, it->dst, sizeof(it->dst)) == NULL)
        return false;
    item_count++;
    return true;
}

// Some folders can be read by every program but changed only with more
// permissions (os_finfo's write_security: <home>/sys needs admin). Ask
// the user once, before anything is touched, for whatever the whole job
// lacks: the destination folders, and for a move or delete the items and
// the folders they leave. Nothing is asked when nothing is missing; a
// refusal just lets each item fail with "permission denied".
static void ask_for_write_access(void) {
    sec_mask_t missing = 0;
    char dir[PATH_MAX];
    for (size_t i = 0; i < item_count; i++) {
        if (op != OP_COPY)
            missing |= sec_write_missing(items[i].src, true);
        if (op != OP_DELETE) {
            parent_of(items[i].dst, dir, sizeof(dir));
            missing |= sec_write_missing(dir, false);
        }
    }
    if (missing == 0)
        return;
    const char* what = (op == OP_DELETE) ? "Delete" : (op == OP_MOVE) ? "Move" : "Copy";
    char reason[96];
    if (item_count == 1)
        snprintf(reason, sizeof(reason), "%s %s", what, base_name(items[0].src));
    else
        snprintf(reason, sizeof(reason), "%s %u items", what, (unsigned)item_count);
    sec_elevate_bits(reason, missing);  // Held until fileop ends
}

// fileop paste DIR: the clipboard's files. Returns the clipboard serial to
// clear after a complete move (0: none), or -1 when there is nothing to do.
static int64_t take_clipboard(const char* dir_arg) {
    char dir[PATH_MAX];
    if (os_abspath(dir_arg, dir, sizeof(dir)) == NULL)
        return -1;

    wm_clip_info_t info;
    char* list = wm_clip_get(&info);
    if (list == NULL || info.type != WM_CLIP_FILES) {
        wm_notify("Paste", list == NULL ? "The clipboard is empty" : "No files on the clipboard");
        free(list);
        return -1;
    }
    char* save = NULL;
    for (char* path = strtok_r(list, "\n", &save); path != NULL; path = strtok_r(NULL, "\n", &save)) {
        char dst[PATH_MAX];
        if (join(dst, sizeof(dst), dir, base_name(path)))
            add_item(path, dst);
    }
    free(list);

    op = (info.flags & WM_CLIP_CUT) ? OP_MOVE : OP_COPY;
    rename_taken = true;
    return (op == OP_MOVE) ? (int64_t)info.serial : 0;
}

static void default_theme(void) {
    theme.client.background = 0xFF141420; theme.client.background_danger = 0xFF100101;
    theme.client.text = 0xFFFFFFFF;       theme.client.text_value = 0xFF3FFF3F;
    theme.client.text_muted = 0xFF8888AA; theme.client.text_danger = 0xFFC00404;
    theme.client.accent = 0xFF04C004;
    theme.button.fill = 0xFF1C1C2A;       theme.button.border = 0xFF444466;
    theme.button.text = 0xFF3FFF3F;
    theme.edit.background = 0xFF202020;   theme.edit.border = 0xFF404040;
}

// Connected already (main)
static void open_window(void) {
    if (wm_get_theme(&theme) < 0)
        default_theme();

    // Centered, nudged per process so several jobs don't hide each other
    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    int32_t nudge = (int32_t)(getpid() % 5) * 20;
    wm_window_desc_t d = {
        .title  = (op == OP_DELETE) ? (ask_first ? "Delete" : "Deleting files") : (op == OP_MOVE) ? "Moving files" : "Copying files",
        .x      = (sw > WIN_W) ? (sw - WIN_W) / 2 + nudge : WM_POS_DEFAULT,
        .y      = (sh > WIN_H) ? (sh - WIN_H - 20) / 2 + nudge : WM_POS_DEFAULT,
        .width  = WIN_W,
        .height = WIN_H,
        .style  = WM_STYLE_NOCLOSEBOX,          // Cancel is the way out
    };
    win = wm_window_create(&d);
    if (win == 0)
        return;
    ui = true;
    draw();
}

int main(int argc, char** argv) {
    if (argc < 3)
        return usage();

    int64_t clip_serial = 0;
    const char* verb = argv[1];
    int i = 2;

    if (strcmp(verb, "paste") == 0) {
        if (argc != 3)
            return usage();
        clip_serial = take_clipboard(argv[2]);
        if (clip_serial < 0)
            return 1;
    } else if (strcmp(verb, "copy") == 0 || strcmp(verb, "move") == 0) {
        op = (verb[0] == 'c') ? OP_COPY : OP_MOVE;
        if (strcmp(argv[i], "-r") == 0) {
            rename_taken = true;
            i++;
        }
        if (argc - i < 2 || (argc - i) % 2 != 0)
            return usage();
        for (; i + 1 < argc; i += 2)
            if (!add_item(argv[i], argv[i + 1]))
                return usage();
    } else if (strcmp(verb, "delete") == 0) {
        op = OP_DELETE;
        if (strcmp(argv[i], "-i") == 0) {
            ask_first = true;
            if (++i >= argc)
                return usage();
        }
        for (; i < argc; i++) {
            if (!add_item(argv[i], NULL))
                return usage();
            if (kind_of(items[item_count - 1].src, NULL) == SYS_TYPE_MOUNT) {
                wm_notify("Delete", "Storage devices can't be deleted");
                return 1;
            }
        }
    } else {
        return usage();
    }
    if (item_count == 0)
        return usage();

    strlcpy(shown_name, base_name(items[0].src), sizeof(shown_name));
    if (op != OP_DELETE) {
        buffer = (uint8_t*)malloc(CHUNK);
        verify_buffer = (uint8_t*)malloc(CHUNK);
        if (buffer == NULL || verify_buffer == NULL) {
            wm_notify_ex(WM_NOTIFY_ERROR, "Copy", "Out of memory", WM_NOTIFY_DEFAULT_MS);
            return 1;
        }
    }

    // Copy and move ask for any permissions they lack now; delete asks
    // after its own question (below)
    if (op != OP_DELETE)
        ask_for_write_access();

    // A move within one volume is only relinking: done before any window
    // would show. The window opens only for what has to be copied.
    bool work = true;
    if (op == OP_MOVE)
        work = relink_all();

    if (work)
        open_window();
    if (op == OP_DELETE && ask_first && !ask_to_delete()) {
        // Not confirmed (or nobody could be asked): nothing touched
        if (ui)
            wm_window_destroy(win);
        wm_disconnect();
        free(items);
        return 3;
    }
    if (op == OP_DELETE) {
        ask_for_write_access();
        run_delete();
    }
    else if (work)
        run_copy();

    announce(NULL, true);           // Last changes out now
    os_sync();                      // And on disk, not just in the cache
    report();

    // A cut is used up once everything arrived
    if (clip_serial > 0 && failed == 0 && !cancelled)
        wm_clip_clear((uint32_t)clip_serial);

    if (ui)
        wm_window_destroy(win);
    wm_disconnect();
    free(items);
    free(buffer);
    free(verify_buffer);
    return cancelled ? 3 : (failed > 0 ? 1 : 0);
}
