/*
 * User-space desktop on the DWM (dwm/).
 *
 * The desktop shows, top-left first:
 *
 *   - an "icon_storage" icon for every device under /mnt
 *   - an icon for every file and folder in <home>/usr/desktop
 *
 * Drives come and go (USB sticks): /mnt is looked at every DRIVE_POLL_MS,
 * and when a drive appears or disappears a notification says so and every
 * program gets WM_EVENT_FS_CHANGED, the desktop included, which re-reads
 * its icons. On the same timer, a USB device plugged in that no driver
 * took gets a notification too (usbwatch.h).
 *
 * A file's icon comes from the system's file types (os/filetype.h): its
 * extension's key under hkey_root/filetype ("opensans.ttf" -> filetype/ttf,
 * "icon_font"), else what its first bytes say it is (a program is
 * filetype/elf, "icon_executable"; a driver filetype/drv, "icon_driver").
 * A type whose image isn't installed uses its fallback, then "icon_file".
 *
 * Each icon's userdata (dwm_icon_user) is a malloc'd copy of the absolute
 * path it stands for. Links and other special entries are skipped for now.
 *
 * Icons stay where they were left: positions are kept in memory as icons
 * move (a refresh or a rename keeps them too) and written to
 * <home>/usr/icons (layout.h) only when the desktop quits, then read back
 * at start-up. Icons with a saved place go down
 * first; new ones then take the free grid spots around them.
 *
 *   double-click / Enter   open the icon: a drive or folder in
 *                          explorer, a file whose type names an opener
 *                          with it (os/filetype.h: a ".sh" script in
 *                          "console --script"), a program marked
 *                          executable by running it, any other file in
 *                          notepad (helper
 *                          programs are looked for in <home>/sys, then
 *                          <home>/bin: os_programpath)
 *   right-click            context menu: on the desktop or an icon, the
 *                          lines in the registry (menus.h, built-in
 *                          defaults without them); on a window, window_items
 *                          below.
 *                          Cut / Copy put the selected icons on the clipboard
 *                          as files; Paste and Delete run <home>/sys/fileop
 *                          (its progress window, Delete asks first), and the
 *                          desktop re-reads its icons when fileop says it
 *                          changed something (DWM_EVENT_FS_CHANGED).
 *                          New folder / New file make one where the menu
 *                          opened ("new_folder", "new_folder (1)", ... as
 *                          the explorer names them) and start renaming it;
 *                          Rename edits the name in place (Enter keeps,
 *                          Escape cancels, a click elsewhere keeps);
 *                          Open console here starts <home>/bin/console in
 *                          the desktop folder, or in the folder or drive
 *                          right-clicked; Unmount (on a drive) writes out
 *                          everything cached for it and unmounts it, so a
 *                          USB stick can be pulled out safely
 *   drag and drop          (dwm/drag.h) icons let go on a folder or drive
 *                          icon, or files dropped there from a window, move
 *                          into it; icons let go over a window go to that
 *                          window as a drop (an explorer window moves them
 *                          into its folder); files dropped on the desktop
 *                          itself move into <home>/usr/desktop, the first
 *                          icon landing where they were let go. Ctrl held
 *                          when letting go copies instead; a drag with the
 *                          right button asks (Move here / Copy here /
 *                          Cancel) where it was let go. fileop does the
 *                          work, with its progress window. Drives are never
 *                          moved.
 *   taskbar                (dwm/taskbar.h) Start opens the start menu: the
 *                          programs in <home>/sys and <home>/bin (helpers like fileop left
 *                          out), the desktop folder, the settings folder
 *                          (<home>/sys/settings, its programs open from
 *                          there), the games folder (<home>/games), the
 *                          home drive and the drives; the shut down button
 *                          ("ui_shutdown") over the right end of the search
 *                          box; typing searches every
 *                          drive for files and folders by name, and a
 *                          result opens as its icon would. Window buttons
 *                          bring windows to the front or minimize them.
 *   Alt+Tab                the open windows, front first: Tab moves on
 *                          (Shift+Tab back), letting go of Alt brings the
 *                          picked one to the front (restored if minimized)
 *   Alt+F4                 close the focused window
 *
 * The background is a color or a wallpaper, as the registry says
 * (wallpaper.h, os/desktop.h); <home>/sys/settings/desktop changes
 * it and tells the desktop (DWM_EVENT_SETTINGS_CHANGED), which reads it
 * again.
 *
 * Services start with the desktop: the ones the registry lists under
 * hkey_root/system/services (services.h), which only an admin can change;
 * today the Wi-Fi and volume icons on the taskbar, <home>/sys/services/netsrv
 * and audiosrv. Each
 * gets only the permissions it needs (service_grants below).
 *
 * The desktop quits only from the start menu's shut down button, never
 * from a key or a context menu.
 *
 * Needs SEC_FILE_READ, SEC_GFX, SEC_GFX_SCREEN and SEC_INPUT (SEC_EXEC to
 * open icons). Problems show as notifications (dwm/notify.h), never on the
 * console: the desktop is drawn over it. If the DWM can't start at all,
 * the exit code is the errno value (the kernel logs it on COM1).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>          // snprintf only: nothing is printed
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/disk.h>
#include <os/input.h>
#include <os/os.h>
#include <os/filetype.h>
#include <os/process.h>
#include <os/security.h>

#include "dwm/dwm.h"
#include "dwm/consent.h"   // consent_ask_launch: the desktop's own elevated launches
#include "layout.h"
#include "menus.h"
#include "services.h"
#include "usbwatch.h"
#include "wallpaper.h"
#include "resources/imagelib.h"
#include "resources/fontlib.h"

#define MNT_DIR         "/mnt"

// Drives plugged in or pulled out: /mnt checked this often
#define DRIVE_POLL_MS   1000
#define TIMER_DRIVES    1           // Desktop timer id
#define DRIVES_MAX      32
#define DESKTOP_SUB     "usr/desktop"       // Under the home device
#define SETTINGS_SUB    "settings"          // <home>/sys/settings: the settings programs (start menu "Settings")
#define GAMES_SUB       "games"             // <home>/games: the games (start menu "Games")

#define IMAGE_DRIVE     "icon_storage"
#define IMAGE_FOLDER    "icon_folder"
#define IMAGE_FILE      "icon_file"

// Programs that open icons, in <home>/sys or <home>/bin
#define PROGRAM_EXPLORER    "explorer"
#define PROGRAM_NOTEPAD     "notepad"
#define PROGRAM_PROMPT      "console"     // PROGRAM_CONSOLE is a program kind (os/process.h)
#define PROGRAM_PROP        "prop"          // Properties of a file, folder or drive
#define PROGRAM_FILEOP      "fileop"        // Copy / move / delete with a progress window
#define PROGRAM_FORMAT      "format"        // Erase a drive and give it an empty file system

// What New folder / New file call what they make (then "name (1)", ...)
#define NEW_FOLDER_NAME     "new_folder"
#define NEW_FILE_NAME       "new_file"

// Paths handed to one fileop (its argv also holds the program, the verb and -i)
#define FILEOP_MAX_PATHS    (SPAWN_ARG_MAX - 3)
// Source / destination pairs for one "fileop move -r" (or copy)
#define FILEOP_MAX_PAIRS    ((SPAWN_ARG_MAX - 3) / 2)

// After Paste the first new icon lands where the menu opened, if it shows
// up within this long (fileop reports when it is done)
#define PASTE_ORIGIN_MS     60000

// What a program started from the desktop may hold. The dwm itself has
// admin, input and the raw screen; nothing it starts should. (Masked with
// our own as well: spawn_ex refuses bits we don't hold.)
#define LAUNCH_MASK     (SEC_FILE | SEC_GFX | SEC_IPC | SEC_EXEC | SEC_ELEVATE)    // The "user" policy

// Services: programs started with the desktop (services.h lists where
// they come from), each with exactly what it needs. By program file name;
// one not listed here gets SERVICE_DEFAULT_GRANT: it may read files, draw
// and talk over IPC, never write files, start programs, use the network or
// ask for admin.
//
// The network tray reads /dev/wlanN (network) and draws its icon and list
// (gfx, ipc); it can't start programs, write files or ask for admin, so the
// network bit stays with it.
#define SERVICE_DEFAULT_GRANT   (SEC_FILE_READ | SEC_GFX | SEC_IPC)

static const struct {
    const char* program;
    sec_mask_t  security;
} service_grants[] = {
    { "netsrv", SEC_FILE_READ | SEC_GFX | SEC_IPC | SEC_NET | SEC_NET_CONFIG },
    { "audiosrv", SEC_FILE_READ | SEC_FILE_WRITE | SEC_GFX | SEC_IPC },   // Registry: the level kept
};

// Built in, for a registry without hkey_root/system/services: the same
// line "boot install" and "boot repair" write
// (kernel/console/commands/boot_registry.h)
static const services_entry_t services_defaults[] = {
    { "audiosrv", "sys/services/audiosrv" },
    { "netsrv", "sys/services/netsrv" },
};

//
// Directory listing: regular files and folders, sorted
//

typedef struct {
    char          name[256];                // = struct dirent's d_name
    unsigned char type;                     // DT_DIR or DT_REG
} entry_t;

// Folders first, then by name (case-insensitive)
static int compare_entries(const void* a, const void* b) {
    const entry_t* x = (const entry_t*)a;
    const entry_t* y = (const entry_t*)b;
    if (x->type != y->type)
        return (x->type == DT_DIR) ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

// The entries of `dir` in *out (free() it). Returns how many; 0 with a
// message when the folder can't be read.
static size_t list_dir(const char* dir, entry_t** out) {
    *out = NULL;
    DIR* d = opendir(dir);
    if (d == NULL) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "Can't read folder", "%s: %s", dir, strerror(errno));
        return 0;
    }

    entry_t* list = NULL;
    size_t count = 0, capacity = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR && e->d_type != DT_REG)
            continue;       // Links and anything unusual: not yet
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        if (count == capacity) {
            size_t grow = capacity ? capacity * 2 : 16;
            entry_t* p = (entry_t*)realloc(list, grow * sizeof(entry_t));
            if (p == NULL) {
                dwm_notifyf(DWM_NOTIFY_ERROR, "Out of memory", "Some of %s not shown", dir);
                break;
            }
            list = p;
            capacity = grow;
        }
        strlcpy(list[count].name, e->d_name, sizeof(list[count].name));
        list[count].type = e->d_type;
        count++;
    }
    closedir(d);

    if (count > 1)
        qsort(list, count, sizeof(entry_t), compare_entries);
    *out = list;
    return count;
}

//
// Icons
//

// Icons are gathered first and placed afterwards (place_icons), so the ones
// with a saved position can claim it before new ones fill the free spots
typedef struct {
    const char* image;
    char*       path;               // malloc'd; becomes the icon's userdata
} pending_icon_t;

static pending_icon_t pending[DWM_MAX_ICONS];
static size_t         pending_count;

// Where the next new icon goes after a Paste (see PASTE_ORIGIN_MS)
static bool           paste_origin;
static int32_t        paste_x, paste_y;
static uint64_t       paste_time;

// The image for a file: its type from the registry (hkey_root/filetype, by
// extension, then by what its header says) or the built-in table
// (os/filetype.h); the type's icon when it is installed, else its
// fallback, else the plain file icon. The name returned is the image
// library's own copy, so it outlives the lookup.
static const char* file_image(const char* path) {
    filetype_info_t t;
    filetype_lookup(path, &t);
    const image_t* img = imagelib_get(t.icon);
    if (img == NULL && t.fallback[0] != '\0')
        img = imagelib_get(t.fallback);
    return (img != NULL) ? img->name : IMAGE_FILE;
}

// Queue an icon for <dir>/<name>. image NULL: a file, typed by its contents.
static void queue_icon(const char* image, const char* dir, const char* name) {
    if (pending_count == DWM_MAX_ICONS) {
        // One message for the lot: duplicates merge into the one showing
        dwm_notify("Desktop full", "Some icons are not shown", DWM_NOTIFY_DEFAULT_MS);
        return;
    }

    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "Icon skipped", "%s: path too long", name);
        return;
    }
    if (image == NULL)
        image = file_image(path);

    char* copy = strdup(path);
    if (copy == NULL) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "Out of memory", "Icon %s not shown", name);
        return;
    }
    pending[pending_count].image = image;
    pending[pending_count].path  = copy;
    pending_count++;
}

// Add one queued icon at (x, y) (DWM_POS_DEFAULT: the next free spot).
// Takes the path over either way.
static void add_icon(const pending_icon_t* p, int32_t x, int32_t y) {
    const char* name = strrchr(p->path, '/');
    name = (name != NULL) ? name + 1 : p->path;

    dwm_icon_t icon = dwm_icon_add(name, p->image, x, y);
    if (icon == 0) {
        dwm_notify("Desktop full", "Some icons are not shown", DWM_NOTIFY_DEFAULT_MS);
        free(p->path);
        return;
    }
    dwm_icon_set_user(icon, p->path);
    dwm_icon_set_path(icon, p->path);       // For the drag plate's Move / Copy
    // Folders and drives take what is dropped on them
    if (strcmp(p->image, IMAGE_FOLDER) == 0 || strcmp(p->image, IMAGE_DRIVE) == 0)
        dwm_icon_set_drop_target(icon, true);
}

// Saved places first, then everything new on the free grid spots
static void place_icons(void) {
    bool placed[DWM_MAX_ICONS] = { false };
    for (size_t i = 0; i < pending_count; i++) {
        int32_t x, y;
        if (layout_find(pending[i].path, &x, &y)) {
            add_icon(&pending[i], x, y);
            placed[i] = true;
        }
    }
    for (size_t i = 0; i < pending_count; i++) {
        if (placed[i])
            continue;
        if (paste_origin && dwm_time_ms() - paste_time < PASTE_ORIGIN_MS) {
            // Pasted at the menu's spot (kept on the screen)
            int32_t x = paste_x, y = paste_y;
            if (x > dwm_screen_width() - DWM_ICON_WIDTH)   x = dwm_screen_width() - DWM_ICON_WIDTH;
            if (y > dwm_work_area().h - DWM_ICON_GRID_Y)   y = dwm_work_area().h - DWM_ICON_GRID_Y;
            add_icon(&pending[i], x < 0 ? 0 : x, y < 0 ? 0 : y);
            paste_origin = false;
        } else {
            add_icon(&pending[i], DWM_POS_DEFAULT, DWM_POS_DEFAULT);
        }
    }
    pending_count = 0;
}

// One icon per device mounted under /mnt
static void add_drives(void) {
    entry_t* list;
    size_t count = list_dir(MNT_DIR, &list);
    for (size_t i = 0; i < count; i++) {
        if (list[i].type == DT_DIR)     // Device roots report as directories
            queue_icon(IMAGE_DRIVE, MNT_DIR, list[i].name);
    }
    free(list);
}

//
// Drives coming and going
//

// The drives under /mnt at the last look, sorted (list_dir)
static char   known_drives[DRIVES_MAX][256];
static size_t known_drive_count = 0;

static size_t read_drives(char (*names)[256]) {
    entry_t* list;
    size_t count = list_dir(MNT_DIR, &list);
    size_t n = 0;
    for (size_t i = 0; i < count && n < DRIVES_MAX; i++) {
        if (list[i].type == DT_DIR)
            strlcpy(names[n++], list[i].name, sizeof(names[0]));
    }
    free(list);
    return n;
}

static bool drive_listed(char (*names)[256], size_t count, const char* name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0)
            return true;
    }
    return false;
}

static void drives_remember(void) {
    known_drive_count = read_drives(known_drives);
}

static void desktop_sync(void);

// Timer: say which drives came or went, and have everyone re-read. After
// an unmount (`announce` false) the drives that went say nothing: Unmount
// has said "Safe to remove" for them.
static void drives_check(bool announce) {
    static char now[DRIVES_MAX][256];
    size_t count = read_drives(now);

    bool changed = false;
    for (size_t i = 0; i < count; i++) {
        if (!drive_listed(known_drives, known_drive_count, now[i])) {
            dwm_notifyf(DWM_NOTIFY_INFO, "Drive connected", "%s", now[i]);
            changed = true;
        }
    }
    for (size_t i = 0; i < known_drive_count; i++) {
        if (!drive_listed(now, count, known_drives[i])) {
            if (announce)
                dwm_notifyf(DWM_NOTIFY_INFO, "Drive removed", "%s", known_drives[i]);
            changed = true;
        }
    }
    if (!changed)
        return;

    memcpy(known_drives, now, sizeof(now[0]) * count);
    known_drive_count = count;
    desktop_sync();         // The desktop's icons (it gets no FS_CHANGED of its own)
    dwm_fs_changed();       // Any open explorer windows
}

// One icon per file or folder in <home>/usr/desktop
static void add_desktop_items(void) {
    // No home disk: dwm_init() has already said so
    char home[PATH_MAX];
    if (os_gethome(home, sizeof(home)) == NULL)
        return;

    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/%s", home, DESKTOP_SUB);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        dwm_notify_ex(DWM_NOTIFY_ERROR, "No desktop folder", "Path too long", DWM_NOTIFY_LONG_MS);
        return;
    }

    dwm_set_drop_folder(DWM_DESKTOP, dir);      // Files dropped on the desktop land here
    entry_t* list;
    size_t count = list_dir(dir, &list);
    for (size_t i = 0; i < count; i++)
        queue_icon(list[i].type == DT_DIR ? IMAGE_FOLDER : NULL, dir, list[i].name);
    free(list);
}

// Every icon gone, paths freed
static void remove_icons(void) {
    while (dwm_icon_count() > 0) {
        dwm_icon_t icon = dwm_icon_at_index(0);
        free(dwm_icon_user(icon));
        dwm_icon_remove(icon);
    }
}

static void load_icons(void) {
    add_drives();
    add_desktop_items();
    place_icons();
}

// The showing icon for `path`, or 0
static dwm_icon_t icon_for(const char* path);

// The queued icons are exactly the ones showing (same paths)
static bool pending_matches_icons(void) {
    if (pending_count != dwm_icon_count())
        return false;
    for (size_t i = 0; i < pending_count; i++) {
        if (icon_for(pending[i].path) == 0)
            return false;
    }
    return true;
}

// Same icons: a file whose contents changed type gets its new image
static void update_images(void) {
    for (size_t i = 0; i < pending_count; i++) {
        dwm_icon_t icon = icon_for(pending[i].path);
        if (icon != 0 && strcmp(dwm_icon_image(icon), pending[i].image) != 0)
            dwm_icon_set_image(icon, pending[i].image);
    }
}

// A program changed files (fileop copied, moved or deleted something):
// re-read the drives and the desktop folder, and rebuild the icons only if
// something came or went, keeping everyone else where they are
static void desktop_sync(void) {
    add_drives();
    add_desktop_items();
    if (pending_matches_icons()) {
        update_images();
        for (size_t i = 0; i < pending_count; i++)
            free(pending[i].path);
        pending_count = 0;
        return;
    }
    layout_capture();
    remove_icons();
    place_icons();
}

// Start <program> from <home>/sys or <home>/bin with `count` arguments after
// argv[0], in `cwd`
static pid_t launch_args(const char* program, const char* const* extra, size_t count, const char* cwd) {
    char path[PATH_MAX];
    if (os_programpath(program, path, sizeof(path)) == NULL)
        return -1;
    if (count > SPAWN_ARG_MAX - 1) {
        errno = E2BIG;
        return -1;
    }
    char* args[SPAWN_ARG_MAX + 1];
    args[0] = path;
    for (size_t i = 0; i < count; i++)
        args[1 + i] = (char*)extra[i];
    args[1 + count] = NULL;
    return spawn_ex(path, args, cwd, sec_get() & LAUNCH_MASK);
}

static pid_t launch_system(const char* program, const char* arg, const char* cwd) {
    return launch_args(program, &arg, 1, cwd);
}

// Say why a program didn't start
static void launch_failed(const char* what, const char* program) {
    dwm_notifyf(DWM_NOTIFY_ERROR, what, "%s: %s", program,
                errno == EPERM ? "not allowed to start programs" : strerror(errno));
}

// The folder holding `path` ("/mnt/x/a.txt" -> "/mnt/x", "/a" -> "/")
static void parent_dir(const char* path, char* out, size_t size) {
    strlcpy(out, path, size);
    char* slash = strrchr(out, '/');
    if (slash == NULL)
        strlcpy(out, "/", size);
    else if (slash == out)
        out[1] = '\0';
    else
        *slash = '\0';
}

// Say why a program didn't start; nothing when the user said no at the
// administrator prompt
static void run_failed(const char* name, int err) {
    if (err == ECANCELED)
        return;
    if (err == EPERM)
        dwm_notifyf(DWM_NOTIFY_ERROR, "Can't open", "%s: not allowed to start programs", name);
    else
        dwm_notifyf(DWM_NOTIFY_ERROR, "Can't run", "%s: %s", name, strerror(err));
}

// A windowed program whose file needs admin, waiting for the user's answer
// at the administrator prompt (consent_ask_launch)
typedef struct {
    char path[PATH_MAX];
    char dir[PATH_MAX];
} elevated_launch_t;

static void elevated_answer(bool allow, void* ctx) {
    elevated_launch_t* l = (elevated_launch_t*)ctx;
    if (allow) {
        // We hold every bit: the kernel doesn't ask again, and the program
        // starts with what its file needs added to the usual launch mask
        char* args[] = { l->path, NULL };
        if (spawn_program(l->path, args, l->dir, (sec_get() & LAUNCH_MASK) | SPAWN_ELEVATE) < 0) {
            const char* name = strrchr(l->path, '/');
            run_failed(name != NULL ? name + 1 : l->path, errno);
        }
    }
    free(l);
}

// Start a windowed program that needs more than a user program holds
// (admin, network, ...) once the user allows it. True when that is how it
// goes (or why not was said); false: start it as usual.
static bool open_elevated(const char* path, const char* dir, const char* name) {
    sec_mask_t mask = sec_get() & LAUNCH_MASK;
    os_finfo_t info;
    if (program_needs_elevation(path, mask) != 1 || os_finfo(path, &info) < 0)
        return false;
    // A console program's window asks for it itself, naming the program
    if (program_kind(path) != PROGRAM_WINDOWED)
        return false;

    elevated_launch_t* l = (elevated_launch_t*)malloc(sizeof(*l));
    if (l == NULL) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "Can't run", "%s: out of memory", name);
        return true;
    }
    strlcpy(l->path, path, sizeof(l->path));
    strlcpy(l->dir, dir, sizeof(l->dir));
    if (!consent_ask_launch(l->path, info.security & ~mask, elevated_answer, l)) {
        int err = errno;
        free(l);
        dwm_notifyf(DWM_NOTIFY_ERROR, "Can't run", "%s: %s", name,
                    err == EBUSY ? "another program is waiting for administrator permission"
                                 : "administrator permission can't be asked for");
    }
    return true;
}

// Open a file, folder or drive the way its icon would be
static void open_path(const char* path) {
    const char* name = strrchr(path, '/');
    name = (name != NULL && name[1] != '\0') ? name + 1 : path;

    struct stat st;
    if (stat(path, &st) < 0) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "Can't open", "%s: %s", name, strerror(errno));
        return;
    }

    pid_t pid;
    const char* how;
    if (S_ISDIR(st.st_mode)) {
        // Drives report as directories too
        how = PROGRAM_EXPLORER;
        pid = launch_system(PROGRAM_EXPLORER, path, path);
    } else {
        char dir[PATH_MAX];
        parent_dir(path, dir, sizeof(dir));
        filetype_info_t type;
        if (filetype_lookup(path, &type) == 0 && type.open[0] != '\0' &&
            ((st.st_mode & S_IXUSR) || type.viewer)) {
            // Its type names a program that opens it. One that runs it (a
            // script: the console) only when it is marked executable, if
            // not notepad below; a viewer (an image: paint) always.
            how = "run";
            pid = filetype_open(&type, path, dir, sec_get() & LAUNCH_MASK);
        } else if (st.st_mode & S_IXUSR) {
            // Needs admin: the administrator prompt first
            if (open_elevated(path, dir, name))
                return;
            // A console program gets a console window (os/process.h)
            how = "run";
            char* args[] = { (char*)path, NULL };
            pid = spawn_program(path, args, dir, sec_get() & LAUNCH_MASK);
        } else {
            how = PROGRAM_NOTEPAD;
            pid = launch_system(PROGRAM_NOTEPAD, path, dir);
        }
    }

    // Success needs no message: the program's window says it
    if (pid < 0) {
        if (strcmp(how, "run") == 0)
            run_failed(name, errno);
        else if (errno == EPERM)
            dwm_notifyf(DWM_NOTIFY_ERROR, "Can't open", "%s: not allowed to start programs", name);
        else
            dwm_notifyf(DWM_NOTIFY_ERROR, "Can't open", "%s: %s won't start (%s)", name, how, strerror(errno));
    }
}

static void open_icon(dwm_icon_t icon) {
    const char* path = (const char*)dwm_icon_user(icon);
    if (path != NULL)
        open_path(path);
}

//
// Context menus. The desktop's two (the desktop itself, an icon) come from
// the registry (menus.h): each line names one of menu_commands below. The
// window menu and the drop menu are tables here. An item runs its action
// when chosen; `state` (optional) can hide, disable or check it depending
// on where the menu was opened.
//

// Desktop

static uint32_t only_on_icon(const dwm_menu_context_t* ctx) {
    return ctx->icon != 0 ? 0 : DWM_MENU_HIDDEN;
}

// A drive's icon (/mnt/<device>): it can be copied from, never cut or deleted
static bool is_drive(const char* path) {
    return path != NULL && strncmp(path, MNT_DIR "/", sizeof(MNT_DIR)) == 0 &&
           strchr(path + sizeof(MNT_DIR), '/') == NULL;
}

// The selected icons' paths, drives left out when `files_only`. How many.
static size_t selected_paths(const char** paths, size_t max, bool files_only) {
    dwm_icon_t selected[DWM_MAX_ICONS];
    size_t n = dwm_icon_selection(selected, DWM_MAX_ICONS), count = 0;
    for (size_t i = 0; i < n && i < DWM_MAX_ICONS && count < max; i++) {
        const char* path = (const char*)dwm_icon_user(selected[i]);
        if (path != NULL && !(files_only && is_drive(path)))
            paths[count++] = path;
    }
    return count;
}

// Cut / Delete: on an icon, dimmed when only drives are selected
static uint32_t only_on_files(const dwm_menu_context_t* ctx) {
    if (ctx->icon == 0)
        return DWM_MENU_HIDDEN;
    const char* paths[1];
    return selected_paths(paths, 1, true) > 0 ? 0 : DWM_MENU_DISABLED;
}

// Paste: on the desktop itself, when the clipboard holds files
static uint32_t only_with_files(const dwm_menu_context_t* ctx) {
    if (ctx->icon != 0)
        return DWM_MENU_HIDDEN;
    dwm_clip_info_t info;
    return (dwm_clip_info(&info) && info.type == DWM_CLIP_FILES) ? 0 : DWM_MENU_DISABLED;
}

// <home>/usr/desktop into out. False when there is no home disk.
static bool desktop_dir(char* out, size_t size) {
    char home[PATH_MAX];
    if (os_gethome(home, sizeof(home)) == NULL)
        return false;
    int n = snprintf(out, size, "%s/%s", home, DESKTOP_SUB);
    return n >= 0 && (size_t)n < size;
}

static void desktop_open(const dwm_menu_context_t* ctx) {
    (void)ctx;
    dwm_icon_t selected[DWM_MAX_ICONS];
    size_t n = dwm_icon_selection(selected, DWM_MAX_ICONS);
    for (size_t i = 0; i < n && i < DWM_MAX_ICONS; i++)
        open_icon(selected[i]);
}

// Edit: on a file icon (not a folder or drive), shown only there
static uint32_t only_on_file(const dwm_menu_context_t* ctx) {
    if (ctx->icon == 0)
        return DWM_MENU_HIDDEN;
    const char* path = (const char*)dwm_icon_user(ctx->icon);
    struct stat st;
    if (path == NULL || is_drive(path) || stat(path, &st) < 0 || S_ISDIR(st.st_mode))
        return DWM_MENU_HIDDEN;
    return 0;
}

// The selected files in notepad, whatever their type (folders and drives
// left out), one window each
static void desktop_edit(const dwm_menu_context_t* ctx) {
    (void)ctx;
    const char* paths[DWM_MAX_ICONS];
    size_t count = selected_paths(paths, DWM_MAX_ICONS, true);
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        if (stat(paths[i], &st) < 0 || S_ISDIR(st.st_mode))
            continue;
        char dir[PATH_MAX];
        parent_dir(paths[i], dir, sizeof(dir));
        if (launch_system(PROGRAM_NOTEPAD, paths[i], dir) < 0) {
            launch_failed("Can't edit", PROGRAM_NOTEPAD);
            return;
        }
    }
}

// The selected icons' paths onto the system clipboard, as files: an
// explorer window (or any program) can paste them
static void clip_selection(bool cut) {
    const char* paths[DWM_MAX_ICONS];
    size_t count = selected_paths(paths, DWM_MAX_ICONS, cut);    // Drives can't be cut
    if (count > 0 && !dwm_clip_set_files(paths, count, cut))
        dwm_notifyf(DWM_NOTIFY_ERROR, cut ? "Not cut" : "Not copied", "%s", strerror(errno));
}

static void desktop_cut(const dwm_menu_context_t* ctx) {
    (void)ctx;
    clip_selection(true);
}

static void desktop_copy(const dwm_menu_context_t* ctx) {
    (void)ctx;
    clip_selection(false);
}

// The next new icon goes around (x, y), screen (see PASTE_ORIGIN_MS)
static void new_icons_near(int32_t x, int32_t y) {
    paste_origin = true;
    paste_x = x - DWM_ICON_WIDTH / 2;
    paste_y = y - DWM_ICON_GRID_Y / 3;
    paste_time = dwm_time_ms();
}

// ... where the menu opened
static void new_icons_at(const dwm_menu_context_t* ctx) {
    new_icons_near(ctx->x, ctx->y);
}

// fileop pastes the clipboard's files into the desktop folder; the icons
// appear when it reports (desktop_sync), the first at the menu's spot
static void desktop_paste(const dwm_menu_context_t* ctx) {
    char dir[PATH_MAX];
    if (!desktop_dir(dir, sizeof(dir))) {
        dwm_notify_ex(DWM_NOTIFY_ERROR, "Can't paste", "No desktop folder", DWM_NOTIFY_LONG_MS);
        return;
    }
    const char* args[2] = { "paste", dir };
    if (launch_args(PROGRAM_FILEOP, args, 2, dir) < 0) {
        launch_failed("Can't paste", PROGRAM_FILEOP);
        return;
    }
    new_icons_at(ctx);
}

// fileop asks first, then deletes with its progress window
static void desktop_delete(const dwm_menu_context_t* ctx) {
    (void)ctx;
    const char* args[2 + DWM_MAX_ICONS];
    args[0] = "delete";
    args[1] = "-i";
    size_t count = selected_paths(args + 2, DWM_MAX_ICONS, true);
    if (count == 0)
        return;
    if (count > FILEOP_MAX_PATHS) {
        dwm_notifyf(DWM_NOTIFY_INFO, "Delete", "Only the first %u at once", (unsigned)FILEOP_MAX_PATHS);
        count = FILEOP_MAX_PATHS;
    }
    if (launch_args(PROGRAM_FILEOP, args, 2 + count, "/") < 0)
        launch_failed("Can't delete", PROGRAM_FILEOP);
}

static void desktop_properties(const dwm_menu_context_t* ctx) {
    const char* path = (const char*)dwm_icon_user(ctx->icon);
    if (path == NULL)
        return;
    char dir[PATH_MAX];
    parent_dir(path, dir, sizeof(dir));
    if (launch_system(PROGRAM_PROP, path, dir) < 0)
        launch_failed("No properties", PROGRAM_PROP);
}

// Format: one drive icon. format asks for admin and refuses the system drive.
static uint32_t only_on_one_drive(const dwm_menu_context_t* ctx) {
    if (ctx->icon == 0)
        return DWM_MENU_HIDDEN;
    const char* path = (const char*)dwm_icon_user(ctx->icon);
    if (path == NULL || !is_drive(path))
        return DWM_MENU_HIDDEN;
    return dwm_icon_selection(NULL, 0) == 1 ? 0 : DWM_MENU_DISABLED;
}

static void desktop_format(const dwm_menu_context_t* ctx) {
    const char* path = (const char*)dwm_icon_user(ctx->icon);
    if (path == NULL)
        return;
    if (launch_system(PROGRAM_FORMAT, path, "/") < 0)
        launch_failed("Can't format", PROGRAM_FORMAT);
}

// Unmount: one drive icon, dimmed on the drive home is on (the kernel
// refuses the rest of the system's disk too, and the menu says why)
static uint32_t only_on_removable(const dwm_menu_context_t* ctx) {
    uint32_t state = only_on_one_drive(ctx);
    if (state != 0)
        return state;
    const char* path = (const char*)dwm_icon_user(ctx->icon);
    char home[PATH_MAX];
    if (os_gethome(home, sizeof(home)) != NULL) {
        size_t n = strlen(path);
        if (strncmp(home, path, n) == 0 && (home[n] == '\0' || home[n] == '/'))
            return DWM_MENU_DISABLED;
    }
    return 0;
}

// Write out everything cached for the drive and unmount it (the whole disk,
// partitions and all). Its icon goes at once; no "Drive removed" follows.
static void desktop_unmount(const dwm_menu_context_t* ctx) {
    const char* path = (const char*)dwm_icon_user(ctx->icon);
    if (path == NULL || !is_drive(path))
        return;
    char device[64];
    strlcpy(device, path + sizeof(MNT_DIR), sizeof(device));    // "/mnt/usb0" -> "usb0"

    if (disk_unmount(device) < 0) {
        const char* why;
        switch (errno) {
            case EBUSY:  why = "A file on it is open: close it and try again"; break;
            case EACCES: why = "It holds the system"; break;
            case EIO:    why = "Writing to it failed: it is still mounted"; break;
            case EPERM:  why = "No file write clearance"; break;
            case ENOENT: why = "It is not mounted"; break;
            default:     why = strerror(errno); break;
        }
        dwm_notifyf(DWM_NOTIFY_ERROR, "Not unmounted", "%s: %s", device, why);
        return;
    }
    dwm_notifyf(DWM_NOTIFY_INFO, "Safe to remove", "%s can be pulled out now", device);
    drives_check(false);    // Its icon (and its partitions') go now, explorer windows re-read
}

// Rename: one icon, not a drive
static uint32_t only_on_one_file(const dwm_menu_context_t* ctx) {
    if (ctx->icon == 0)
        return DWM_MENU_HIDDEN;
    const char* path = (const char*)dwm_icon_user(ctx->icon);
    return (dwm_icon_selection(NULL, 0) == 1 && path != NULL && !is_drive(path)) ? 0 : DWM_MENU_DISABLED;
}

// New folder / New file: on the desktop itself
static uint32_t only_on_desktop(const dwm_menu_context_t* ctx) {
    return ctx->icon == 0 ? 0 : DWM_MENU_HIDDEN;
}

static dwm_icon_t icon_for(const char* path) {
    for (size_t i = 0; i < dwm_icon_count(); i++) {
        dwm_icon_t icon = dwm_icon_at_index(i);
        const char* p = (const char*)dwm_icon_user(icon);
        if (p != NULL && strcmp(p, path) == 0)
            return icon;
    }
    return 0;
}

// A free path in `dir` for `name`: the name itself, else "stem (1).ext",
// "stem (2).ext", ... False (errno) when there is none.
static bool free_path(const char* dir, const char* name, char* out, size_t size) {
    const char* dot = strrchr(name, '.');
    size_t stem = (dot != NULL && dot != name) ? (size_t)(dot - name) : strlen(name);
    for (int n = 0; n < 1000; n++) {
        int len = (n == 0) ? snprintf(out, size, "%s/%s", dir, name)
                           : snprintf(out, size, "%s/%.*s (%d)%s", dir, (int)stem, name, n, name + stem);
        if (len < 0 || (size_t)len >= size) {
            errno = ENAMETOOLONG;
            return false;
        }
        struct stat st;
        if (stat(out, &st) < 0 && errno == ENOENT)
            return true;
    }
    errno = EEXIST;
    return false;
}

// Make an empty folder or file on the desktop, put its icon where the menu
// opened and start renaming it
static void desktop_new(const dwm_menu_context_t* ctx, bool folder) {
    const char* what = folder ? "No new folder" : "No new file";
    char dir[PATH_MAX], path[PATH_MAX];
    if (!desktop_dir(dir, sizeof(dir))) {
        dwm_notify_ex(DWM_NOTIFY_ERROR, what, "No desktop folder", DWM_NOTIFY_LONG_MS);
        return;
    }
    if (!free_path(dir, folder ? NEW_FOLDER_NAME : NEW_FILE_NAME, path, sizeof(path))) {
        dwm_notifyf(DWM_NOTIFY_ERROR, what, "%s", strerror(errno));
        return;
    }
    int r;
    if (folder) {
        r = mkdir(path, 0777);
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        r = (fd < 0) ? -1 : close(fd);
    }
    if (r < 0) {
        dwm_notifyf(DWM_NOTIFY_ERROR, what, "%s", errno == EPERM ? "No file write clearance" : strerror(errno));
        return;
    }

    new_icons_at(ctx);
    desktop_sync();
    dwm_fs_changed();               // Explorer windows showing the desktop folder
    dwm_icon_t icon = icon_for(path);
    if (icon != 0)
        dwm_icon_edit(icon);
}

static void desktop_new_folder(const dwm_menu_context_t* ctx) {
    desktop_new(ctx, true);
}

static void desktop_new_file(const dwm_menu_context_t* ctx) {
    desktop_new(ctx, false);
}

static void desktop_rename(const dwm_menu_context_t* ctx) {
    dwm_icon_edit(ctx->icon);
}

static void desktop_select_all(const dwm_menu_context_t* ctx) {
    (void)ctx;
    dwm_icon_select_all(true);
}

static void menus_load(void);

// Icons, and the menus too: the registry may have changed
static void desktop_refresh(const dwm_menu_context_t* ctx) {
    (void)ctx;
    menus_load();
    layout_capture();       // Keep where they are now
    remove_icons();
    load_icons();
}

// Open console here: on the desktop, or on one folder or drive icon
static uint32_t only_in_folder(const dwm_menu_context_t* ctx) {
    if (ctx->icon == 0)
        return 0;
    const char* path = (const char*)dwm_icon_user(ctx->icon);
    struct stat st;
    return (path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : DWM_MENU_HIDDEN;
}

// A console window starting in the desktop folder, or in the folder or
// drive the menu opened on (the console takes it as its argument)
static void desktop_console(const dwm_menu_context_t* ctx) {
    char dir[PATH_MAX];
    const char* path = (ctx->icon != 0) ? (const char*)dwm_icon_user(ctx->icon) : NULL;
    if (path != NULL) {
        strlcpy(dir, path, sizeof(dir));
    } else if (!desktop_dir(dir, sizeof(dir))) {
        dwm_notify_ex(DWM_NOTIFY_ERROR, "No console", "No desktop folder", DWM_NOTIFY_LONG_MS);
        return;
    }
    if (launch_system(PROGRAM_PROMPT, dir, dir) < 0)
        launch_failed("No console", PROGRAM_PROMPT);
}

// "run <program>": <program> on the icon the menu opened on, or on the
// desktop folder. The menu's user is its desktop_menu_t, the item's id its
// line.
static void desktop_run(const dwm_menu_context_t* ctx);

// What a menu line's command can be (menus.h). "separator" and
// "run <program>" are handled where the menus are built.
static const struct {
    const char*       name;
    dwm_menu_action_t action;
    dwm_menu_state_t  state;
} menu_commands[] = {
    { "open",       desktop_open,       only_on_icon },
    { "edit",       desktop_edit,       only_on_file },
    { "cut",        desktop_cut,        only_on_files },
    { "copy",       desktop_copy,       only_on_icon },
    { "paste",      desktop_paste,      only_with_files },
    { "delete",     desktop_delete,     only_on_files },
    { "rename",     desktop_rename,     only_on_one_file },
    { "newfolder",  desktop_new_folder, only_on_desktop },
    { "newfile",    desktop_new_file,   only_on_desktop },
    { "selectall",  desktop_select_all, NULL },
    { "refresh",    desktop_refresh,    NULL },
    { "console",    desktop_console,    only_in_folder },
    { "format",     desktop_format,     only_on_one_drive },
    { "unmount",    desktop_unmount,    only_on_removable },
    { "properties", desktop_properties, only_on_icon },
};

#define MENU_SEPARATOR  "separator"
#define MENU_RUN        "run "

// Commands the menus no longer have: a registry written before they went
// may still list them, so those lines are left out rather than shown dimmed
// ("boot repair" takes them out of the registry too)
static const char* const retired_commands[] = {
    "quit",         // Shut down is the start menu's button now
};

static bool retired_command(const char* command) {
    for (size_t i = 0; i < sizeof(retired_commands) / sizeof(retired_commands[0]); i++)
        if (strcmp(retired_commands[i], command) == 0)
            return true;
    return false;
}

// Lines the menus have gained: a registry written before they came gets
// them shown anyway, right after the line with command `after` (that menu
// left alone without one, or when it already has the command). "boot
// repair" writes them into the registry too (boot_menu_added).
static const struct {
    const char* menu;
    const char* label;
    const char* command;
    const char* after;
} added_commands[] = {
    { "icon", "Edit", "edit", "open" },
};

static int find_command(const menus_line_t* lines, int n, const char* command) {
    for (int i = 0; i < n; i++)
        if (strcmp(lines[i].command, command) == 0)
            return i;
    return -1;
}

// Built in, for a registry without the menus: the same lines "boot install"
// and "boot repair" write (kernel/console/commands/boot_registry.h)
static const menus_line_t default_desktop_lines[] = {
    { "Paste",        "paste" },
    { "",             MENU_SEPARATOR },
    { "New folder",   "newfolder" },
    { "New file",     "newfile" },
    { "",             MENU_SEPARATOR },
    { "Select all",   "selectall" },
    { "Refresh",      "refresh" },
    { "",             MENU_SEPARATOR },
    { "Open console here", "console" },
};

static const menus_line_t default_icon_lines[] = {
    { "Open",         "open" },
    { "Edit",         "edit" },
    { "Open console here", "console" },
    { "",             MENU_SEPARATOR },
    { "Cut",          "cut" },
    { "Copy",         "copy" },
    { "Delete",       "delete" },
    { "Rename",       "rename" },
    { "",             MENU_SEPARATOR },
    { "Select all",   "selectall" },
    { "Refresh",      "refresh" },
    { "",             MENU_SEPARATOR },
    { "Format...",    "format" },
    { "Unmount",      "unmount" },
    { "Properties",   "properties" },
};

typedef struct {
    const char*         name;               // Under MENUS_KEY
    const menus_line_t* defaults;
    size_t              default_count;
    menus_line_t        lines[DWM_MENU_MAX_ITEMS];
    dwm_menu_item_t     items[DWM_MENU_MAX_ITEMS];
    dwm_menu_t          menu;               // What dwm_menu_show gets
} desktop_menu_t;

#define DESKTOP_MENU(key, table) \
    { .name = (key), .defaults = (table), .default_count = sizeof(table) / sizeof((table)[0]) }

static desktop_menu_t desktop_menu = DESKTOP_MENU("desktop", default_desktop_lines);
static desktop_menu_t icon_menu    = DESKTOP_MENU("icon", default_icon_lines);

// (Re)build `m` from the registry, or from its defaults when the registry
// has no lines for it. A command nobody knows shows dimmed, so a typo in
// the registry is seen rather than lost.
static void menu_load(desktop_menu_t* m) {
    int n = menus_read(m->name, m->lines, DWM_MENU_MAX_ITEMS);
    if (n <= 0) {
        n = (int)m->default_count;
        memcpy(m->lines, m->defaults, m->default_count * sizeof(m->lines[0]));
    }

    // Retired lines out, then any separator they leave at the end
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (retired_command(m->lines[i].command))
            continue;
        if (kept != i)
            m->lines[kept] = m->lines[i];
        kept++;
    }
    if (kept < n) {
        while (kept > 0 && strcmp(m->lines[kept - 1].command, MENU_SEPARATOR) == 0)
            kept--;
        n = kept;
    }

    for (size_t a = 0; a < sizeof(added_commands) / sizeof(added_commands[0]); a++) {
        if (strcmp(added_commands[a].menu, m->name) != 0 || n >= DWM_MENU_MAX_ITEMS ||
            find_command(m->lines, n, added_commands[a].command) >= 0)
            continue;
        int after = find_command(m->lines, n, added_commands[a].after);
        if (after < 0)
            continue;
        memmove(&m->lines[after + 2], &m->lines[after + 1], (size_t)(n - after - 1) * sizeof(m->lines[0]));
        strlcpy(m->lines[after + 1].label, added_commands[a].label, sizeof(m->lines[0].label));
        strlcpy(m->lines[after + 1].command, added_commands[a].command, sizeof(m->lines[0].command));
        n++;
    }

    for (int i = 0; i < n; i++) {
        const menus_line_t* line = &m->lines[i];
        dwm_menu_item_t* it = &m->items[i];
        memset(it, 0, sizeof(*it));
        it->label = line->label;
        it->id    = (uint32_t)i;

        if (strcmp(line->command, MENU_SEPARATOR) == 0) {
            it->flags = DWM_MENU_SEPARATOR;
            continue;
        }
        if (strncmp(line->command, MENU_RUN, strlen(MENU_RUN)) == 0) {
            it->action = desktop_run;
            continue;
        }
        it->flags = DWM_MENU_DISABLED;
        for (size_t k = 0; k < sizeof(menu_commands) / sizeof(menu_commands[0]); k++) {
            if (strcmp(menu_commands[k].name, line->command) == 0) {
                it->flags  = 0;
                it->action = menu_commands[k].action;
                it->state  = menu_commands[k].state;
                break;
            }
        }
    }

    m->menu.items = m->items;
    m->menu.count = (size_t)n;
    m->menu.user  = m;
}

static void menus_load(void) {
    menu_load(&desktop_menu);
    menu_load(&icon_menu);
}

static void desktop_run(const dwm_menu_context_t* ctx) {
    const desktop_menu_t* m = (const desktop_menu_t*)ctx->user;
    if (m == NULL || ctx->id >= m->menu.count)
        return;
    const char* program = m->lines[ctx->id].command + strlen(MENU_RUN);
    while (*program == ' ')
        program++;
    const char* what = m->lines[ctx->id].label[0] != '\0' ? m->lines[ctx->id].label : "Can't run";
    if (*program == '\0')
        return;

    char target[PATH_MAX], dir[PATH_MAX];
    const char* path = (ctx->icon != 0) ? (const char*)dwm_icon_user(ctx->icon) : NULL;
    if (path != NULL) {
        strlcpy(target, path, sizeof(target));
        parent_dir(path, dir, sizeof(dir));
    } else if (desktop_dir(target, sizeof(target))) {
        strlcpy(dir, target, sizeof(dir));
    } else {
        dwm_notify_ex(DWM_NOTIFY_ERROR, what, "No desktop folder", DWM_NOTIFY_LONG_MS);
        return;
    }
    if (launch_system(program, target, dir) < 0)
        launch_failed(what, program);
}

// Every window (unless its procedure opens a menu of its own)

static uint32_t only_closable(const dwm_menu_context_t* ctx) {
    return (dwm_window_style(ctx->window) & DWM_STYLE_NOCLOSEBOX) ? DWM_MENU_DISABLED : 0;
}

static void window_to_front(const dwm_menu_context_t* ctx) {
    dwm_window_raise(ctx->window);
    if (!(dwm_window_style(ctx->window) & DWM_STYLE_NOFOCUS))
        dwm_window_focus(ctx->window);
}

static void window_center(const dwm_menu_context_t* ctx) {
    gfx_rect_t frame;
    if (dwm_window_get_rect(ctx->window, &frame, NULL))
        dwm_window_move(ctx->window, (dwm_screen_width() - frame.w) / 2, (dwm_work_area().h - frame.h) / 2);
}

static void window_close(const dwm_menu_context_t* ctx) {
    dwm_window_close(ctx->window);
}

static void window_minimize(const dwm_menu_context_t* ctx) {
    dwm_window_minimize(ctx->window);
}

// Minimize: windows with a titlebar (the ones with a taskbar button), dimmed
// for those that can't be minimized
static uint32_t only_titled(const dwm_menu_context_t* ctx) {
    uint32_t style = dwm_window_style(ctx->window);
    if (style & (DWM_STYLE_NOBORDER | DWM_STYLE_NOTITLEBAR))
        return DWM_MENU_HIDDEN;
    return (style & DWM_STYLE_NOMINIMIZE) ? DWM_MENU_DISABLED : 0;
}

static const dwm_menu_item_t window_items[] = {
    { .label = "Bring to front",   .action = window_to_front },
    { .label = "Center on screen", .action = window_center },
    { .label = "Minimize",         .action = window_minimize, .state = only_titled },
    DWM_MENU_SEP,
    { .label = "Close",            .action = window_close, .state = only_closable },
};
static const dwm_menu_t window_menu = DWM_MENU_INIT(window_items, NULL);

//
// Renaming
//

// `old_path` is now `new_path`: clipboard paths at or below it follow, so
// a cut or copy made before the rename still pastes. The list is rewritten
// into one buffer sized from the clipboard data, freed before returning.
static void clip_renamed(const char* old_path, const char* new_path) {
    dwm_clip_info_t info;
    const char* data = (const char*)dwm_clip_data();
    if (!dwm_clip_info(&info) || info.type != DWM_CLIP_FILES || data == NULL)
        return;

    size_t old_len = strlen(old_path), new_len = strlen(new_path);

    // Worst case: every line starts with old_path and grows by the difference
    size_t lines = 1;
    for (uint32_t i = 0; i < info.size; i++)
        lines += (data[i] == '\n');
    size_t grow = (new_len > old_len) ? new_len - old_len : 0;
    char* out = (char*)malloc((size_t)info.size + lines * grow + 1);
    if (out == NULL)
        return;     // Leave it rather than spoil it

    size_t at = 0;
    bool changed = false;
    const char* p = data;
    const char* stop = data + info.size;
    while (p < stop) {
        const char* end = (const char*)memchr(p, '\n', (size_t)(stop - p));
        size_t len = end ? (size_t)(end - p) : (size_t)(stop - p);

        if (len >= old_len && strncmp(p, old_path, old_len) == 0 &&
            (len == old_len || p[old_len] == '/')) {
            if (new_len + (len - old_len) >= PATH_MAX) {
                free(out);
                return;
            }
            memcpy(out + at, new_path, new_len);
            at += new_len;
            memcpy(out + at, p + old_len, len - old_len);
            at += len - old_len;
            changed = true;
        } else {
            memcpy(out + at, p, len);
            at += len;
        }

        if (end == NULL)
            break;
        out[at++] = '\n';
        p = end + 1;
    }

    if (changed) {
        info.size = (uint32_t)at;
        dwm_clip_set(&info, out);   // Copies it; serial and owner are set fresh
    }
    free(out);
}

// A rename was kept in the icon's box: rename the file, then the icon
static void icon_renamed(dwm_icon_t icon) {
    const char* name = dwm_icon_edit_result();
    char* path = (char*)dwm_icon_user(icon);
    if (path == NULL || is_drive(path))
        return;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strchr(name, '/') != NULL) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "Not renamed", "\"%s\" can't be a name", name);
        return;
    }

    char dir[PATH_MAX], renamed[PATH_MAX];
    parent_dir(path, dir, sizeof(dir));
    int n = snprintf(renamed, sizeof(renamed), "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, name);
    if (n < 0 || (size_t)n >= sizeof(renamed)) {
        dwm_notify("Not renamed", "Name too long", DWM_NOTIFY_DEFAULT_MS);
        return;
    }
    if (os_rename(path, name) < 0) {
        dwm_notifyf(DWM_NOTIFY_ERROR, "Not renamed", "%s: %s", name,
                    errno == EEXIST ? "name taken" : errno == EPERM ? "no file write clearance" : strerror(errno));
        return;
    }

    char* copy = strdup(renamed);
    if (copy == NULL) {
        desktop_refresh(NULL);      // Renamed, but the icon can't keep up: read them again
        return;
    }
    clip_renamed(path, renamed);
    layout_rename(path, renamed);
    dwm_icon_set_user(icon, copy);
    dwm_icon_set_path(icon, copy);
    dwm_icon_set_label(icon, name);
    free(path);
    dwm_fs_changed();
}

//
// Drag and drop (dwm/drag.h)
//

// `path` is `dir` or inside it
static bool path_inside(const char* path, const char* dir) {
    size_t n = strlen(dir);
    if (n == 1 && dir[0] == '/')
        return path[0] == '/';
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

// `a` and `b` are on the same drive (or both outside every drive)
static bool same_drive(const char* a, const char* b) {
    return dwm_same_drive(a, b);
}

// fileop moves (or copies) each of `paths` into `dir`, with its progress
// window; a taken name becomes "name (1)". Anything from another drive is
// copied instead: a move only happens within one drive. Files already in
// `dir` stay (a move there does nothing), drives and folders dropped into
// themselves are left out. `place`: the first new icon lands near (x, y).
static void move_into(const char* dir, const char* const* paths, size_t count, bool copy,
                      bool place, int32_t x, int32_t y) {
    static char targets[FILEOP_MAX_PAIRS][PATH_MAX];       // Static: too big for the stack
    const char* args[3 + 2 * FILEOP_MAX_PAIRS];
    for (size_t i = 0; i < count && !copy; i++)
        copy = !same_drive(paths[i], dir);
    const char* what = copy ? "Not copied" : "Not moved";
    size_t n = 2, pairs = 0;
    args[0] = copy ? "copy" : "move";
    args[1] = "-r";

    for (size_t i = 0; i < count; i++) {
        const char* src = paths[i];
        const char* name = strrchr(src, '/');
        name = (name != NULL && name[1] != '\0') ? name + 1 : src;
        char parent[PATH_MAX];
        parent_dir(src, parent, sizeof(parent));

        if (!copy && strcmp(parent, dir) == 0)
            continue;                           // Already there
        if (!copy && is_drive(src)) {
            dwm_notifyf(DWM_NOTIFY_INFO, what, "%s: drives can't be moved", name);
            continue;
        }
        if (path_inside(dir, src)) {
            dwm_notifyf(DWM_NOTIFY_ERROR, what, "%s can't go inside itself", name);
            continue;
        }
        if (pairs == FILEOP_MAX_PAIRS) {
            dwm_notifyf(DWM_NOTIFY_INFO, copy ? "Copy" : "Move", "Only the first %u at once",
                        (unsigned)FILEOP_MAX_PAIRS);
            break;
        }
        int len = snprintf(targets[pairs], PATH_MAX, "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, name);
        if (len < 0 || len >= PATH_MAX) {
            dwm_notifyf(DWM_NOTIFY_ERROR, what, "%s: path too long", name);
            continue;
        }
        args[n++] = src;
        args[n++] = targets[pairs++];
    }
    if (pairs == 0)
        return;

    if (launch_args(PROGRAM_FILEOP, args, n, dir) < 0) {
        launch_failed(what, PROGRAM_FILEOP);
        return;
    }
    if (place)
        new_icons_near(x, y);
}

// Move (or copy) the '\n'-separated paths in `list` (cut up here) into
// `onto` (a folder or drive), or with NULL into the desktop folder, the
// first new icon near (x, y)
static void drop_list(char* list, const char* onto, int32_t x, int32_t y, bool copy) {
    const char* paths[FILEOP_MAX_PAIRS + 1];        // One over, so move_into says "only the first"
    size_t count = 0;
    char* save = NULL;
    for (char* line = strtok_r(list, "\n", &save); line != NULL && count < FILEOP_MAX_PAIRS + 1;
         line = strtok_r(NULL, "\n", &save))
        paths[count++] = line;

    char dir[PATH_MAX];
    if (onto != NULL)
        move_into(onto, paths, count, copy, false, 0, 0);
    else if (desktop_dir(dir, sizeof(dir)))
        move_into(dir, paths, count, copy, true, x, y);
    else
        dwm_notify_ex(DWM_NOTIFY_ERROR, copy ? "Not copied" : "Not moved", "No desktop folder",
                      DWM_NOTIFY_LONG_MS);
}

// A right-button drop waits for its menu: Move here / Copy here / Cancel
static struct {
    char*   list;                   // malloc'd paths, NULL: none waiting
    char*   onto;                   // malloc'd folder, NULL: the desktop folder
    int32_t x, y;
} asked;

static void asked_free(void) {
    free(asked.list);
    free(asked.onto);
    memset(&asked, 0, sizeof(asked));
}

static void asked_finish(bool copy) {
    if (asked.list != NULL)
        drop_list(asked.list, asked.onto, asked.x, asked.y, copy);
    asked_free();
}

static void drop_move_here(const dwm_menu_context_t* ctx) { (void)ctx; asked_finish(false); }
static void drop_copy_here(const dwm_menu_context_t* ctx) { (void)ctx; asked_finish(true); }
static void drop_cancel(const dwm_menu_context_t* ctx)    { (void)ctx; asked_free(); }

static const dwm_menu_item_t drop_items[] = {
    { .label = "Move here", .action = drop_move_here },
    { .label = "Copy here", .action = drop_copy_here },
    DWM_MENU_SEP,
    { .label = "Cancel",    .action = drop_cancel },
};
static const dwm_menu_t drop_menu = DWM_MENU_INIT(drop_items, NULL);

// From another drive a left drag copies: Copy here comes first then
static const dwm_menu_item_t drop_copy_items[] = {
    { .label = "Copy here", .action = drop_copy_here },
    { .label = "Move here", .action = drop_move_here },
    DWM_MENU_SEP,
    { .label = "Cancel",    .action = drop_cancel },
};
static const dwm_menu_t drop_copy_menu = DWM_MENU_INIT(drop_copy_items, NULL);

// Any of the '\n'-separated paths in `list` on another drive than `dir`
static bool list_other_drive(const char* list, const char* dir) {
    char path[PATH_MAX];
    for (const char* at = list; *at != '\0'; ) {
        const char* end = strchr(at, '\n');
        size_t len = (end != NULL) ? (size_t)(end - at) : strlen(at);
        if (len > 0 && len < sizeof(path)) {
            memcpy(path, at, len);
            path[len] = '\0';
            if (!same_drive(path, dir))
                return true;
        }
        if (end == NULL)
            break;
        at = end + 1;
    }
    return false;
}

// Keep the drop (list, onto: copied) and ask at (x, y) what to do with it
static void ask_drop(const char* list, const char* onto, int32_t x, int32_t y) {
    asked_free();               // One that was never answered (the menu was dismissed)
    asked.list = strdup(list);
    asked.onto = (onto != NULL) ? strdup(onto) : NULL;
    asked.x = x;
    asked.y = y;
    if (asked.list == NULL || (onto != NULL && asked.onto == NULL)) {
        asked_free();
        dwm_notify("Not moved", "Out of memory", DWM_NOTIFY_DEFAULT_MS);
        return;
    }
    char dir[PATH_MAX];
    const char* into = (onto != NULL) ? onto : desktop_dir(dir, sizeof(dir)) ? dir : NULL;
    bool copy_first = into != NULL && list_other_drive(list, into);
    if (!dwm_menu_show(DWM_DESKTOP, copy_first ? &drop_copy_menu : &drop_menu, x, y))
        asked_free();
}

// `paths` joined with '\n' (malloc'd), NULL when out of memory
static char* join_lines(const char* const* paths, size_t count) {
    size_t total = 1;
    for (size_t i = 0; i < count; i++)
        total += strlen(paths[i]) + 1;
    char* list = (char*)malloc(total);
    if (list == NULL)
        return NULL;
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(paths[i]);
        memcpy(list + at, paths[i], n);
        at += n;
        if (i + 1 < count)
            list[at++] = '\n';
    }
    list[at] = '\0';
    return list;
}

// The selected icons were let go over a window or onto a folder / drive icon
// (they are back in their places): the window gets them as a drop, the
// folder gets them moved into it (after asking, for a right drag)
static void icons_dropped(const dwm_event_t* ev) {
    const char* paths[DWM_MAX_ICONS];
    bool ask = (ev->button == INPUT_BTN_RIGHT);
    bool copy = (ev->mods & INPUT_MOD_CTRL) != 0;
    size_t count = selected_paths(paths, DWM_MAX_ICONS, !copy && !ask);    // Drives can only be copied
    if (count == 0) {
        dwm_notify("Not moved", "Drives can't be moved (hold Ctrl to copy)", DWM_NOTIFY_DEFAULT_MS);
        return;
    }

    dwm_handle_t window = (dwm_handle_t)ev->param[0];
    if (window != DWM_DESKTOP) {
        // A right drag: the window asks
        if (!dwm_drop_files(window, ev->x, ev->y, ev->mods, ev->button, paths, count))
            dwm_notifyf(DWM_NOTIFY_ERROR, copy ? "Not copied" : "Not moved", "%s", strerror(errno));
        return;
    }

    const char* dir = (const char*)dwm_icon_user((dwm_icon_t)ev->param[1]);
    if (dir == NULL)
        return;
    if (!ask) {
        move_into(dir, paths, count, copy, false, 0, 0);
        return;
    }
    char* list = join_lines(paths, count);
    if (list == NULL) {
        dwm_notify("Not moved", "Out of memory", DWM_NOTIFY_DEFAULT_MS);
        return;
    }
    ask_drop(list, dir, ev->x, ev->y);
    free(list);
}

// A program's files were let go over the desktop: into the folder or drive
// icon under them, else into the desktop folder where they were let go
// (a right drag asks first)
static void files_dropped(const dwm_event_t* ev) {
    dwm_clip_info_t info;
    const char* data = dwm_drop_data(ev->param[1], &info);
    if (data == NULL || info.type != DWM_CLIP_FILES)
        return;
    const char* onto = (ev->param[0] != 0) ? (const char*)dwm_icon_user((dwm_icon_t)ev->param[0]) : NULL;

    if (ev->button == INPUT_BTN_RIGHT) {
        ask_drop(data, onto, ev->x, ev->y);
        return;
    }
    char* list = strdup(data);
    if (list == NULL) {
        dwm_notify("Not moved", "Out of memory", DWM_NOTIFY_DEFAULT_MS);
        return;
    }
    drop_list(list, onto, ev->x, ev->y, (ev->mods & INPUT_MOD_CTRL) != 0);
    free(list);
}

//
// Start menu (dwm/taskbar.h): the programs in <home>/sys and <home>/bin, a few folders,
// and the search, whose results open as their icons would
//

#define START_PROGRAMS_MAX  (DWM_START_MAX_ITEMS - 9)    // Room for the folders and separators

// What the programs in <home>/sys and <home>/bin are called in the menu. Not listed:
// shown by their file name. NULL: helpers the desktop runs, left out.
static const struct {
    const char* file;
    const char* label;
} program_names[] = {
    { "explorer",    "File explorer" },
    { "notepad",     "Notepad" },
    { "paint",       "Paint" },
    { "hexedit",     "Hex editor" },
    { "calc",        "Calculator" },
    { "charmap",     "Character map" },
    { "htmlview",    "HTML viewer" },
    { "mediaplay",   "Media player" },
    { "minesweeper", "Minesweeper" },
    { "prompt",      "Prompt" },
    { "console",     "Console" },
    { "flight",      "Flight" },
    { "8086tiny",    "8086tiny" },
    { "dwm",         NULL },
    { PROGRAM_PROP,  NULL },
    { PROGRAM_FILEOP, NULL },
    { PROGRAM_FORMAT, "Format drive" },
    { "netsrv",      NULL },        // An older install: now in sys/services
};

typedef struct {
    char        label[DWM_START_LABEL_MAX];
    char        path[PATH_MAX];
    const char* image;
} start_program_t;

static int compare_programs(const void* a, const void* b) {
    return strcasecmp(((const start_program_t*)a)->label, ((const start_program_t*)b)->label);
}

// The shut down button: the desktop ends, and main() saves the icons'
// places and writes every file system out before it returns
static void start_shutdown(const char* arg) {
    (void)arg;
    dwm_quit();
}

// Add the programs in `dir` to programs[count...]; returns the new count.
// A name already listed (from an earlier folder) is left out: that one is
// what os_programpath() would start.
static size_t add_start_programs(const char* dir, start_program_t* programs, size_t count) {
    struct stat dir_st;
    if (stat(dir, &dir_st) < 0 || !S_ISDIR(dir_st.st_mode))
        return count;           // No bin folder: nothing to say
    entry_t* list = NULL;
    size_t n = list_dir(dir, &list);
    for (size_t i = 0; i < n && count < START_PROGRAMS_MAX; i++) {
        if (list[i].type != DT_REG)
            continue;
        const char* label = list[i].name;
        bool skip = false;
        for (size_t k = 0; k < sizeof(program_names) / sizeof(program_names[0]); k++) {
            if (strcmp(program_names[k].file, list[i].name) == 0) {
                label = program_names[k].label;
                skip = (label == NULL);
                break;
            }
        }
        for (size_t k = 0; k < count && !skip; k++) {
            const char* slash = strrchr(programs[k].path, '/');
            skip = (slash != NULL && strcmp(slash + 1, list[i].name) == 0);
        }
        start_program_t* p = &programs[count];
        int len = snprintf(p->path, sizeof(p->path), "%s/%s", dir, list[i].name);
        struct stat st;
        if (skip || len < 0 || (size_t)len >= sizeof(p->path) || stat(p->path, &st) < 0 ||
            !(st.st_mode & S_IXUSR))
            continue;           // Fonts and other files in sys: not programs
        strlcpy(p->label, label, sizeof(p->label));
        p->image = file_image(p->path);
        count++;
    }
    free(list);
    return count;
}

// Fill the start menu: programs (by name), folders, then the desktop's own
static void build_start_menu(void) {
    static start_program_t programs[START_PROGRAMS_MAX];    // Static: big
    size_t count = 0;

    // Same order as os_programpath(): sys, then bin
    char dir[PATH_MAX];
    if (os_syspath("", dir, sizeof(dir)) != NULL)
        count = add_start_programs(dir, programs, count);
    char bin_home[PATH_MAX];
    if (os_gethome(bin_home, sizeof(bin_home)) != NULL) {
        int n = snprintf(dir, sizeof(dir), "%s/bin", bin_home);
        if (n >= 0 && (size_t)n < sizeof(dir))
            count = add_start_programs(dir, programs, count);
    }
    if (count > 1)
        qsort(programs, count, sizeof(programs[0]), compare_programs);

    static char home[PATH_MAX], desktop[PATH_MAX], settings[PATH_MAX], games[PATH_MAX];
    bool has_home = os_gethome(home, sizeof(home)) != NULL;
    bool has_desktop = desktop_dir(desktop, sizeof(desktop));
    struct stat settings_st, games_st;
    bool has_settings = os_syspath(SETTINGS_SUB, settings, sizeof(settings)) != NULL &&
                        stat(settings, &settings_st) == 0 && S_ISDIR(settings_st.st_mode);
    int games_len = has_home ? snprintf(games, sizeof(games), "%s/%s", home, GAMES_SUB) : -1;
    bool has_games = games_len > 0 && (size_t)games_len < sizeof(games) &&
                     stat(games, &games_st) == 0 && S_ISDIR(games_st.st_mode);

    dwm_start_item_t items[DWM_START_MAX_ITEMS];
    size_t at = 0;
    for (size_t i = 0; i < count; i++)
        items[at++] = (dwm_start_item_t){ .label = programs[i].label, .image = programs[i].image,
                                          .arg = programs[i].path, .action = open_path };
    if (at > 0) items[at++] = (dwm_start_item_t)DWM_START_SEP;
    if (has_desktop) items[at++] = (dwm_start_item_t){ .label = "Desktop", .image = IMAGE_FOLDER, .arg = desktop, .action = open_path };
    if (has_settings) items[at++] = (dwm_start_item_t){ .label = "Settings", .image = IMAGE_FOLDER, .arg = settings, .action = open_path };
    if (has_games)   items[at++] = (dwm_start_item_t){ .label = "Games", .image = IMAGE_FOLDER, .arg = games, .action = open_path };
    items[at++] = (dwm_start_item_t)DWM_START_SEP;
    if (has_home)    items[at++] = (dwm_start_item_t){ .label = "Home", .image = IMAGE_DRIVE, .arg = home, .action = open_path };
    items[at++] = (dwm_start_item_t){ .label = "Drives", .image = IMAGE_DRIVE, .arg = MNT_DIR, .action = open_path };
    dwm_start_set_items(items, at);
}

//
// Desktop
//

static bool desktop_proc(dwm_handle_t win, const dwm_event_t* ev, void* user) {
    (void)win; (void)user;
    switch (ev->type) {
        case DWM_EVENT_ICON_OPEN:
            open_icon((dwm_icon_t)ev->param[0]);
            return true;

        case DWM_EVENT_ICONS_MOVED:
            return true;            // Kept where they are; written when the desktop quits

        case DWM_EVENT_ICON_RENAMED:
            icon_renamed((dwm_icon_t)ev->param[0]);
            return true;

        case DWM_EVENT_TIMER:
            if (ev->param[0] == TIMER_DRIVES) {
                drives_check(true);
                usbwatch_check();   // USB devices nothing drives
            }
            return true;

        case DWM_EVENT_FS_CHANGED:
            desktop_sync();
            build_start_menu();     // A program may have come or gone
            return true;

        case DWM_EVENT_ICONS_DROPPED:
            icons_dropped(ev);
            return true;

        case DWM_EVENT_SETTINGS_CHANGED:
            wallpaper_load();       // The desktop settings program changed the background
            return true;

        case DWM_EVENT_DROP:
            files_dropped(ev);
            return true;

        case DWM_EVENT_CONTEXT_MENU:
            // The icon menu on an icon, the desktop menu elsewhere. Handled
            // even when nothing shows: never the other menu instead.
            dwm_menu_show(DWM_DESKTOP, ev->param[0] != 0 ? &icon_menu.menu : &desktop_menu.menu,
                          ev->x, ev->y);
            return true;
    }
    return false;
}

// What service program `path` may hold: its entry in service_grants, by
// file name, else SERVICE_DEFAULT_GRANT
static sec_mask_t service_grant(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* file = (slash != NULL) ? slash + 1 : path;
    for (size_t i = 0; i < sizeof(service_grants) / sizeof(service_grants[0]); i++)
        if (strcmp(service_grants[i].program, file) == 0)
            return service_grants[i].security;
    return SERVICE_DEFAULT_GRANT;
}

// Start the services the registry lists (services.h), or the built-in
// ones when it has no list. A listed service that is missing or won't
// start says so; a built-in one that isn't installed is left out quietly
// (an older install).
static void start_services(void) {
    static services_entry_t list[SERVICES_MAX];     // Static: PATH_MAX each
    const services_entry_t* services = list;
    bool builtin = false;
    int count = services_read(list, SERVICES_MAX);
    if (count < 0) {
        if (errno != ENOENT) {
            dwm_notifyf(DWM_NOTIFY_ERROR, "Services not started", "%s: %s",
                        SERVICES_KEY, strerror(errno));
            return;
        }
        services = services_defaults;
        count = (int)(sizeof(services_defaults) / sizeof(services_defaults[0]));
        builtin = true;
    }

    for (int i = 0; i < count; i++) {
        const services_entry_t* s = &services[i];
        char path[PATH_MAX];
        if (services_resolve(s->path, path, sizeof(path)) == NULL) {
            if (!builtin)
                launch_failed("Service not started", s->name);
            continue;
        }
        struct stat st;
        if (stat(path, &st) < 0 || S_ISDIR(st.st_mode)) {
            if (!builtin)
                dwm_notifyf(DWM_NOTIFY_ERROR, "Service not found", "%s: %s", s->name, s->path);
            continue;
        }
        char* args[] = { path, NULL };
        if (spawn_ex(path, args, SPAWN_CWD_PROGRAM, service_grant(path) & sec_get()) < 0)
            launch_failed("Service didn't start", s->name);
    }
}

int main(void) {
    if (dwm_init() < 0)
        return errno != 0 ? errno : 1;      // No screen to say why: the exit code does

    dwm_set_desktop_proc(desktop_proc, NULL);
    wallpaper_load();                       // Color or wallpaper, from the registry
    menus_load();
    dwm_set_desktop_menu(&desktop_menu.menu);   // desktop_proc opens them itself
    dwm_set_window_menu(&window_menu);
    build_start_menu();
    dwm_start_set_open(open_path);          // Search results; the roots stay /mnt
    dwm_start_set_shutdown(start_shutdown); // The button over the search box's right end

    if (layout_load() < 0)
        dwm_notifyf(DWM_NOTIFY_ERROR, "Layout not loaded", "%s", strerror(errno));
    load_icons();

    drives_remember();
    usbwatch_remember();
    if (!dwm_timer_start(DWM_DESKTOP, TIMER_DRIVES, DRIVE_POLL_MS, true))
        dwm_notify("Drives not watched", "Plugged-in drives show after Refresh", DWM_NOTIFY_DEFAULT_MS);
    start_services();

    int rc = dwm_run();

    // However the desktop ended (menu, Esc), the icons stay where they are.
    // Then every file system is written out, so the layout (and anything
    // else still cached) is on disk before the machine goes down. Nobody
    // would see a notification now, so a failure goes unsaid.
    layout_save();
    os_sync();

    dwm_shutdown();
    for (size_t i = 0; i < dwm_icon_count(); i++)
        free(dwm_icon_user(dwm_icon_at_index(i)));
    asked_free();
    layout_free();
    imagelib_free();
    fontlib_free();
    return rc < 0 ? 1 : 0;
}
