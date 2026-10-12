#include <kernel/dwm/windows/start_menu.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/kernel.h>
#include <kernel/malloc.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/console/display.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/watchdog.h>
#include <kernel/util/string.h>
#include <kernel/vfs/vfs.h>
#include <kernel/registry/registry.h>
#include <kernel/events.h>

// Single instance; 0 while closed
static WindowHandle start_menu_handle = 0;

// Open:    slides up from behind the taskbar to its spot above it.
// Closing: slides back down, then the window is closed for real. While
//          closing it counts as closed (button up, clicks and keys ignored),
//          and opening it again just turns it around.
static bool sm_closing    = false;
static bool sm_close_sent = false;      // CLOSE posted: too late to turn around

#define SM_SLIDE_DIV       3            // A bit quicker than notifications

// ==========================================
// Layout (window-local coordinates, 6x8 font)
// ==========================================
//
//   +------------------------------+
//   | header                       |
//   | item rows, or search results |
//   |   while a search is typed    |
//   | ---------------------------- |
//   |                      [power] |
//   | [search box                ] |
//   +------------------------------+
//
// The bottom rows are laid out from the bottom edge up.

#define SM_HEADER_H        44
#define SM_ITEM_TOP        50
#define SM_ROW_H           24
#define SM_ROW_X           6
#define SM_ACCENT_W        4
#define SM_BULLET_SIZE     8
#define SM_TEXT_X          30
#define SM_FONT_W          6
#define SM_FONT_H          8

#define SM_PAD_BOTTOM      8
#define SM_SEARCH_H        18
#define SM_SEARCH_PAD      5            // Text inset inside the search box
#define SM_POWER_SIZE      18
#define SM_POWER_GAP       6            // Power box to search box

#define SM_SEARCH_Y        (DWM_START_MENU_HEIGHT - SM_PAD_BOTTOM - SM_SEARCH_H)
#define SM_POWER_Y         (SM_SEARCH_Y - SM_POWER_GAP - SM_POWER_SIZE)
#define SM_SEPARATOR_Y     (SM_POWER_Y - 6)

#define SM_SEARCH_MAX      48           // Characters (the box scrolls past what fits)

#define SM_KEY_BACKSPACE   0x01
#define SM_KEY_ENTER       0x02
#define SM_KEY_ESCAPE      0x1B

// Colors (from the system theme)
#define SM_COLOR_BG          (theme.client.background)
#define SM_COLOR_ACCENT      (theme.client.accent)
#define SM_COLOR_DIVIDER     (theme.start_menu.separator)
#define SM_COLOR_TEXT        (theme.client.text)
#define SM_COLOR_SUBTEXT     (theme.client.text_muted)
#define SM_COLOR_ROW         (theme.start_menu.row)
#define SM_COLOR_ROW_BORDER  (theme.start_menu.row_border)

// ==========================================
// Search results layout and limits
// ==========================================
#define SM_RESULT_ROW_H      20
#define SM_RESULT_MAX        ((SM_SEPARATOR_Y - SM_ITEM_TOP) / SM_RESULT_ROW_H)   // 6

#define SM_SEARCH_DEPTH      8          // Folder levels below <home>/usr
#define SM_SEARCH_QUEUE_MAX  1024       // Folders waiting to be listed
#define SM_DEBOUNCE_MS       120        // Typing pause before a search starts
#define SM_POST_RETRIES      200        // dwm_invoke retries, 5 ms apart

#define SM_COLOR_FOLDER      0xFFFDA008
#define SM_COLOR_FILE        0xFF3FFF3F

// ==========================================
// Search text (DWM lock)
// ==========================================
// The menu has keyboard focus while open, so typed keys arrive as
// DWM_EVENT_KEYBOARD and go straight into the search box.

static char     sm_search[SM_SEARCH_MAX + 1];
static uint16_t sm_search_len = 0;

// ==========================================
// Search
// ==========================================
//
// Threading:
//
//   DWM thread      edits the query, publishes it with a new generation
//                   number, draws results, handles clicks.
//   Search thread   walks <home>/usr with the public vfs_* calls (each takes
//                   the VFS lock on its own). It NEVER touches DWM state or
//                   takes the DWM lock; results go back through dwm_invoke.
//
// Every query change bumps sm_req_gen. The search thread checks it before
// every item and abandons a stale walk; the DWM thread drops results whose
// generation is no longer current. The request fields are shared under
// irq_save (a few dozen bytes, no calls), so no new lock enters the lock order.
//
// Results are found breadth first, so shallow matches come before deep
// ones. The walk stops once SM_RESULT_MAX matches are found.

struct SmResult {
    char path[DWM_MAX_PATH_LEN];        // DWM_MAX_PATH_LEN: what icons, the
    bool is_dir;                        // clipboard and Properties accept
};

struct SmResultsMsg {
    uint32_t        gen;
    uint32_t        count;
    bool            more;               // Stopped with matches left over
    struct SmResult results[SM_RESULT_MAX];
};

// ---- Shared request (irq_save) ----
static volatile uint32_t sm_req_gen = 0;
static char              sm_req_query[SM_SEARCH_MAX + 1];
static char              sm_req_root[DWM_MAX_PATH_LEN];

static Event             sm_search_event  = EVENT_INITIALIZER;
static ThreadBlock*      sm_search_thread = NULL;

// ---- DWM thread only ----
static struct SmResult   sm_results[SM_RESULT_MAX];
static uint32_t          sm_result_count = 0;
static bool              sm_result_more  = false;
static bool              sm_searching    = false;
static bool              sm_no_root      = false;

// Result the context menu was opened on (copied: results may change while
// the menu is up)
static struct SmResult   sm_ctx_item;
static bool              sm_ctx_valid = false;

// ---- Helpers ----

static size_t sm_u32_to_str(uint32_t value, char* out) {
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

static char sm_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static const char* sm_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

static bool sm_join(char* out, size_t size, const char* dir, const char* name) {
    size_t dir_len  = strlen(dir);
    size_t name_len = strlen(name);
    bool   slash    = (dir_len == 0 || dir[dir_len - 1] != '/');

    if (dir_len + (slash ? 1 : 0) + name_len + 1 > size) return false;

    memcpy(out, dir, dir_len);
    if (slash) out[dir_len++] = '/';
    memcpy(&out[dir_len], name, name_len + 1);
    return true;
}

static void sm_parent(const char* path, char* out, size_t size) {
    memset(out, '\0', size);
    strncpy(out, path, size - 1);

    char* slash = strrchr(out, '/');
    if (slash == NULL)      strncpy(out, "/", size - 1);
    else if (slash == out)  out[1] = '\0';
    else                    *slash = '\0';
}

// Case-insensitive substring match. `query` is already lowercase.
static bool sm_name_matches(const char* name, const char* query, size_t qlen) {
    size_t n = strlen(name);
    if (qlen == 0 || qlen > n) return false;

    for (size_t i = 0; i + qlen <= n; i++) {
        size_t j = 0;
        while (j < qlen && sm_lower(name[i + j]) == query[j]) j++;
        if (j == qlen) return true;
    }
    return false;
}

// Lowercase and trim spaces in place
static void sm_prepare_query(char* q) {
    size_t start = 0;
    while (q[start] == ' ') start++;

    size_t len = strlen(&q[start]);
    memmove(q, &q[start], len + 1);
    while (len > 0 && q[len - 1] == ' ') q[--len] = '\0';

    for (size_t i = 0; i < len; i++) q[i] = sm_lower(q[i]);
}

// "<home>/usr". False when there is no home device or no usr folder.
static bool sm_get_search_root(char* out, size_t size) {
    memset(out, '\0', size);

    struct LocalPaths paths;
    kernel_get_local_paths(&paths);
    if (paths.home[0] == '\0') return false;

    strncpy(out, paths.home, size - 1);
    strncat(out, "/usr", size - strlen(out) - 1);
    return vfs_directory_check(out);
}

// ---- Search thread ----

struct SmDir {
    struct SmDir* next;
    uint32_t      depth;
    char          path[DWM_MAX_PATH_LEN];
};

// Breadth-first walk below `root`. Returns false when a newer query
// arrived (the results are then useless).
static bool sm_run_search(uint32_t gen, const char* query, const char* root, struct SmResultsMsg* out) {
    size_t qlen = strlen(query);

    struct SmDir* head = (struct SmDir*)malloc(sizeof(struct SmDir));
    if (head == NULL) return true;      // Nothing found: better than "Searching..." forever
    memset(head, 0, sizeof(struct SmDir));
    strncpy(head->path, root, sizeof(head->path) - 1);

    struct SmDir* tail = head;
    uint32_t queued = 1;
    bool cancelled = false;
    bool stop = false;

    char name[VFS_NAME_MAX + 1];
    char child[DWM_MAX_PATH_LEN];

    while (head != NULL && !stop) {
        struct SmDir* dir = head;
        head = dir->next;
        if (head == NULL) tail = NULL;
        queued--;

        uint32_t count = vfs_directory_get_item_count(dir->path);

        for (uint32_t i = 0; i < count; i++) {
            if (sm_req_gen != gen) {
                cancelled = true;
                stop = true;
                break;
            }
            wd_pet();

            memset(name, '\0', sizeof(name));
            if (!vfs_directory_get_item(dir->path, i, name, sizeof(name))) continue;
            if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            // Paths the rest of the DWM cannot hold are not worth listing
            if (!sm_join(child, sizeof(child), dir->path, name)) continue;

            bool is_dir = vfs_directory_check(child);

            if (sm_name_matches(name, query, qlen)) {
                if (out->count >= SM_RESULT_MAX) {
                    out->more = true;
                    stop = true;
                    break;
                }
                struct SmResult* r = &out->results[out->count++];
                memcpy(r->path, child, sizeof(r->path));
                r->is_dir = is_dir;
            }

            // Never wander into another device mounted inside the tree
            if (is_dir && dir->depth + 1 < SM_SEARCH_DEPTH &&
                queued < SM_SEARCH_QUEUE_MAX && !vfs_directory_check_mounted(child)) {
                struct SmDir* sub = (struct SmDir*)malloc(sizeof(struct SmDir));
                if (sub != NULL) {
                    memset(sub, 0, sizeof(struct SmDir));
                    sub->depth = dir->depth + 1;
                    memcpy(sub->path, child, sizeof(sub->path));
                    if (tail != NULL) tail->next = sub;
                    else              head = sub;
                    tail = sub;
                    queued++;
                }
            }
        }

        free(dir);
    }

    while (head != NULL) {
        struct SmDir* next = head->next;
        free(head);
        head = next;
    }

    return !cancelled;
}

// DWM thread, DWM lock held (runs through dwm_invoke)
static void sm_ui_results(void* arg) {
    struct SmResultsMsg* m = (struct SmResultsMsg*)arg;

    if (m->gen == sm_req_gen && sm_search_len > 0) {
        memcpy(sm_results, m->results, sizeof(sm_results));
        sm_result_count = m->count;
        sm_result_more  = m->more;
        sm_searching    = false;

        if (start_menu_handle != 0)
            dwm_window_send_event(start_menu_handle, DWM_EVENT_REDRAW);
    }

    free(m);
}

// Search thread: hand results to the DWM thread, retrying while its queue
// is full. Gives up early once the results are stale anyway.
static void sm_post_results(struct SmResultsMsg* msg) {
    for (int i = 0; i < SM_POST_RETRIES; i++) {
        if (msg->gen != sm_req_gen) break;
        if (dwm_invoke(sm_ui_results, msg)) return;
        wd_pet();
        thread_sleep(5);
    }
    free(msg);
}

static void sm_search_thread_main(void) {
    char query[SM_SEARCH_MAX + 1];
    char root[DWM_MAX_PATH_LEN];

    while (1) {
        event_wait(&sm_search_event, 0);    // 0 = until signaled
        wd_pet();

        // Wait for a pause in typing so every keystroke doesn't start a walk
        uint32_t gen;
        do {
            gen = sm_req_gen;
            thread_sleep(SM_DEBOUNCE_MS);
            wd_pet();
        } while (gen != sm_req_gen);

        uint32_t flags = irq_save();
        gen = sm_req_gen;
        memcpy(query, sm_req_query, sizeof(query));
        memcpy(root,  sm_req_root,  sizeof(root));
        irq_restore(flags);

        query[sizeof(query) - 1] = '\0';
        root[sizeof(root) - 1]   = '\0';
        sm_prepare_query(query);

        struct SmResultsMsg* msg = (struct SmResultsMsg*)malloc(sizeof(struct SmResultsMsg));
        if (msg == NULL) continue;
        memset(msg, 0, sizeof(struct SmResultsMsg));
        msg->gen = gen;

        // A query of only spaces still gets an (empty) answer
        bool current = true;
        if (query[0] != '\0' && root[0] != '\0')
            current = sm_run_search(gen, query, root, msg);

        if (current) sm_post_results(msg);
        else         free(msg);
    }
}

static bool sm_search_thread_start(void) {
    if (sm_search_thread != NULL) return true;

    sm_search_thread = thread_create(sm_search_thread_main, PRIORITY_NORMAL);
    if (sm_search_thread == NULL) return false;

    // Report-only: a big user folder on a slow disk can take a while
    wd_watch(sm_search_thread, "dwm-search", 10000, WD_NEVER);
    return true;
}

// The search text changed (DWM thread, DWM lock held)
static void sm_search_changed(void) {
    char root[DWM_MAX_PATH_LEN];
    bool have_root = (sm_search_len > 0) && sm_get_search_root(root, sizeof(root));
    if (!have_root) memset(root, '\0', sizeof(root));

    uint32_t flags = irq_save();
    sm_req_gen++;
    memcpy(sm_req_query, sm_search, sizeof(sm_req_query));
    memcpy(sm_req_root, root, sizeof(sm_req_root));
    irq_restore(flags);

    sm_no_root = false;

    if (sm_search_len == 0) {
        sm_result_count = 0;
        sm_result_more  = false;
        sm_searching    = false;
        return;
    }

    if (!have_root) {
        sm_no_root      = true;
        sm_result_count = 0;
        sm_result_more  = false;
        sm_searching    = false;
        return;
    }

    if (!sm_search_thread_start()) {
        sm_result_count = 0;
        sm_searching    = false;
        return;
    }

    // Old results stay on screen until the new ones arrive (no flicker)
    sm_searching = true;
    event_signal(&sm_search_event);
}

static void sm_search_clear(void) {
    memset(sm_search, '\0', sizeof(sm_search));
    sm_search_len = 0;
    sm_search_changed();
}

// ==========================================
// Menu items
// ==========================================
// Bullet colors identify each item, so they stay here rather than in the theme
typedef void (*StartMenuAction)(void);

typedef struct {
    const char*     text;
    uint32_t        bullet_color;
    StartMenuAction action;
} StartMenuItem;

static void action_explorer(void) {
    dwm_kernel_event_send(KEVENT_EXECUTE, "explorer", "/");
}

static void action_documents(void) {
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);
    dwm_kernel_event_send(KEVENT_EXECUTE, "explorer", paths.home);
}

static void action_notepad(void) {
    dwm_kernel_event_send(KEVENT_EXECUTE, "notepad", "");
}

static void action_settings(void) {
    dwm_notify("Settings", "Settings are not implemented yet", DWM_NOTIFY_DEFAULT_MS);
}

static void action_run(void) {
    dwm_notify("Run", "Run dialog is not implemented yet", DWM_NOTIFY_DEFAULT_MS);
}

static void action_shutdown(void) {
    // Syncing mid-copy would save a half-written file
    if (dwm_filecopy_busy()) {
        dwm_notify("Shut down", "Wait for the copy to finish (or cancel it) first", DWM_NOTIFY_DEFAULT_MS);
        return;
    }
    
    // Same for a delete still removing items
    if (dwm_filedelete_busy()) {
        dwm_notify("Shut down", "Wait for the delete to finish (or cancel it) first", DWM_NOTIFY_DEFAULT_MS);
        return;
    }
    
    // Everything that writes to disk goes first; the device flush goes last
    // so it pushes out what the earlier steps left in the caches. Every step
    // runs even if an earlier one fails, so one problem doesn't block the rest.
    bool layout_ok   = dwm_desktop_layout_save();
    bool registry_ok = registry_hive_save_all();
    bool storage_ok  = vfs_sync_all();
    
    if (layout_ok && registry_ok && storage_ok) {
        // Sticky: you want to see this before pulling the plug
        dwm_notify("Shut down", "Saved. It is safe to power off", DWM_NOTIFY_STICKY);
        return;
    }
    
    // Name what failed
    char message[64] = "Save failed:";
    if (!layout_ok)   strncat(message, " desktop", sizeof(message) - strlen(message) - 1);
    if (!registry_ok) strncat(message, " registry", sizeof(message) - strlen(message) - 1);
    if (!storage_ok)  strncat(message, " storage", sizeof(message) - strlen(message) - 1);
    
    dwm_notify("Shut down", message, DWM_NOTIFY_STICKY);
}

static const StartMenuItem main_items[] = {
    { "File explorer", 0xFFFDA008, action_explorer  },
    { "Documents",     0xFF3F9FFF, action_documents },
    { "Notepad",       0xFF3FFF3F, action_notepad   },
    { "Settings",      0xFFAAAAAA, action_settings  },
    { "Run...",        0xFFF700F7, action_run       },
};

// Drawn as a plain box (no text) on the right, above the search box
static const StartMenuItem power_item = { "Shut down", 0xFFC00404, action_shutdown };

#define SM_MAIN_COUNT  (sizeof(main_items) / sizeof(main_items[0]))

// ==========================================
// Search result actions (DWM thread, DWM lock held)
// ==========================================

enum {
    SM_CTX_OPEN = 0,
    SM_CTX_LOCATION,
    SM_CTX_COPY,
    SM_CTX_CUT,
    SM_CTX_DELETE,
    SM_CTX_PROPERTIES,
};

static const char* sm_ctx_labels[] = {
    "Open",
    "Open location",
    "Copy",
    "Cut",
    "Delete",
    "Properties",
};

#define SM_CTX_COUNT  (sizeof(sm_ctx_labels) / sizeof(sm_ctx_labels[0]))

// The walk ran a moment ago; the item may have been moved or deleted since
static bool sm_result_still_exists(const struct SmResult* r) {
    if (vfs_exists(r->path)) return true;
    dwm_notify("Search", "That item no longer exists", DWM_NOTIFY_DEFAULT_MS);
    return false;
}

// Same as double clicking a desktop icon
static void sm_open_result(const struct SmResult* r) {
    if (!sm_result_still_exists(r)) return;

    if (r->is_dir)                              dwm_kernel_event_send(KEVENT_EXECUTE, "explorer", r->path);
    else if (dwm_path_is_executable(r->path))   dwm_launch_program(r->path);
    else                                        dwm_kernel_event_send(KEVENT_EXECUTE, "notepad",  r->path);
}

static void sm_context_action(const struct SmResult* r, uint32_t index) {
    // Icon index convention of dwm_create_folder / dwm_create_file
    uint16_t icon_index = r->is_dir ? 0 : 1;

    switch (index) {
    case SM_CTX_OPEN:
        sm_open_result(r);
        break;

    case SM_CTX_LOCATION: {
        if (!sm_result_still_exists(r)) break;
        char parent[DWM_MAX_PATH_LEN];
        sm_parent(r->path, parent, sizeof(parent));
        dwm_kernel_event_send(KEVENT_EXECUTE, "explorer", parent);
        break;
    }

    // These explain their own failures
    case SM_CTX_COPY:
        dwm_clipboard_put_file(r->path, DWM_CLIPBOARD_OP_COPY);
        break;

    case SM_CTX_CUT:
        dwm_clipboard_put_file(r->path, DWM_CLIPBOARD_OP_CUT);
        break;

    case SM_CTX_DELETE:
        if (!sm_result_still_exists(r)) break;
        dwm_summon_dialog_delete("Deletion request", r->path, 0, icon_index);
        break;

    case SM_CTX_PROPERTIES:
        if (!sm_result_still_exists(r)) break;
        dwm_summon_properties("Properties", sm_basename(r->path), r->path, icon_index);
        break;

    default:
        break;
    }
}

// Close a context menu this window opened. The DWM keeps a raw pointer to
// the owner (ctxmenu.handle) and dereferences it when an item is picked, so
// it must not outlive the window.
static void sm_release_context_menu(WindowHandle handle) {
    struct WindowObject* self = dwm_get_window_by_id(handle);
    if (self == NULL || ctxmenu.handle != self) return;

    if (ctxmenu.menu_directive == DWM_CONTEXT_MENU_USER) {
        for (int i = 0; i < ctxmenu.menu_count; i++) {
            ctxmenu.menus[i].visible = false;
            dwm_invalidate_region(ctxmenu.menus[i].x, ctxmenu.menus[i].y,
                                  ctxmenu.menus[i].w, ctxmenu.menus[i].h);
        }
        ctxmenu.menu_count = 0;
    }
    ctxmenu.handle = NULL;
}

// ==========================================
// Open / close
// ==========================================

static int sm_power_x(uint16_t width) {
    return (int)width - SM_ROW_X - SM_POWER_SIZE;
}

// Resting spot: bottom left, just above the taskbar (1px border + 2px gap)
static int sm_shown_y(void) {
    return display_get_height() - taskbar.height - DWM_START_MENU_HEIGHT - 3;
}

// Hidden spot: top edge (and its 1px border) just under the taskbar's top
// edge. The taskbar is drawn over every other topmost window and the rest
// is below the screen, so nothing of the menu shows.
static int sm_hidden_y(void) {
    return display_get_height() - taskbar.height + 1;
}

// The menu window, or NULL (forgetting a handle that no longer exists)
static struct WindowObject* sm_window(void) {
    if (start_menu_handle == 0) return NULL;

    struct WindowObject* window = dwm_get_window_by_id(start_menu_handle);
    if (window == NULL) {
        start_menu_handle = 0;
        sm_closing = false;
        sm_close_sent = false;
    }
    return window;
}

bool dwm_start_menu_is_open(void) {
    DWM_GUARD();
    return sm_window() != NULL && !sm_closing;
}

WindowHandle dwm_start_menu_open(void) {
    DWM_GUARD();
    struct WindowObject* existing = sm_window();
    if (existing != NULL) {
        if (!sm_closing) return start_menu_handle;

        if (!sm_close_sent) {
            // Still sliding down: turn around (the search text and results are kept)
            sm_closing = false;
            dwm_set_focus(existing);
            dwm_window_send_event(taskbar.window, DWM_EVENT_REDRAW);
            return start_menu_handle;
        }

        // Its CLOSE is already queued: let it go and open a fresh one
        start_menu_handle = 0;
        sm_closing = false;
        sm_close_sent = false;
    }

    WindowClass wclass;
    memset(&wclass, 0, sizeof(WindowClass));

    wclass.width      = DWM_START_MENU_WIDTH;
    wclass.height     = DWM_START_MENU_HEIGHT;
    wclass.max_width  = DWM_START_MENU_WIDTH;
    wclass.max_height = DWM_START_MENU_HEIGHT;

    // Starts hidden behind the taskbar; dwm_start_menu_tick slides it up
    wclass.x = 3;
    wclass.y = (uint16_t)sm_hidden_y();

    strncpy(wclass.title, "Start", DWM_MAX_TITLE_LEN - 1);

    // A fresh menu starts with an empty search
    sm_search_clear();
    sm_ctx_valid = false;

    struct WindowObject* menu = dwm_allocate_window(
        wclass,
        DWM_WSTYLE_TOPMOST | DWM_WSTYLE_NOTITLEBAR | DWM_WSTYLE_NOCLOSEBOX,
        (WindowProcedure)callback_start_menu_handler
    );
    if (menu == NULL) return 0;

    start_menu_handle = menu->id;
    sm_closing = false;
    sm_close_sent = false;

    // Keyboard focus: keys typed right after clicking Start land in the
    // search box (the DWM thread posts keys to the focused window)
    dwm_set_focus(menu);

    // Let the taskbar draw its button pressed
    dwm_window_send_event(taskbar.window, DWM_EVENT_REDRAW);
    return start_menu_handle;
}

// Starts the slide down; dwm_start_menu_tick closes the window at the bottom
void dwm_start_menu_close(void) {
    DWM_GUARD();
    if (!dwm_start_menu_is_open()) return;
    sm_closing = true;

    // A result's context menu has nothing left to act on
    sm_release_context_menu(start_menu_handle);
    sm_ctx_valid = false;

    // Pop the taskbar button up now, not when the slide ends
    dwm_window_send_event(taskbar.window, DWM_EVENT_REDRAW);
}

// Once per frame from dwm_update (DWM thread, DWM lock held)
void dwm_start_menu_tick(void) {
    struct WindowObject* menu = sm_window();
    if (menu == NULL) return;

    int target = sm_closing ? sm_hidden_y() : sm_shown_y();
    int y = dwm_slide_step(menu->y, target, SM_SLIDE_DIV);
    if (y != (int)menu->y) dwm_window_place(menu, menu->x, y);

    // All the way down: close it for real (DESTROY forgets the handle)
    if (sm_closing && !sm_close_sent && (int)menu->y == target) {
        sm_close_sent = true;
        dwm_window_send_event(start_menu_handle, DWM_EVENT_CLOSE);
    }
}

void dwm_start_menu_toggle(void) {
    DWM_GUARD();
    if (dwm_start_menu_is_open()) {
        dwm_start_menu_close();
    } else {
        dwm_start_menu_open();
    }
}

// ==========================================
// Drawing
// ==========================================
static void draw_row(uint16_t width, uint16_t row_y, const StartMenuItem* item) {
    uint16_t row_w = width - (SM_ROW_X * 2);
    
    dwm_draw_rect_filled(SM_ROW_X, row_y + 1, row_w, SM_ROW_H - 2, SM_COLOR_ROW);
    dwm_draw_rect(SM_ROW_X, row_y + 1, row_w, SM_ROW_H - 2, SM_COLOR_ROW_BORDER);
    
    uint16_t bullet_x = SM_ROW_X + 10;
    uint16_t bullet_y = row_y + ((SM_ROW_H - SM_BULLET_SIZE) / 2);
    dwm_draw_rect_filled(bullet_x, bullet_y, SM_BULLET_SIZE, SM_BULLET_SIZE, item->bullet_color);
    
    dwm_draw_text(SM_TEXT_X, row_y + ((SM_ROW_H - SM_FONT_H) / 2), item->text, SM_COLOR_TEXT);
}

// Copy `src` cut to `cols` characters, ending in "..." when it was cut
static size_t sm_fit(char* out, size_t out_size, const char* src, int cols) {
    if (out_size == 0) return 0;
    if (cols < 0) cols = 0;
    if ((size_t)cols > out_size - 1) cols = (int)(out_size - 1);

    size_t len = strlen(src);
    if (len <= (size_t)cols) {
        memcpy(out, src, len + 1);
        return len;
    }

    if (cols < 4) {
        memcpy(out, src, (size_t)cols);
        out[cols] = '\0';
        return (size_t)cols;
    }

    memcpy(out, src, (size_t)cols - 3);
    memcpy(&out[cols - 3], "...", 4);
    return (size_t)cols;
}

// Bullet (folder or file color), the name, then the folder it is in (muted)
static void draw_result_row(uint16_t width, uint16_t row_y, const struct SmResult* r, bool is_default) {
    uint16_t row_w = width - (SM_ROW_X * 2);

    // The first result is what Enter opens: give it the active border
    uint32_t border = is_default ? theme.button.border_active : SM_COLOR_ROW_BORDER;

    dwm_draw_rect_filled(SM_ROW_X, row_y + 1, row_w, SM_RESULT_ROW_H - 2, SM_COLOR_ROW);
    dwm_draw_rect(SM_ROW_X, row_y + 1, row_w, SM_RESULT_ROW_H - 2, border);

    uint16_t bullet_x = SM_ROW_X + 10;
    uint16_t bullet_y = row_y + ((SM_RESULT_ROW_H - SM_BULLET_SIZE) / 2);
    dwm_draw_rect_filled(bullet_x, bullet_y, SM_BULLET_SIZE, SM_BULLET_SIZE,
                         r->is_dir ? SM_COLOR_FOLDER : SM_COLOR_FILE);

    int cols   = ((int)width - SM_TEXT_X - SM_ROW_X - 4) / SM_FONT_W;
    int text_y = row_y + ((SM_RESULT_ROW_H - SM_FONT_H) / 2);

    char shown[DWM_MAX_PATH_LEN];
    size_t used = sm_fit(shown, sizeof(shown), sm_basename(r->path), cols);
    dwm_draw_text(SM_TEXT_X, text_y, shown, SM_COLOR_TEXT);

    // Room left over: name the containing folder, so equal names can be told apart
    int remaining = cols - (int)used - 2;
    if (remaining >= 4) {
        char parent[DWM_MAX_PATH_LEN];
        sm_parent(r->path, parent, sizeof(parent));

        char location[DWM_MAX_PATH_LEN];
        sm_fit(location, sizeof(location), sm_basename(parent), remaining);
        dwm_draw_text(SM_TEXT_X + (int)(used + 2) * SM_FONT_W, text_y, location, SM_COLOR_SUBTEXT);
    }
}

// Header subtitle (and the body text while there are no results)
static void sm_status_text(char* out, size_t size) {
    memset(out, '\0', size);

    if (sm_search_len == 0) { strncpy(out, "Applications", size - 1); return; }
    if (sm_no_root)         { strncpy(out, "No user folder to search", size - 1); return; }
    if (sm_searching)       { strncpy(out, "Searching...", size - 1); return; }
    if (sm_result_count == 0) { strncpy(out, "No results", size - 1); return; }

    char num[12];
    sm_u32_to_str(sm_result_count, num);

    if (sm_result_more) strncpy(out, "First ", size - 1);
    strncat(out, num, size - strlen(out) - 1);
    strncat(out, (sm_result_count == 1) ? " result" : " results", size - strlen(out) - 1);
}

// Placeholder for the shutdown icon: a plain red box
static void draw_power_box(uint16_t width) {
    int box_x = sm_power_x(width);
    int box_y = SM_POWER_Y;

    struct Image* icon = (struct Image*)dwm_resource_find("ui_power");

    if (icon != NULL && icon->data != NULL) {
        int16_t icon_x = (int16_t)(box_x + (SM_POWER_SIZE - (int)icon->width)  / 2);
        int16_t icon_y = (int16_t)(box_y + (SM_POWER_SIZE - (int)icon->height) / 2);
        dwm_draw_sprite(icon_x, icon_y, icon);
    } else {
        dwm_draw_rect_filled(box_x, box_y, SM_POWER_SIZE, SM_POWER_SIZE, power_item.bullet_color);
    }
}

static void draw_search_box(uint16_t width) {
    int box_x = SM_ROW_X;
    int box_w = (int)width - (SM_ROW_X * 2);

    dwm_draw_rect_filled(box_x, SM_SEARCH_Y, box_w, SM_SEARCH_H, theme.edit.background);
    dwm_draw_rect(box_x, SM_SEARCH_Y, box_w, SM_SEARCH_H, theme.edit.border);

    int text_x = box_x + SM_SEARCH_PAD;
    int text_y = SM_SEARCH_Y + (SM_SEARCH_H - SM_FONT_H) / 2;
    int cursor_x = text_x;

    if (sm_search_len == 0) {
        dwm_draw_text(text_x + 4, text_y, "Type to search", SM_COLOR_SUBTEXT);
    } else {
        // The cursor sits at the end, so long text shows its tail
        int visible = (box_w - (SM_SEARCH_PAD * 2) - 2) / SM_FONT_W;
        const char* shown = sm_search;
        int shown_len = sm_search_len;
        if (shown_len > visible) {
            shown += shown_len - visible;
            shown_len = visible;
        }
        dwm_draw_text(text_x, text_y, shown, theme.edit.text);
        cursor_x = text_x + shown_len * SM_FONT_W;
    }

    // Two pixel wide cursor, same as the other edit boxes
    dwm_draw_line(cursor_x,     text_y, 0, SM_FONT_H, theme.edit.cursor);
    dwm_draw_line(cursor_x + 1, text_y, 0, SM_FONT_H, theme.edit.cursor);
}

static void start_menu_draw(struct WindowObject* window) {
    uint16_t w = window->w;
    uint16_t h = window->h;

    // Background + left accent strip
    dwm_draw_rect_filled(0, 0, w, h, SM_COLOR_BG);
    dwm_draw_rect_filled(0, 0, SM_ACCENT_W, h, SM_COLOR_ACCENT);

    // Header
    char status[40];
    sm_status_text(status, sizeof(status));

    dwm_draw_rect_filled_gradient_vertical(SM_ACCENT_W, 0, w - SM_ACCENT_W, SM_HEADER_H,
                                           theme.start_menu.header_low, theme.start_menu.header_high);
    dwm_draw_text(SM_TEXT_X - 14, 12, (sm_search_len == 0) ? "Start" : "Search", SM_COLOR_TEXT);
    dwm_draw_text(SM_TEXT_X - 14, 26, status, SM_COLOR_SUBTEXT);
    dwm_draw_line(SM_ACCENT_W, SM_HEADER_H, w - SM_ACCENT_W, 0, SM_COLOR_ACCENT);

    // Main items, or search results while something is typed
    if (sm_search_len == 0) {
        for (uint16_t i = 0; i < SM_MAIN_COUNT; i++) {
            draw_row(w, SM_ITEM_TOP + (i * SM_ROW_H), &main_items[i]);
        }
    } else if (sm_result_count > 0) {
        for (uint32_t i = 0; i < sm_result_count; i++) {
            draw_result_row(w, SM_ITEM_TOP + (i * SM_RESULT_ROW_H), &sm_results[i], i == 0);
        }
    } else {
        dwm_draw_text(SM_TEXT_X - 14, SM_ITEM_TOP + 8, status, SM_COLOR_SUBTEXT);
    }

    // Separator, power box, search box
    dwm_draw_line(SM_ROW_X + 4, SM_SEPARATOR_Y, w - (SM_ROW_X * 2) - 8, 0, SM_COLOR_DIVIDER);
    draw_power_box(w);
    draw_search_box(w);
}

// Returns the item under a window-local point, or NULL
static const StartMenuItem* start_menu_hit_test(struct WindowObject* window, uint16_t x, uint16_t y) {
    int px = sm_power_x(window->w);
    if (x >= px && x < px + SM_POWER_SIZE &&
        y >= SM_POWER_Y && y < SM_POWER_Y + SM_POWER_SIZE) {
        return &power_item;
    }

    if (x < SM_ROW_X || x >= window->w - SM_ROW_X) return NULL;

    // The item rows only exist while the search is empty
    if (sm_search_len == 0 &&
        y >= SM_ITEM_TOP && y < SM_ITEM_TOP + (SM_MAIN_COUNT * SM_ROW_H)) {
        return &main_items[(y - SM_ITEM_TOP) / SM_ROW_H];
    }

    return NULL;
}

// Index of the search result under a window-local point, or -1
static int sm_result_hit_test(struct WindowObject* window, uint16_t x, uint16_t y) {
    if (sm_search_len == 0 || sm_result_count == 0) return -1;
    if (x < SM_ROW_X || x >= window->w - SM_ROW_X) return -1;
    if (y < SM_ITEM_TOP) return -1;

    uint32_t index = (uint32_t)(y - SM_ITEM_TOP) / SM_RESULT_ROW_H;
    return (index < sm_result_count) ? (int)index : -1;
}

// ==========================================
// Keyboard (search box)
// ==========================================
static void start_menu_key(WindowHandle handle, uint32_t key) {
    // Releases and extended keys (arrows, function keys) are ignored
    if (key > 0xFF) return;
    uint8_t ch = (uint8_t)key;

    switch (ch) {
    case SM_KEY_ESCAPE:
        // First Esc clears the search, the next one closes the menu
        if (sm_search_len > 0) {
            sm_search_clear();
            dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        } else {
            dwm_start_menu_close();
        }
        return;

    case SM_KEY_BACKSPACE:
        if (sm_search_len > 0) {
            sm_search[--sm_search_len] = '\0';
            sm_search_changed();
            dwm_window_send_event(handle, DWM_EVENT_REDRAW);
        }
        return;

    case SM_KEY_ENTER:
        // Open the first (highlighted) result
        if (sm_search_len > 0 && sm_result_count > 0) {
            struct SmResult r = sm_results[0];
            dwm_start_menu_close();
            sm_open_result(&r);
        }
        return;
    }

    if (ch < 0x20 || ch > 0x7E) return;
    if (sm_search_len >= SM_SEARCH_MAX) return;

    sm_search[sm_search_len++] = (char)ch;
    sm_search[sm_search_len] = '\0';
    sm_search_changed();
    dwm_window_send_event(handle, DWM_EVENT_REDRAW);
}

// ==========================================
// Event handler
// ==========================================
void callback_start_menu_handler(WindowHandle handle, wEvent event, uint32_t wparam, int32_t lparam) {
    // Only the current, fully open menu reacts to input. A menu that is
    // sliding away (or an old one whose CLOSE is still queued) just draws.
    bool live = (handle == start_menu_handle) && !sm_closing;

    switch (event) {
        case DWM_EVENT_REDRAW: {
            struct WindowObject* window = dwm_get_window_by_id(handle);
            if (window != NULL) start_menu_draw(window);
            break;
        }

        case DWM_EVENT_MOUSE: {
            if (!live) break;

            struct WindowObject* window = dwm_get_window_by_id(handle);
            if (window == NULL) break;

            uint16_t click_x = (uint16_t)(wparam & 0xFFFF);
            uint16_t click_y = (uint16_t)((wparam >> 16) & 0xFFFF);

            // Right click on a result: its context menu
            if (lparam & DWM_STATE_MOUSE_BTN_RIGHT) {
                int index = sm_result_hit_test(window, click_x, click_y);
                if (index >= 0) {
                    sm_ctx_item  = sm_results[index];
                    sm_ctx_valid = true;
                    dwm_summon_context_menu(handle, click_x, click_y, sm_ctx_labels, SM_CTX_COUNT);
                }
                break;
            }

            if (!(lparam & DWM_STATE_MOUSE_BTN_LEFT)) break;

            // Single click on a result opens it
            int index = sm_result_hit_test(window, click_x, click_y);
            if (index >= 0) {
                struct SmResult r = sm_results[index];
                dwm_start_menu_close();
                sm_open_result(&r);
                break;
            }

            const StartMenuItem* item = start_menu_hit_test(window, click_x, click_y);
            if (item != NULL) {
                dwm_start_menu_close();
                if (item->action != NULL) item->action();
            }
            break;
        }

        // An item picked from a result's context menu (wparam = index)
        case DWM_EVENT_CONTEXT_MENU: {
            if (!live || !sm_ctx_valid) break;

            struct SmResult r = sm_ctx_item;
            sm_ctx_valid = false;

            dwm_start_menu_close();
            sm_context_action(&r, wparam);
            break;
        }

        case DWM_EVENT_KEYBOARD:
            if (live) start_menu_key(handle, wparam);
            break;

        // Another window (or the desktop) was clicked. Skipped when the menu
        // has focus again by now: clicking Start while it slides down turns it
        // around, and that click's FOCUS_LOST arrives after the reopen.
        case DWM_EVENT_FOCUS_LOST:
            if (live && dwm_window_get_focus() != handle) dwm_start_menu_close();
            break;

        case DWM_EVENT_DESTROY:
            // The window still exists here; it is freed right after DESTROY
            sm_release_context_menu(handle);

            if (start_menu_handle == handle) {
                start_menu_handle = 0;
                sm_closing = false;
                sm_close_sent = false;
                sm_ctx_valid = false;
                sm_search_clear();      // Also cancels a search still running
            }
            // Pop the taskbar button back up
            dwm_window_send_event(taskbar.window, DWM_EVENT_REDRAW);
            break;

        default:
            break;
    }
}
