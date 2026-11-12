/*
 * prop: the properties of a file, folder or storage device.
 *
 *     prop <path>
 *
 * Installed as <home>/sys/prop; the explorer and the desktop start it for
 * "Properties". The kernel DWM's properties window, as a program:
 *
 *   General       icon, type and name (a file's from its type in the
 *                 registry, hkey_root/filetype, as the explorer shows it); the path (the part inside a device in
 *                 the mount color); size, or for a folder its items and the
 *                 size of everything in it, or for a device how full it is
 *                 with a pie; created / modified
 *   Permissions   readable / writeable / executable. Click a box to change
 *                 it, when allowed: the folder holding the file must be
 *                 writable, and this program needs file write clearance.
 *   Security      who may read and who may write it, by group. On top the
 *                 groups (the group policies, read from the registry,
 *                 hkey_root/system/policy); the file's policy is in bold
 *                 (a file in none shows as user) and a click picks the
 *                 group shown below. Under it "Permissions for <group>":
 *                 Read and Write, each with an Allow box. A group is
 *                 allowed when it holds every bit the file asks for
 *                 (security for reading, security | write_security for
 *                 writing), so allowing a group also allows every group
 *                 holding more: those ticks show greyed, as does system,
 *                 which holds everything. "Modify" asks the user for
 *                 admin (sec_elevate, the desktop's prompt); once allowed,
 *                 a click on an Allow box changes it. Allowing a group
 *                 sets what the file asks for to what the group holds;
 *                 taking it away asks for administration on top (or, for
 *                 admin, for what only system holds). The file's policy
 *                 follows Read: the smallest group allowed to read it.
 *
 * Tab switches tabs; Escape, Enter, Close or the close box end it.
 *
 * Anything slow runs in the background of the event loop, a short step at a
 * time between events, so the window opens at once and keeps answering:
 * adding up a folder's contents, and the kernel counting a device's used
 * space (SYS_FINFO answers "still counting" after a moment and is asked
 * again until it is done).
 *
 * Needs SEC_FILE_READ, SEC_GFX and SEC_IPC (SEC_FILE_WRITE to change
 * permissions, SEC_ELEVATE to modify the security).
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/filetype.h>
#include <os/gfx.h>
#include <os/input.h>
#include <os/os.h>
#include <os/registry.h>
#include <os/resource.h>
#include <os/security.h>
#include <os/syscall.h>
#include <os/wm.h>
#include <os/wm_app.h>     // Not on the desktop: says so and exits before main()

//
// Layout (client coordinates, from the kernel version)
//

#define WIN_W           320
#define WIN_H           420

#define TAB_X           25
#define TAB_Y           5
#define TAB_W           88
#define TAB_H           24
#define TAB_GAP         4
#define TAB_COUNT       3
#define DIVIDER_Y       35

#define ICON_X          24
#define ICON_Y          54
#define ICON_FALLBACK   32
#define LABEL_X         15
#define VALUE_X         90
#define ROW_H           15

#define BOX             12
#define BOX_X           25
#define BOX_LABEL_X     45

#define BUTTON_W        60
#define BUTTON_H        24
#define BUTTON_X        ((WIN_W - (BUTTON_W + BUTTON_W / 2)) + 10)
#define BUTTON_Y        ((WIN_H - BUTTON_H) - BUTTON_H + 7)

#define PIE_X           150
#define PIE_Y           250
#define PIE_R           45
#define PIE_DEPTH       10

// Folder size: stop counting past this many entries or this deep
#define WALK_MAX        20000
#define WALK_DEPTH      32
#define WALK_STEP       64      // Entries looked at per step of the event loop

enum { TAB_GENERAL, TAB_PERMISSIONS, TAB_SECURITY };
static const char* const tab_names[TAB_COUNT] = { "General", "Permissions", "Security" };

// Permission boxes, top to bottom
static const struct { uint32_t bit; const char* label; int32_t y; } perm_boxes[] = {
    { SYS_PERM_READ,    "Readable",   60 },
    { SYS_PERM_WRITE,   "Writeable",  85 },
    { SYS_PERM_EXECUTE, "Executable", 110 },
};
#define PERM_BOXES  (sizeof(perm_boxes) / sizeof(perm_boxes[0]))

// Security tab: the policy list, then the mask list under it
#define LIST_X          LABEL_X
#define LIST_W          (WIN_W - LABEL_X * 2)
#define LIST_ROW        15
#define LIST_PAD        2
#define POLICY_HEAD_Y   44
#define POLICY_LIST_Y   58
#define POLICY_MAX      8           // Rows in the policy list
#define SEC_GAP         10          // Group list to the permissions heading
#define SEC_ROWS        2           // Read, Write
#define SEC_ROW         20
#define SEC_ALLOW_X     230         // The Allow boxes

#define MODIFY_Y        (BUTTON_Y - BUTTON_H - 6)

//
// What is shown
//

enum { WHAT_FILE, WHAT_FOLDER, WHAT_STORAGE };

static struct {
    char        path[PATH_MAX];
    const char* name;
    int         kind;               // SYS_TYPE_*
    SysStat     st;
    os_finfo_t  fi;
    bool        have_fi;            // SYS_FINFO answered (older kernels don't have it)
    size_t      mount_split;        // Path text from here on is inside a device
    bool        in_mount;

    int         what;               // WHAT_*
    char        type[FT_NAME_MAX];  // "Storage", "Folder", or the file's type ("Font", "Program", ...)
    int         icon;               // Resource handle, -1 if none

    uint32_t    items;              // Folder: direct entries
    uint64_t    bytes;              // Folder: everything inside
    bool        partial;            // ... but not every item could be counted
    bool        sized;
    bool        walking;            // Folder: still adding up (see walk_step)
    bool        counting;           // Storage: the kernel is still counting the used space
} P;

// The folder walk in progress: one open directory per level, all sharing
// `path`, which holds the deepest one and is cut back as levels close
static struct {
    DIR*     dirs[WALK_DEPTH + 1];
    size_t   bases[WALK_DEPTH + 1];     // strlen(path) for that level
    int      depth;                     // Open levels, 0 when done
    uint32_t seen;
    char     path[PATH_MAX];
} W;

// The policies the Security tab lists: the registry's hkey_root/system/policy
// keys, by id (the kernel's table, read-only)
#define POLICY_KEY      "hkey_root/system/policy"

typedef struct {
    uint32_t id;
    char     name[OS_REG_NAME_MAX];
    uint32_t permissions;           // What it holds: the bits a file in it may allow
} policy_entry_t;

static policy_entry_t policies[POLICY_MAX];
static size_t         policy_count;
static bool           modifying;    // "Modify" allowed: the lists change the file

static wm_theme_t  theme;
static wm_window_t win;
static int         tab = TAB_GENERAL;
static int32_t     cw = 6, lh = 8;     // Font cell

//
// Formatting
//

// 0 .. 999: "512 (bytes)"; from 1,000: "1.0 (kilobytes)", "2.5 (megabytes)" ...
#define SIZE_BASE   1000u
static const char* const size_units[] = { "bytes", "kilobytes", "megabytes", "gigabytes", "terabytes" };
#define SIZE_UNITS  (sizeof(size_units) / sizeof(size_units[0]))

static void format_size(uint64_t bytes, char* out, size_t size) {
    if (bytes < SIZE_BASE) {
        snprintf(out, size, "%u %s", (unsigned)bytes, bytes == 1 ? "(byte)" : "(bytes)");
        return;
    }
    uint32_t unit = 0;
    uint64_t divisor = 1;
    while (unit + 1 < SIZE_UNITS && bytes >= divisor * SIZE_BASE) {
        divisor *= SIZE_BASE;
        unit++;
    }
    uint64_t tenths = (bytes * 10 + divisor / 2) / divisor;
    if (tenths >= (uint64_t)SIZE_BASE * 10 && unit + 1 < SIZE_UNITS) {     // 999,960 -> "1.0 megabytes"
        divisor *= SIZE_BASE;
        unit++;
        tenths = (bytes * 10 + divisor / 2) / divisor;
    }
    snprintf(out, size, "%u.%u (%s)", (unsigned)(tenths / 10), (unsigned)(tenths % 10), size_units[unit]);
}

// 1234567 -> "1,234,567"
static void format_commas(uint64_t value, char* out, size_t size) {
    char digits[24];
    int n = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
    size_t at = 0;
    for (int i = 0; i < n && at + 1 < size; i++) {
        if (i > 0 && (n - i) % 3 == 0 && at + 1 < size)
            out[at++] = ',';
        if (at + 1 < size)
            out[at++] = digits[i];
    }
    out[at] = '\0';
}

// Seconds since the Unix epoch -> "2026-10-17 21:51" (UTC: the kernel keeps no zone)
static void format_time(char* out, size_t size, uint32_t seconds) {
    int32_t days = (int32_t)(seconds / 86400u);
    uint32_t rest = seconds % 86400u;

    // Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm)
    int32_t z = days + 719468;
    int32_t era = z / 146097;
    int32_t doe = z - era * 146097;
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = yoe + era * 400;
    int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int32_t mp = (5 * doy + 2) / 153;
    int32_t d = doy - (153 * mp + 2) / 5 + 1;
    int32_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;

    snprintf(out, size, "%04d-%02d-%02d %02u:%02u", (int)y, (int)m, (int)d,
             (unsigned)(rest / 3600), (unsigned)(rest % 3600 / 60));
}

//
// Gathering
//

// SYS_STAT directly: the libc's stat() folds device roots into directories
static int kind_of(const char* path, SysStat* st) {
    SysStat local;
    int32_t r = syscall2(SYS_STAT, (uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)(st ? st : &local));
    if (SYS_IS_ERROR(r)) {
        errno = (r == SYS_ENOENT) ? ENOENT : (r == SYS_EACCES) ? EACCES : (r == SYS_EPERM) ? EPERM : EIO;
        return -1;
    }
    return (int)(st ? st : &local)->type;
}

// Open the folder in W.path as the next level down
static bool walk_open(void) {
    if (W.depth > WALK_DEPTH) {
        P.partial = true;
        return false;
    }
    DIR* d = opendir(W.path);
    if (d == NULL) {
        P.partial = true;
        return false;
    }
    W.dirs[W.depth] = d;
    W.bases[W.depth] = strlen(W.path);
    W.depth++;
    return true;
}

static void walk_stop(void) {
    while (W.depth > 0)
        closedir(W.dirs[--W.depth]);
    P.walking = false;
}

static void walk_start(void) {
    strlcpy(W.path, P.path, sizeof(W.path));
    W.depth = 0;
    W.seen = 0;
    P.sized = walk_open();
    P.walking = P.sized;
}

// Look at up to WALK_STEP more entries: files add their size, folders are
// entered (depth first), devices inside (only below "/" or /mnt) are not
// part of this folder's size
static void walk_step(void) {
    for (int n = 0; n < WALK_STEP && W.depth > 0; n++) {
        int top = W.depth - 1;
        size_t base = W.bases[top];
        W.path[base] = '\0';

        struct dirent* e = readdir(W.dirs[top]);
        if (e == NULL) {
            closedir(W.dirs[top]);
            W.depth--;
            continue;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (top == 0)
            P.items++;
        if (++W.seen > WALK_MAX) {
            P.partial = true;
            walk_stop();
            return;
        }
        int len = snprintf(W.path + base, PATH_MAX - base, "%s%s", base > 1 ? "/" : "", e->d_name);
        if (len < 0 || (size_t)len >= PATH_MAX - base) {
            P.partial = true;
            continue;
        }
        SysStat st;
        int kind = kind_of(W.path, &st);
        if (kind == SYS_TYPE_FILE)
            P.bytes += st.size;
        else if (kind == SYS_TYPE_DIRECTORY)
            walk_open();
        else if (kind < 0)
            P.partial = true;
    }
    if (W.depth == 0)
        P.walking = false;
}

// SYS_FINFO again; for a device the kernel counts on where it left off
static void refresh_finfo(void) {
    P.have_fi = (os_finfo(P.path, &P.fi) == 0);
    P.counting = P.have_fi && (P.fi.flags & SYS_FINFO_DEVICE) && (P.fi.flags & SYS_FINFO_COUNTING);
}

static bool background_busy(void) {
    return P.walking || P.counting;
}

static void background_step(void) {
    if (P.walking)
        walk_step();
    if (P.counting)
        refresh_finfo();
}

// Where the path text turns into the mount color: just before the first
// segment that is a device root ("/mnt" + "/ata0p1/docs")
static void find_mount_split(void) {
    size_t len = strlen(P.path);
    P.mount_split = len;
    P.in_mount = false;

    char prefix[PATH_MAX];
    for (size_t i = 2; i <= len; i++) {
        if (P.path[i] != '/' && P.path[i] != '\0')
            continue;
        memcpy(prefix, P.path, i);
        prefix[i] = '\0';
        if (kind_of(prefix, NULL) == SYS_TYPE_MOUNT) {
            P.mount_split = (size_t)(strrchr(prefix, '/') - prefix);
            P.in_mount = true;
            return;
        }
    }
}

// The image `name`, else `fallback` (either may be NULL or ""): -1 if neither is installed
static int open_icon(const char* name, const char* fallback) {
    int handle = (name != NULL && name[0] != '\0') ? resource_open_system(name) : -1;
    if (handle < 0 && fallback != NULL && fallback[0] != '\0')
        handle = resource_open_system(fallback);
    return handle;
}

// The kernel's table, for a registry without the listing (an older kernel)
static void policies_builtin(void) {
    static const sec_mask_t user = SEC_FILE_READ | SEC_FILE_WRITE | SEC_CONSOLE | SEC_GFX |
                                   SEC_IPC | SEC_EXEC | SEC_ELEVATE;
    static const struct { uint32_t id; sec_mask_t permissions; } table[] = {
        { SYS_POLICY_USER,    user },
        { SYS_POLICY_ADMIN,   user | SEC_ADMIN },
        { SYS_POLICY_SYSTEM,  SEC_ALL },
        { SYS_POLICY_NETWORK, user | SEC_NET },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]) && policy_count < POLICY_MAX; i++) {
        policy_entry_t* e = &policies[policy_count++];
        e->id = table[i].id;
        e->permissions = table[i].permissions;
        strlcpy(e->name, os_policy_name(table[i].id), sizeof(e->name));
    }
}

static void load_policies(void) {
    policy_count = 0;
    char name[OS_REG_NAME_MAX], key[PATH_MAX], text[32];
    for (unsigned i = 0; policy_count < POLICY_MAX && os_reg_key(POLICY_KEY, i, name, sizeof(name)) > 0; i++) {
        snprintf(key, sizeof(key), "%s/%s", POLICY_KEY, name);
        if (os_reg_get_string(key, "id", text, sizeof(text)) < 0)
            continue;
        char* end = NULL;
        unsigned long id = strtoul(text, &end, 0);
        if (end == text || id == SYS_POLICY_NONE)
            continue;
        uint32_t permissions = 0;
        if (os_reg_get_string(key, "permissions", text, sizeof(text)) == 0)
            permissions = (uint32_t)strtoul(text, NULL, 0);

        // In order of id
        size_t at = policy_count;
        while (at > 0 && policies[at - 1].id > id) {
            policies[at] = policies[at - 1];
            at--;
        }
        policies[at].id = (uint32_t)id;
        policies[at].permissions = permissions;
        strlcpy(policies[at].name, name, sizeof(policies[at].name));
        policy_count++;
    }
    if (policy_count == 0)
        policies_builtin();
}

// The policy the tab shows as the file's: its own, or user for one in none
// (what every program may use, as a file in no group)
static uint32_t shown_policy(void) {
    return (P.fi.policy == SYS_POLICY_NONE) ? SYS_POLICY_USER : P.fi.policy;
}

static size_t viewed;               // The group the permissions grid shows (index in policies)

// What group i holds; every bit when it is out of range
static sec_mask_t group_bits(size_t i) {
    return (i < policy_count) ? policies[i].permissions : SEC_ALL;
}

// Does group i hold everything `need` asks for?
static bool group_covers(size_t i, sec_mask_t need) {
    return (need & ~group_bits(i)) == 0;
}

// What reading and writing the file ask for
static sec_mask_t read_need(void)  { return P.fi.security; }
static sec_mask_t write_need(void) { return P.fi.security | P.fi.write_security; }

// Allowed only because a group holding less is: shown greyed, and changed
// through that group. System holds everything, so it is always allowed.
static bool group_inherits(size_t i, sec_mask_t need) {
    if (!group_covers(i, need))
        return false;
    if (group_bits(i) == SEC_ALL || (group_bits(i) & SEC_ALL) == SEC_ALL)
        return true;
    for (size_t j = 0; j < policy_count; j++) {
        sec_mask_t bj = group_bits(j), bi = group_bits(i);
        if (j != i && (bj & ~bi) == 0 && bj != bi && group_covers(j, need))
            return true;
    }
    return false;
}

// The smallest group allowed to read with `need`: the policy the file is
// put in. SYS_POLICY_SYSTEM when no listed group holds less.
static uint32_t policy_for(sec_mask_t need) {
    size_t best = policy_count;
    for (size_t i = 0; i < policy_count; i++) {
        if (!group_covers(i, need))
            continue;
        if (best == policy_count || (group_bits(i) & ~group_bits(best)) == 0)
            best = i;
    }
    return (best < policy_count) ? policies[best].id : SYS_POLICY_SYSTEM;
}

// Taking group i away: ask for a bit it lacks that a bigger group holds,
// administration when it lacks that, else what only system holds
static sec_mask_t deny_bits(size_t i) {
    sec_mask_t bits = group_bits(i);
    return (bits & SEC_ADMIN) ? (SEC_ALL & ~bits) : SEC_ADMIN;
}

static int gather(const char* arg) {
    if (os_abspath(arg, P.path, sizeof(P.path)) == NULL)
        return -1;
    P.kind = kind_of(P.path, &P.st);
    if (P.kind < 0)
        return -1;

    const char* slash = strrchr(P.path, '/');
    P.name = (strcmp(P.path, "/") == 0 || slash == NULL) ? P.path : slash + 1;
    refresh_finfo();
    find_mount_split();

    // Devices and folders as the explorer shows them; a file by its type
    // in the registry (hkey_root/filetype, see os/filetype.h), the same
    // lookup the explorer makes for its icons and Type column
    if (P.kind == SYS_TYPE_MOUNT || (P.have_fi && (P.fi.flags & SYS_FINFO_DEVICE))) {
        P.what = WHAT_STORAGE;
        strlcpy(P.type, "Storage", sizeof(P.type));
        P.icon = open_icon("icon_storage", NULL);
    } else if (P.kind == SYS_TYPE_DIRECTORY) {
        P.what = WHAT_FOLDER;
        strlcpy(P.type, "Folder", sizeof(P.type));
        P.icon = open_icon("icon_folder", NULL);
    } else {
        P.what = WHAT_FILE;
        filetype_info_t t;
        filetype_lookup(P.path, &t);
        strlcpy(P.type, t.type[0] != '\0' ? t.type : "File", sizeof(P.type));
        P.icon = open_icon(t.icon, t.fallback);
        if (P.icon < 0)
            P.icon = open_icon("icon_file", NULL);  // Neither image installed
    }

    if (P.what == WHAT_FOLDER)
        walk_start();       // Carried on by the event loop
    return 0;
}

//
// Drawing
//

static gfx_rect_t tab_rect(int i) {
    return gfx_rect(TAB_X + i * (TAB_W + TAB_GAP), TAB_Y, TAB_W, TAB_H);
}

static gfx_rect_t box_rect(size_t i) {
    return gfx_rect(BOX_X, perm_boxes[i].y, BOX, BOX);
}

static gfx_rect_t button_rect(void) {
    return gfx_rect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H);
}

static gfx_rect_t modify_rect(void) {
    return gfx_rect(BUTTON_X, MODIFY_Y, BUTTON_W, BUTTON_H);
}

static gfx_rect_t policy_list_rect(void) {
    return gfx_rect(LIST_X, POLICY_LIST_Y, LIST_W, (int32_t)policy_count * LIST_ROW + LIST_PAD * 2);
}

static gfx_rect_t policy_row_rect(size_t i) {
    gfx_rect_t l = policy_list_rect();
    return gfx_rect(l.x + 1, l.y + LIST_PAD + (int32_t)i * LIST_ROW, l.w - 2, LIST_ROW);
}

static int32_t sec_head_y(void) {
    gfx_rect_t l = policy_list_rect();
    return l.y + l.h + SEC_GAP;
}

static gfx_rect_t sec_list_rect(void) {
    return gfx_rect(LIST_X, sec_head_y() + 16, LIST_W, SEC_ROWS * SEC_ROW + LIST_PAD * 2);
}

static gfx_rect_t sec_row_rect(size_t i) {
    gfx_rect_t l = sec_list_rect();
    return gfx_rect(l.x + 1, l.y + LIST_PAD + (int32_t)i * SEC_ROW, l.w - 2, SEC_ROW);
}

static bool in_rect(gfx_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void text(gfx_surface_t* s, int32_t x, int32_t y, uint32_t color, const char* fmt, ...)
    __attribute__((format(printf, 5, 6)));
static void text(gfx_surface_t* s, int32_t x, int32_t y, uint32_t color, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gfx_draw_text(s, x, y, buf, color);
}

// Bold, the way this font can: drawn twice, a pixel apart
static void text_bold(gfx_surface_t* s, int32_t x, int32_t y, const char* str, uint32_t color) {
    gfx_draw_text(s, x, y, str, color);
    gfx_draw_text(s, x + 1, y, str, color);
}

// Text that fits in `cols` characters ("..." at the end when cut)
static void text_fit(gfx_surface_t* s, int32_t x, int32_t y, uint32_t color, const char* str, size_t cols) {
    size_t len = strlen(str);
    if (len <= cols || cols < 4) {
        gfx_draw_text(s, x, y, str, color);
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%.*s...", (int)(cols - 3), str);
    gfx_draw_text(s, x, y, buf, color);
}

static void checkbox(gfx_surface_t* s, int32_t x, int32_t y, bool on, bool enabled) {
    gfx_draw_rect(s, x, y, BOX, BOX, enabled ? theme.button.border : theme.client.divider_soft);
    if (on)
        gfx_fill_rect(s, x + 2, y + 2, BOX - 4, BOX - 4, enabled ? theme.client.accent : theme.client.text_muted);
}

// Line to line on the path
#define PATH_LINE_H     (lh + 6)

// The path over up to two lines, the part inside a device in the mount color.
// How many lines it took.
static int draw_path(gfx_surface_t* s, int32_t y) {
    size_t cols = (size_t)((WIN_W - LABEL_X * 2) / cw);
    size_t len = strlen(P.path);
    size_t skip = 0;
    if (len > cols * 2) {           // Too long even for two lines: keep the end
        skip = len - (cols * 2 - 3);
        gfx_draw_text(s, LABEL_X, y, "...", theme.client.text_muted);
    }

    // Character by character run: [value color][mount color], wrapped
    size_t col = skip ? 3 : 0;
    int32_t line_y = y;
    int lines = 1;
    for (size_t i = skip; i < len; ) {
        size_t room = cols - col;
        size_t end = i + room;
        if (end > len) end = len;
        // Split the run where the color changes
        if (P.in_mount && i < P.mount_split && end > P.mount_split)
            end = P.mount_split;
        uint32_t color = (P.in_mount && i >= P.mount_split) ? theme.client.text_mount : theme.client.text_value;
        gfx_draw_text_n(s, LABEL_X + (int32_t)col * cw, line_y, P.path + i, end - i, color);
        col += end - i;
        i = end;
        if (col >= cols && i < len) {
            col = 0;
            line_y += PATH_LINE_H;
            lines++;
        }
    }
    return lines;
}

static void draw_general(gfx_surface_t* s) {
    if (P.icon > 0)
        gfx_draw_resource(s, P.icon, ICON_X, ICON_Y);
    else
        gfx_draw_rect(s, ICON_X, ICON_Y, ICON_FALLBACK, ICON_FALLBACK, theme.button.border);

    size_t value_cols = (size_t)((WIN_W - VALUE_X - LABEL_X) / cw);
    gfx_draw_text(s, VALUE_X, 58, P.type, theme.client.text);
    text_fit(s, VALUE_X, 78, theme.client.text_value, P.name, value_cols);

    gfx_draw_text(s, LABEL_X, 110, "Path", theme.client.text);
    // A second path line pushes everything below it down
    int32_t shift = (draw_path(s, 124) - 1) * PATH_LINE_H;

    int32_t y = 150 + shift;
    char buf[64];
    if (P.what == WHAT_STORAGE) {
        if (!P.have_fi || P.fi.device_capacity == 0) {
            gfx_draw_text(s, LABEL_X, y, "Space", theme.client.text);
            gfx_draw_text(s, VALUE_X, y, "Unknown", theme.client.text_muted);
            return;
        }
        uint64_t used  = ((uint64_t)P.fi.device_used_hi << 32) | P.fi.device_used;
        uint64_t total = ((uint64_t)P.fi.device_capacity_hi << 32) | P.fi.device_capacity;
        if (P.counting) {
            gfx_draw_text(s, LABEL_X, y, "Used", theme.client.text);
            text(s, VALUE_X, y, theme.client.text_muted, "Counting... %u%%", (unsigned)P.fi.device_percent);
            y += ROW_H * 2;
            gfx_draw_text(s, LABEL_X, y, "Total", theme.client.text);
            format_commas(total, buf, sizeof(buf));
            gfx_draw_text(s, VALUE_X, y, buf, theme.client.text_value);
            return;
        }
        if (used > total) used = total;
        const char* labels[3] = { "Used", "Free", "Total" };
        uint64_t values[3] = { used, total - used, total };
        for (int i = 0; i < 3; i++, y += ROW_H) {
            gfx_draw_text(s, LABEL_X, y, labels[i], theme.client.text);
            format_commas(values[i], buf, sizeof(buf));
            gfx_draw_text(s, VALUE_X, y, buf, theme.client.text_value);
        }

        uint32_t used_pct = (uint32_t)((used * 100) / total);
        if (used_pct > 100) used_pct = 100;
        uint32_t slices[2] = { used_pct, 100 - used_pct };
        gfx_color_t colors[2] = { theme.interface.pie_disksize_free, theme.interface.pie_disksize_used };
        if (slices[0] == 0 || slices[1] == 0)          // One slice only: draw it alone
            gfx_draw_pie_isometric(s, PIE_X, PIE_Y + shift, PIE_R, PIE_DEPTH, slices[0] ? &slices[0] : &slices[1],
                                   slices[0] ? &colors[0] : &colors[1], 1);
        else
            gfx_draw_pie_isometric(s, PIE_X, PIE_Y + shift, PIE_R, PIE_DEPTH, slices, colors, 2);
        text(s, PIE_X - PIE_R, PIE_Y + shift + PIE_R / 2 + PIE_DEPTH + 10, theme.client.text_muted,
             "%u%% used", (unsigned)used_pct);
        return;
    }

    if (P.what == WHAT_FOLDER) {
        gfx_draw_text(s, LABEL_X, y, "Items", theme.client.text);
        format_commas(P.items, buf, sizeof(buf));
        gfx_draw_text(s, VALUE_X, y, P.sized ? buf : "Unknown", theme.client.text_value);
        y += ROW_H;
        gfx_draw_text(s, LABEL_X, y, "Size", theme.client.text);
        format_size(P.bytes, buf, sizeof(buf));
        gfx_draw_text(s, VALUE_X, y, P.sized ? buf : "Unknown", theme.client.text_value);
        y += ROW_H;
        if (P.walking) {
            gfx_draw_text(s, VALUE_X, y, "Counting...", theme.client.text_muted);
            y += ROW_H;
        } else if (P.partial) {
            gfx_draw_text(s, VALUE_X, y, "(not every item counted)", theme.client.text_muted);
            y += ROW_H;
        }
    } else {
        gfx_draw_text(s, LABEL_X, y, "Size", theme.client.text);
        format_size(P.st.size, buf, sizeof(buf));
        gfx_draw_text(s, VALUE_X, y, buf, theme.client.text_value);
        y += ROW_H;
    }

    y += 10;
    if (P.st.created != 0) {
        format_time(buf, sizeof(buf), P.st.created);
        gfx_draw_text(s, LABEL_X, y, "Created", theme.client.text);
        gfx_draw_text(s, VALUE_X, y, buf, theme.client.text_value);
        y += ROW_H;
    }
    if (P.st.modified != 0) {
        format_time(buf, sizeof(buf), P.st.modified);
        gfx_draw_text(s, LABEL_X, y, "Modified", theme.client.text);
        gfx_draw_text(s, VALUE_X, y, buf, theme.client.text_value);
    }
}

static void draw_permissions(gfx_surface_t* s) {
    for (size_t i = 0; i < PERM_BOXES; i++) {
        gfx_rect_t b = box_rect(i);
        checkbox(s, b.x, b.y, (P.st.permissions & perm_boxes[i].bit) != 0, 1);
        gfx_draw_text(s, BOX_LABEL_X, b.y + 2, perm_boxes[i].label, theme.client.text);
    }
}

static void draw_button(gfx_surface_t* s, gfx_rect_t b, const char* label, bool active) {
    gfx_fill_rect(s, b.x, b.y, b.w, b.h, theme.button.fill);
    gfx_draw_rect(s, b.x, b.y, b.w, b.h, active ? theme.button.border_active : theme.button.border);
    gfx_draw_text(s, b.x + (b.w - (int32_t)strlen(label) * cw) / 2, gfx_text_y(s, b.y, b.h), label,
                  theme.client.text_value);
}

static void list_frame(gfx_surface_t* s, gfx_rect_t r) {
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, theme.button.fill);
    gfx_draw_rect(s, r.x, r.y, r.w, r.h, modifying ? theme.button.border_active : theme.button.border);
}

static void draw_security(gfx_surface_t* s) {
    if (!P.have_fi) {
        gfx_draw_text(s, LABEL_X, 50, "Advanced Security Settings", theme.client.text);
        gfx_draw_text(s, LABEL_X, 70, "This kernel doesn't report a file's", theme.client.text_muted);
        gfx_draw_text(s, LABEL_X, 70 + ROW_H, "security mask.", theme.client.text_muted);
        return;
    }

    // The groups, the file's policy in bold, the one shown below highlighted
    gfx_draw_text(s, LABEL_X, POLICY_HEAD_Y, "Groups", theme.client.text);

    list_frame(s, policy_list_rect());
    bool listed = false;
    for (size_t i = 0; i < policy_count; i++) {
        gfx_rect_t r = policy_row_rect(i);
        int32_t ty = gfx_text_y(s, r.y, r.h);
        bool current = (policies[i].id == shown_policy());
        listed = listed || current;
        if (i == viewed)
            gfx_fill_rect(s, r.x, r.y, r.w, r.h, theme.client.divider_soft);
        if (current) {
            text_bold(s, r.x + 6, ty, policies[i].name, theme.client.text_value);
            gfx_draw_text(s, r.x + r.w - 7 * cw, ty, "policy", theme.client.text_muted);
        } else {
            gfx_draw_text(s, r.x + 6, ty, policies[i].name, theme.client.text);
        }
    }
    if (!listed)    // An id the registry doesn't list
        text(s, LABEL_X + 50, POLICY_HEAD_Y, theme.client.text_muted, "(in policy %u)", (unsigned)P.fi.policy);

    // Read and Write for the group picked above
    int32_t y = sec_head_y();
    text(s, LABEL_X, y, theme.client.text, "Permissions for %s",
         viewed < policy_count ? policies[viewed].name : "?");
    gfx_draw_text(s, SEC_ALLOW_X + BOX / 2 - (5 * cw) / 2, y, "Allow", theme.client.text_muted);

    list_frame(s, sec_list_rect());
    static const char* const rows[SEC_ROWS] = { "Read", "Write" };
    sec_mask_t needs[SEC_ROWS] = { read_need(), write_need() };
    for (size_t i = 0; i < SEC_ROWS; i++) {
        gfx_rect_t r = sec_row_rect(i);
        bool allowed = group_covers(viewed, needs[i]);
        // Greyed: allowed through a smaller group, or Write while Read isn't
        bool fixed = group_inherits(viewed, needs[i]) || (i == 1 && !group_covers(viewed, needs[0]));
        gfx_draw_text(s, r.x + 6, gfx_text_y(s, r.y, r.h), rows[i],
                      fixed ? theme.client.text_muted : theme.client.text);
        checkbox(s, SEC_ALLOW_X, r.y + (r.h - BOX) / 2, allowed, modifying && !fixed);
    }

    gfx_rect_t l = sec_list_rect();
    y = l.y + l.h + 8;
    gfx_draw_text(s, LABEL_X, y, "Read: open, list and run it", theme.client.text_muted);
    y += ROW_H;
    gfx_draw_text(s, LABEL_X, y, "Write: change, add to, delete, rename,", theme.client.text_muted);
    y += ROW_H;
    gfx_draw_text(s, LABEL_X, y, "move it or change its permissions", theme.client.text_muted);
    y += ROW_H;
    sec_mask_t unknown = write_need() & ~SEC_ALL;
    if (unknown != 0) {
        text(s, LABEL_X, y, theme.client.text_muted, "+ unknown bits 0x%x", (unsigned)unknown);
        y += ROW_H;
    }
    draw_button(s, modify_rect(), "Modify", modifying);
}

static void draw(void) {
    gfx_surface_t* s = wm_window_surface(win);
    if (s == NULL)
        return;
    cw = gfx_char_width(s);
    lh = gfx_line_height(s);

    gfx_clear(s, theme.client.background);
    gfx_draw_hline(s, 0, DIVIDER_Y, WIN_W, theme.client.accent);

    if (tab == TAB_GENERAL)          draw_general(s);
    else if (tab == TAB_PERMISSIONS) draw_permissions(s);
    else                             draw_security(s);

    for (int i = 0; i < TAB_COUNT; i++) {
        gfx_rect_t r = tab_rect(i);
        bool active = (i == tab);
        gfx_fill_rect(s, r.x, r.y, r.w, r.h, theme.button.fill);
        gfx_draw_rect(s, r.x, r.y, r.w, r.h, active ? theme.button.border_active : theme.button.border);
        int32_t tw = (int32_t)strlen(tab_names[i]) * cw;
        gfx_draw_text(s, r.x + (r.w - tw) / 2, gfx_text_y(s, r.y, r.h), tab_names[i],
                      active ? theme.client.text : theme.client.text_muted);
    }

    draw_button(s, button_rect(), "Close", false);

    wm_window_commit(win, NULL);
}

//
// Input
//

static void toggle_permission(size_t i) {
    uint32_t perms = (P.st.permissions ^ perm_boxes[i].bit) & (SYS_PERM_READ | SYS_PERM_WRITE | SYS_PERM_EXECUTE);
    if (os_set_permissions(P.path, perms) < 0) {
        wm_notify_ex(WM_NOTIFY_ERROR, "Permissions unchanged", strerror(errno), WM_NOTIFY_DEFAULT_MS);
        return;
    }
    // Read back what the file system now says
    kind_of(P.path, &P.st);
    refresh_finfo();
}

// "Modify": ask the user for admin, unless this program has it already
static void start_modify(void) {
    if (modifying)
        return;
    if (!sec_is_admin()) {
        draw();
        char reason[128];
        snprintf(reason, sizeof(reason), "Change the security of %.80s", P.name);
        if (sec_elevate(reason) < 0) {
            if (errno == EACCES)
                return;         // Cancel at the prompt: the user just chose it
            const char* why = (errno == EPERM)  ? "This program can't ask for administrator permission"
                            : (errno == EBUSY)  ? "Request blocked"
                            :                     strerror(errno);
            wm_notify_ex(WM_NOTIFY_ERROR, "Privilege elevation", why, WM_NOTIFY_DEFAULT_MS);
            return;
        }
    }
    modifying = true;
}

static void security_failed(void) {
    const char* why = (errno == EPERM)  ? "Not cleared for it, or no administrator permission"
                    : (errno == EACCES) ? "Its folder isn't writable, or it can't keep a security mask"
                    : (errno == EBUSY)  ? "A drive's own folder can't be changed"
                    :                     strerror(errno);
    wm_notify_ex(WM_NOTIFY_ERROR, "Privilege elevation", why, WM_NOTIFY_DEFAULT_MS);
}

// Ask for `read` to read and `write` to write (write holds read), keeping
// the policy the smallest group allowed to read. The kernel lets a mask
// change only by bits the file's policy holds, so a change reaching past
// it goes through "system" (every bit) on the way.
static void apply_security(sec_mask_t read, sec_mask_t write) {
    uint32_t target = policy_for(read);
    sec_mask_t old_policy_bits = SEC_ALL;
    for (size_t i = 0; i < policy_count; i++)
        if (policies[i].id == P.fi.policy)
            old_policy_bits = policies[i].permissions;

    bool ok = true;
    if (read != P.fi.security) {
        if (P.fi.policy != SYS_POLICY_NONE && ((read ^ P.fi.security) & ~old_policy_bits) != 0)
            ok = os_set_policy(P.path, SYS_POLICY_SYSTEM) == 0;
        ok = ok && os_set_security(P.path, read) == 0;
    }
    if (ok && target != P.fi.policy)
        ok = os_set_policy(P.path, target) == 0;
    if (ok && (write & ~read) != P.fi.write_security)
        ok = os_set_write_security(P.path, write & ~read) == 0;
    if (!ok)
        security_failed();
    refresh_finfo();
}

static void toggle_allow(size_t row) {
    if (viewed >= policy_count)
        return;
    sec_mask_t read = read_need(), write = write_need();
    sec_mask_t need = (row == 0) ? read : write;
    if (group_inherits(viewed, need) || (row == 1 && !group_covers(viewed, read)))
        return;     // Greyed: changed through another group

    bool allow = !group_covers(viewed, need);
    sec_mask_t bits = group_bits(viewed);
    if (row == 0) {
        read = allow ? (read & bits) : (read | deny_bits(viewed));
        write |= read;                      // Writing never needs less than reading
    } else {
        write = allow ? (write & bits) : (write | deny_bits(viewed));
    }
    apply_security(read, write | read);
}

// True when the window should close
static bool mouse_down(const wm_event_t* ev) {
    if (ev->button != INPUT_BTN_LEFT)
        return false;
    for (int i = 0; i < TAB_COUNT; i++) {
        if (in_rect(tab_rect(i), ev->x, ev->y)) {
            tab = i;
            return false;
        }
    }
    if (tab == TAB_PERMISSIONS) {
        for (size_t i = 0; i < PERM_BOXES; i++) {
            gfx_rect_t b = box_rect(i);
            b.w = BOX_LABEL_X - BOX_X + (int32_t)strlen(perm_boxes[i].label) * cw;    // The label too
            if (in_rect(b, ev->x, ev->y)) {
                toggle_permission(i);
                return false;
            }
        }
    }
    if (tab == TAB_SECURITY && P.have_fi) {
        if (in_rect(modify_rect(), ev->x, ev->y)) {
            start_modify();
            return false;
        }
        for (size_t i = 0; i < policy_count; i++) {
            if (in_rect(policy_row_rect(i), ev->x, ev->y)) {
                viewed = i;             // Only what is shown below
                return false;
            }
        }
        if (modifying) {
            for (size_t i = 0; i < SEC_ROWS; i++) {
                if (in_rect(sec_row_rect(i), ev->x, ev->y)) {
                    toggle_allow(i);
                    return false;
                }
            }
        }
    }
    return in_rect(button_rect(), ev->x, ev->y);
}

static void default_theme(void) {
    theme.client.background = 0xFF141420; theme.client.text = 0xFFFFFFFF;
    theme.client.text_soft = 0xFFD0D0DF;  theme.client.text_value = 0xFF3FFF3F;
    theme.client.text_muted = 0xFF8888AA; theme.client.text_mount = 0xFFFDA008;
    theme.client.text_danger = 0xFFC00404; theme.client.accent = 0xFF04C004;
    theme.client.divider_soft = 0xFF086008;
    theme.button.fill = 0xFF1C1C2A; theme.button.border = 0xFF444466;
    theme.button.border_active = 0xFF04C004; theme.button.text = 0xFF3FFF3F;
}

int main(int argc, char** argv) {
    if (argc < 2 || argv[1] == NULL || argv[1][0] == '\0') {
        wm_notify_ex(WM_NOTIFY_ERROR, "Properties", "Usage: prop <path>", WM_NOTIFY_DEFAULT_MS);
        wm_disconnect();
        return 2;
    }

    if (gather(argv[1]) < 0) {
        char msg[WM_NOTIFY_TEXT_MAX];
        snprintf(msg, sizeof(msg), "%s: %s", argv[1], strerror(errno));
        wm_notify_ex(WM_NOTIFY_ERROR, "No properties", msg, WM_NOTIFY_DEFAULT_MS);
        wm_disconnect();
        return 1;
    }
    if (wm_get_theme(&theme) < 0)
        default_theme();
    load_policies();
    for (size_t i = 0; i < policy_count; i++)
        if (policies[i].id == shown_policy())
            viewed = i;

    char title[WM_TITLE_MAX];
    snprintf(title, sizeof(title), "%s - Properties", P.name);

    // Centered, like the kernel version (frame: 1 px border, 20 px titlebar)
    int32_t sw = 0, sh = 0;
    wm_screen_size(&sw, &sh);
    wm_window_desc_t d = {
        .title = title,
        .x = (sw > WIN_W) ? (sw - WIN_W - 2) / 2 : WM_POS_DEFAULT,
        .y = (sh > WIN_H) ? (sh - WIN_H - 22) / 2 : WM_POS_DEFAULT,
        .width = WIN_W, .height = WIN_H,
    };
    win = wm_window_create(&d);
    if (win == 0) {
        wm_disconnect();
        return 1;
    }
    draw();
    wm_window_focus(win);

    bool done = false;
    wm_event_t ev;
    while (!done) {
        // With work left, just look for an event; without, wait for one
        int got = wm_wait_event(&ev, background_busy() ? 0 : -1);
        if (got < 0)
            break;
        if (got == 0) {
            if (!background_busy())
                break;
            background_step();
            if (tab == TAB_GENERAL || !background_busy())
                draw();
            continue;
        }
        switch (ev.type) {
            case WM_EVENT_CLOSE:
            case WM_EVENT_DESTROYED:
                done = true;
                break;
            case WM_EVENT_KEY:
                if (ev.key == KEY_ESCAPE || ev.key == KEY_ENTER || ev.key == '\r' || ev.key == '\n') {
                    done = true;
                } else if (ev.key == KEY_TAB) {
                    tab = (ev.mods & INPUT_MOD_SHIFT) ? (tab + TAB_COUNT - 1) % TAB_COUNT : (tab + 1) % TAB_COUNT;
                    draw();
                }
                break;
            case WM_EVENT_MOUSE_DOWN:
                done = mouse_down(&ev);
                if (!done)
                    draw();
                break;
            case WM_EVENT_THEME_CHANGED:
                if (wm_get_theme(&theme) == 0)
                    draw();
                break;
        }
    }

    walk_stop();
    wm_disconnect();
    return 0;
}
