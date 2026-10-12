#include <kernel/dwm/dwm.h>
#include <kernel/dwm/dwm_core_internal.h>
#include <kernel/dwm/dwm_context_menu.h>
#include <kernel/dwm/dwm_actions.h>

#include <kernel/kernel.h>
#include <kernel/events.h>
#include <kernel/vfs/vfs.h>
#include <kernel/registry/registry.h>
#include <kernel/console/display.h>
#include <kernel/util/list.h>
#include <kernel/util/timer.h>
#include <kernel/util/string.h>

// ==========================================
// Registry layout (see dwm_context_menu.h)
// ==========================================

#define MENU_REG_PATH       "software/dwm/menus"
#define MENU_REG_PERMS      (REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE)

#define MENU_NAME_DESKTOP   "desktop"
#define MENU_NAME_ICON      "icon"

#define MENU_ITEMS_MAX      16      // dwm_create_context_menu's limit
#define MENU_LABEL_MAX      16      // struct MenuItem name[16]

typedef struct {
    const char* order;
    const char* label;
    const char* action;
} MenuDefault;

static const MenuDefault desktop_defaults[] = {
    { "10", "Refresh",    "refresh"            },
    { "20", "New folder", "new_folder"         },
    { "30", "New file",   "new_file"           },
    { "40", "Paste",      "paste"              },
    { "50", "Properties", "desktop_properties" },
};

static const MenuDefault icon_defaults[] = {
    { "10", "Open",       "open"       },
    { "20", "Cut",        "cut"        },
    { "30", "Copy",       "copy"       },
    { "40", "Rename",     "rename"     },
    { "50", "Delete",     "delete"     },
    { "60", "Properties", "properties" },
};

#define DESKTOP_DEFAULT_COUNT  (sizeof(desktop_defaults) / sizeof(desktop_defaults[0]))
#define ICON_DEFAULT_COUNT     (sizeof(icon_defaults) / sizeof(icon_defaults[0]))

// ==========================================
// Helpers
// ==========================================

/**
 * Generates a unique file/folder name and full path by appending index numbers 
 * (e.g., "new_folder (1)") if an item with the same name already exists.
 */
void name_get_unique(const char* dir_path, const char* base_name, char* out_name, char* out_path) {
    int count = 0;

    // Detect file extension (ignore leading dot for hidden files)
    const char* dot = strrchr(base_name, '.');
    if (dot == base_name) {
        dot = NULL;
    }

    while (1) {
        if (count == 0) {
            strncpy(out_name, base_name, DWM_MAX_PATH_LEN - 1);
            out_name[DWM_MAX_PATH_LEN - 1] = '\0';
        } else if (dot != NULL) {
            int name_len = (int)(dot - base_name);
            snprintf(out_name, DWM_MAX_PATH_LEN, "%.*s (%d)%s", name_len, base_name, count, dot);
        } else {
            snprintf(out_name, DWM_MAX_PATH_LEN, "%s (%d)", base_name, count);
        }

        // Construct full target path
        memset(out_path, 0, DWM_MAX_PATH_LEN);
        strncpy(out_path, dir_path, DWM_MAX_PATH_LEN - 1);

        size_t dir_len = strlen(out_path);
        if (dir_len > 0 && out_path[dir_len - 1] != '/') {
            strncat(out_path, "/", DWM_MAX_PATH_LEN - strlen(out_path) - 1);
        }
        strncat(out_path, out_name, DWM_MAX_PATH_LEN - strlen(out_path) - 1);

        // Check if path is available
        if (!vfs_exists(out_path)) {
            break;
        }

        count++;
    }
}

static bool desktop_has_icon(const char* path) {
    for (struct list_node* node = workspace.icon_head; node != NULL; node = node->next) {
        struct IconObject* icon = (struct IconObject*)node->data;
        if (strncmp(icon->path, path, DWM_MAX_PATH_LEN) == 0)
            return true;
    }
    return false;
}

void dwm_notify_item_created(const char* path) {
    DWM_GUARD();
    if (path == NULL || path[0] == '\0') return;

    char desktop_dir[DWM_MAX_PATH_LEN];
    if (dwm_desktop_get_directory(desktop_dir, sizeof(desktop_dir))) {
        char parent[DWM_MAX_PATH_LEN];
        memset(parent, '\0', sizeof(parent));
        strncpy(parent, path, sizeof(parent) - 1);

        char* slash = strrchr(parent, '/');
        if (slash != NULL) {
            const char* name = path + (slash - parent) + 1;   // Points into `path`

            if (slash == parent) parent[1] = '\0';            // Parent is "/"
            else                 *slash = '\0';

            if (name[0] != '\0' &&
                strcmp(parent, desktop_dir) == 0 &&
                !desktop_has_icon(path)) {
                dwm_desktop_add_item_icon(path, name, -1, -1);
            }
        }
    }

    dwm_refresh_all_windows();
}

// New folder or empty file in the desktop folder, icon at (x, y)
static bool desktop_create_item(bool folder, int x, int y) {
    char dir_path[DWM_MAX_PATH_LEN];
    if (!dwm_desktop_get_directory(dir_path, sizeof(dir_path)))
        return false;

    char item_name[DWM_MAX_PATH_LEN];
    char item_path[DWM_MAX_PATH_LEN];
    name_get_unique(dir_path, folder ? "new_folder" : "new_file", item_name, item_path);

    if (folder) {
        if (!vfs_mkdir(item_path)) return false;
        dwm_create_folder((uint16_t)x, (uint16_t)y, item_name, item_path);
    } else {
        File item_file = vfs_open(item_path, VFS_OPEN_CREATE);
        if (item_file == VFS_INVALID_FILE) return false;
        vfs_close(item_file);
        dwm_create_file((uint16_t)x, (uint16_t)y, item_name, item_path);
    }

    dwm_notify_item_created(item_path);
    return true;
}

bool dwm_desktop_create_folder(void) {
    DWM_GUARD();
    return desktop_create_item(true, input.mouse_last.x, input.mouse_last.y);
}

bool dwm_desktop_create_file(void) {
    DWM_GUARD();
    return desktop_create_item(false, input.mouse_last.x, input.mouse_last.y);
}

// ==========================================
// Built-in actions
// ==========================================
//
// Actions read only the context they are given. Ones that act on an item
// (cut, delete, ...) need an icon; ones that make sense anywhere (open,
// paste) use ctx->path, which on the desktop is the desktop folder.

static bool action_has_icon(const DWMActionContext* ctx) {
    return ctx->icon != NULL && ctx->path[0] != '\0';
}

static void action_refresh(const DWMActionContext* ctx, void* user) {
    (void)ctx; (void)user;
    dwm_refresh_all_windows();
}

static void action_new_folder(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (!desktop_create_item(true, ctx->x, ctx->y))
        dwm_notify("New folder", "The folder could not be created", DWM_NOTIFY_LONG_MS);
}

static void action_new_file(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (!desktop_create_item(false, ctx->x, ctx->y))
        dwm_notify("New file", "The file could not be created", DWM_NOTIFY_LONG_MS);
}

// On a folder (or storage) icon: paste into it. Anywhere else: the desktop,
// with the first icon where the menu was opened.
static void action_paste(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (action_has_icon(ctx) && vfs_directory_check(ctx->path)) {
        dwm_clipboard_paste_into(ctx->path, -1, -1);
        return;
    }

    char dir[DWM_MAX_PATH_LEN];
    if (!dwm_desktop_get_directory(dir, sizeof(dir))) {
        dwm_notify("Paste", "There is no desktop folder to paste into", DWM_NOTIFY_LONG_MS);
        return;
    }
    dwm_clipboard_paste_into(dir, ctx->x, ctx->y);
}

static void action_desktop_properties(const DWMActionContext* ctx, void* user) {
    (void)ctx; (void)user;
    dwm_notify("Desktop", "Desktop properties are not implemented yet", DWM_NOTIFY_DEFAULT_MS);
}

// Folders open in the explorer, programs (execute bit set) run, other files
// open in notepad. On the desktop this opens the desktop folder itself.
static void action_open(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (ctx->path[0] == '\0') return;

    if (vfs_directory_check(ctx->path)) {
        dwm_kernel_event_send(KEVENT_EXECUTE, "explorer", ctx->path);
    } else if (dwm_path_is_executable(ctx->path)) {
        dwm_launch_program(ctx->path);
    } else {
        dwm_kernel_event_send(KEVENT_EXECUTE, "notepad", ctx->path);
    }
}

static void action_cut(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (action_has_icon(ctx)) dwm_clipboard_put_file(ctx->path, DWM_CLIPBOARD_OP_CUT);
}

static void action_copy(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (action_has_icon(ctx)) dwm_clipboard_put_file(ctx->path, DWM_CLIPBOARD_OP_COPY);
}

static void action_rename(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (action_has_icon(ctx)) dwm_desktop_rename_begin(ctx->icon);
}

static void action_delete(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (!action_has_icon(ctx)) return;
    // No parent window: the desktop has none
    dwm_summon_dialog_delete("Deletion request", ctx->path, 0, ctx->icon->icon_index);
}

static void action_properties(const DWMActionContext* ctx, void* user) {
    (void)user;
    if (!action_has_icon(ctx)) return;
    dwm_summon_properties("Properties", ctx->icon->name, ctx->path, ctx->icon->icon_index);
}

static const struct {
    const char* name;
    DWMActionFn fn;
} builtin_actions[] = {
    { "refresh",            action_refresh            },
    { "new_folder",         action_new_folder         },
    { "new_file",           action_new_file           },
    { "paste",              action_paste              },
    { "desktop_properties", action_desktop_properties },
    { "open",               action_open               },
    { "cut",                action_cut                },
    { "copy",               action_copy               },
    { "rename",             action_rename             },
    { "delete",             action_delete             },
    { "properties",         action_properties         },
};

#define BUILTIN_ACTION_COUNT  (sizeof(builtin_actions) / sizeof(builtin_actions[0]))

// ==========================================
// Menu definitions (registry)
// ==========================================

typedef struct {
    char order[REGISTRY_MAX_NAME_LEN];
    char label[MENU_LABEL_MAX];
    char action[DWM_ACTION_NAME_MAX];
} MenuEntry;

static bool order_is_number(const char* s) {
    if (*s == '\0') return false;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

// Numbers as numbers ("9" < "10"), everything else alphabetically
static int order_cmp(const char* a, const char* b) {
    if (order_is_number(a) && order_is_number(b)) {
        while (a[0] == '0' && a[1] != '\0') a++;
        while (b[0] == '0' && b[1] != '\0') b++;

        size_t la = strlen(a);
        size_t lb = strlen(b);
        if (la != lb) return (la < lb) ? -1 : 1;
    }
    return strcmp(a, b);
}

// Sorted insert into a list capped at MENU_ITEMS_MAX (the last one falls
// off when it is full). Sorting matters: the registry keeps keys in no
// particular order, and a save/load cycle reverses them.
static void menu_insert(MenuEntry* list, uint8_t* count, const MenuEntry* entry) {
    uint8_t pos = *count;
    while (pos > 0 && order_cmp(entry->order, list[pos - 1].order) < 0) pos--;
    if (pos >= MENU_ITEMS_MAX) return;

    uint8_t last = (*count < MENU_ITEMS_MAX) ? *count : (uint8_t)(MENU_ITEMS_MAX - 1);
    memmove(&list[pos + 1], &list[pos], (size_t)(last - pos) * sizeof(MenuEntry));
    list[pos] = *entry;

    if (*count < MENU_ITEMS_MAX) (*count)++;
}

// The menu's key, or NULL when it (or the registry) isn't there
static struct RegistryKey* menu_reg_key(const char* menu_name) {
    if (hkey_root.root == NULL) return NULL;

    struct RegistryKey* menus = registry_open_key(hkey_root.root, MENU_REG_PATH, false, 0);
    return (menus != NULL) ? registry_get_key(menus, menu_name) : NULL;
}

// Write the built-in items into the registry, only when the menu key is
// missing entirely (an existing key, even an empty one, is the user's)
static void menu_seed(const char* menu_name, const MenuDefault* defaults, size_t count) {
    if (hkey_root.root == NULL) return;

    struct RegistryKey* menus = registry_open_key(hkey_root.root, MENU_REG_PATH, true, MENU_REG_PERMS);
    if (menus == NULL || registry_get_key(menus, menu_name) != NULL) return;

    struct RegistryKey* menu = registry_create_key(menus, menu_name, MENU_REG_PERMS);
    if (menu == NULL) return;

    for (size_t i = 0; i < count; i++) {
        struct RegistryKey* item = registry_create_key(menu, defaults[i].order, MENU_REG_PERMS);
        if (item == NULL) continue;

        registry_set_string(item, "label",  MENU_REG_PERMS, defaults[i].label);
        registry_set_string(item, "action", MENU_REG_PERMS, defaults[i].action);
    }
}

// Visible items, in order
static uint8_t menu_load(const char* menu_name, const MenuDefault* defaults, size_t default_count,
                         MenuEntry* out) {
    uint8_t count = 0;
    MenuEntry entry;

    struct RegistryKey* menu = menu_reg_key(menu_name);

    // No registry (or the key was deleted this session): built-in items
    if (menu == NULL) {
        for (size_t i = 0; i < default_count; i++) {
            if (!dwm_action_exists(defaults[i].action)) continue;

            memset(&entry, 0, sizeof(entry));
            strncpy(entry.order,  defaults[i].order,  sizeof(entry.order) - 1);
            strncpy(entry.label,  defaults[i].label,  sizeof(entry.label) - 1);
            strncpy(entry.action, defaults[i].action, sizeof(entry.action) - 1);
            menu_insert(out, &count, &entry);
        }
        return count;
    }

    for (struct RegistryKey* item = menu->child_keys; item != NULL; item = item->next) {
        if (!(item->permissions & REGISTRY_PERMISSION_READ)) continue;

        memset(&entry, 0, sizeof(entry));
        strncpy(entry.order, item->name, sizeof(entry.order) - 1);

        if (!registry_get_string(item, "label", entry.label, sizeof(entry.label)) ||
            entry.label[0] == '\0')
            continue;

        if (!registry_get_string(item, "action", entry.action, sizeof(entry.action)) ||
            !dwm_action_exists(entry.action))
            continue;

        menu_insert(out, &count, &entry);
    }

    return count;
}

// ==========================================
// Open menu
// ==========================================
//
// The action names behind the menu currently on screen. Kept as names,
// resolved when an item is picked, so nothing dangles if an action is
// unregistered while the menu is open.

static struct {
    DWMActionSource source;
    uint8_t         count;
    int             x, y;
    char            actions[MENU_ITEMS_MAX][DWM_ACTION_NAME_MAX];
} open_menu;

static void menu_open(struct WindowContext* ctx, DWMActionSource source, uint32_t directive,
                      const char* menu_name, const MenuDefault* defaults, size_t default_count) {
    MenuEntry entries[MENU_ITEMS_MAX];
    open_menu.count = 0;

    uint8_t count = menu_load(menu_name, defaults, default_count, entries);
    if (count == 0) return;     // Nothing to show (all hidden, or an empty menu)

    const char* labels[MENU_ITEMS_MAX];
    for (uint8_t i = 0; i < count; i++) {
        labels[i] = entries[i].label;
        memcpy(open_menu.actions[i], entries[i].action, DWM_ACTION_NAME_MAX);
    }

    if (!dwm_create_context_menu(ctx->mouse.x, ctx->mouse.y, directive, labels, count))
        return;

    open_menu.source = source;
    open_menu.count  = count;

    // Where the menu actually landed (kept on screen above the taskbar)
    open_menu.x = ctxmenu.menus[0].x;
    open_menu.y = ctxmenu.menus[0].y;
}

static void menu_pick(DWMActionSource source, uint16_t index) {
    if (open_menu.source != source || index >= open_menu.count) return;

    char action[DWM_ACTION_NAME_MAX];
    memcpy(action, open_menu.actions[index], sizeof(action));
    open_menu.count = 0;

    char desktop_dir[DWM_MAX_PATH_LEN];
    desktop_dir[0] = '\0';

    DWMActionContext actx;
    actx.source = source;
    actx.icon   = NULL;
    actx.path   = "";
    actx.x      = open_menu.x;
    actx.y      = open_menu.y;

    if (source == DWM_ACTION_SOURCE_ICON) {
        // Cleared by dwm_destroy_icon if the icon went away meanwhile
        actx.icon = context.focused_icon;
        if (actx.icon != NULL) actx.path = actx.icon->path;
    } else if (dwm_desktop_get_directory(desktop_dir, sizeof(desktop_dir))) {
        actx.path = desktop_dir;
    }

    if (!dwm_action_invoke(action, &actx))
        dwm_notify("Menu", "That action is no longer available", DWM_NOTIFY_DEFAULT_MS);
}

// ==========================================
// Public
// ==========================================

void dwm_context_menu_init(void) {
    DWM_GUARD();

    for (size_t i = 0; i < BUILTIN_ACTION_COUNT; i++)
        dwm_action_register(builtin_actions[i].name, builtin_actions[i].fn, NULL);

    menu_seed(MENU_NAME_DESKTOP, desktop_defaults, DESKTOP_DEFAULT_COUNT);
    menu_seed(MENU_NAME_ICON,    icon_defaults,    ICON_DEFAULT_COUNT);
}

void dwm_icon_right_click(struct WindowContext* ctx) {
    menu_open(ctx, DWM_ACTION_SOURCE_ICON, DWM_CONTEXT_MENU_ICON,
              MENU_NAME_ICON, icon_defaults, ICON_DEFAULT_COUNT);
}

void dwm_desktop_right_click(struct WindowContext* ctx) {
    menu_open(ctx, DWM_ACTION_SOURCE_DESKTOP, DWM_CONTEXT_MENU_DESKTOP,
              MENU_NAME_DESKTOP, desktop_defaults, DESKTOP_DEFAULT_COUNT);
}

void dwm_desktop_context_callback(struct WindowContext* ctx, uint16_t index) {
    (void)ctx;
    menu_pick(DWM_ACTION_SOURCE_DESKTOP, index);
}

void dwm_icon_context_callback(struct WindowContext* ctx, uint16_t index) {
    (void)ctx;
    menu_pick(DWM_ACTION_SOURCE_ICON, index);
}
