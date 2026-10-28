#include <kernel/malloc.h>
#include <kernel/registry/registry.h>
#include <kernel/util/string.h>
#include <kernel/vfs/vfs.h>

#define _REGISTRY_MAGIC_ "REG!"

struct RegistryHive hkey_root = { NULL };
struct RegistryHive hkey_user = { NULL };

// Backing files for the hives, remembered by registry_hive_initiate() so the
// hives can be written back at shutdown. Empty until the registry is loaded.
static char hkr_hive_path[REGISTRY_MAX_PATH_LEN] = { 0 };
static char hku_hive_path[REGISTRY_MAX_PATH_LEN] = { 0 };

static size_t registry_calculate_total_export_size(struct RegistryHive* hive);
static size_t registry_calculate_key_size_recursive(struct RegistryKey* key);

// vfs_read/vfs_write return the byte count on success and -1 on error, so
// compare against the size: `!vfs_read(...)` would accept -1 and short reads.
static bool reg_read_exact(File file, void* buffer, uint32_t size) {
    if (size == 0) return true;
    return vfs_read(file, buffer, size) == (int32_t)size;
}

static bool reg_write_exact(File file, const void* buffer, uint32_t size) {
    if (size == 0) return true;
    return vfs_write(file, buffer, size) == (int32_t)size;
}

static void registry_copy_name(char* dest, const char* src) {
    strncpy(dest, src, REGISTRY_MAX_NAME_LEN);
    dest[REGISTRY_MAX_NAME_LEN - 1] = '\0';   // strncpy does not terminate a 16-char name
}

static void registry_free_value(struct RegistryValue* val) {
    if (!val) return;
    if (val->data) {
        free(val->data);
    }
    free(val);
}

struct RegistryKey* registry_create_key(struct RegistryKey* parent, const char* name, uint16_t permissions) {
    if (!name) return NULL;
    
    struct RegistryKey* new_key = (struct RegistryKey*)malloc(sizeof(struct RegistryKey));
    if (!new_key) return NULL;
    
    registry_copy_name(new_key->name, name);
    new_key->permissions = permissions;
    new_key->next = NULL;
    new_key->child_keys = NULL;
    new_key->values = NULL;
    
    // Link into parent branch
    if (parent) {
        new_key->next = parent->child_keys;
        parent->child_keys = new_key;
    }
    
    return new_key;
}

struct RegistryValue* registry_create_value(struct RegistryKey* parent, const char* name, uint16_t permissions, const void* data, size_t size) {
    if (!parent || !name) return NULL;
    
    struct RegistryValue* new_val = (struct RegistryValue*)malloc(sizeof(struct RegistryValue));
    if (!new_val) return NULL;
    
    registry_copy_name(new_val->name, name);
    new_val->permissions = permissions;
    new_val->data_len = size;
    new_val->next = NULL;
    
    if (data && size > 0) {
        new_val->data = malloc(size);
        if (!new_val->data) {
            free(new_val);
            return NULL;
        }
        memcpy(new_val->data, data, size);
    } else {
        new_val->data = NULL;
    }
    
    // Link into parent key's value list
    new_val->next = parent->values;
    parent->values = new_val;
    
    return new_val;
}

void registry_free_key(struct RegistryKey* key) {
    if (!key) return;
    
    struct RegistryKey* current_child = key->child_keys;
    while (current_child) {
        struct RegistryKey* next_child = current_child->next;
        registry_free_key(current_child);
        current_child = next_child;
    }
    
    struct RegistryValue* current_val = key->values;
    while (current_val) {
        struct RegistryValue* next_val = current_val->next;
        registry_free_value(current_val);
        current_val = next_val;
    }
    
    free(key);
}

uint16_t registry_get_permissions(void* ptr) {
    if (!ptr) return 0;
    return ((struct RegistryKey*)ptr)->permissions; 
}

void registry_set_permissions(void* ptr, uint16_t permissions) {
    if (!ptr) return;
    ((struct RegistryKey*)ptr)->permissions = permissions;
}

struct RegistryKey* registry_get_key(struct RegistryKey* parent, const char* name) {
    if (!parent || !name) return NULL;
    
    struct RegistryKey* current = parent->child_keys;
    while (current) {
        // Assuming your kernel string library uses standard strcmp semantics
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL; // Key not found
}

struct RegistryValue* registry_get_value(struct RegistryKey* parent, const char* name) {
    if (!parent || !name) return NULL;
    
    struct RegistryValue* current = parent->values;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL; // Value not found
}

static bool registry_export_key_recursive(struct RegistryKey* key, File file_handle) {
    if (!key) return true;
    
    // Write the key metadata out to disk
    if (!reg_write_exact(file_handle, key->name, REGISTRY_MAX_NAME_LEN)) return false;
    if (!reg_write_exact(file_handle, &key->permissions, sizeof(key->permissions))) return false;
    
    // Count and write the number of values directly under this key
    uint32_t val_count = 0;
    struct RegistryValue* v = key->values;
    while (v) {
        val_count++;
        v = v->next;
    }
    if (!reg_write_exact(file_handle, &val_count, sizeof(val_count))) return false;
    
    // Serialize each value associated with this key
    v = key->values;
    while (v) {
        if (!reg_write_exact(file_handle, v->name, REGISTRY_MAX_NAME_LEN)) return false;
        if (!reg_write_exact(file_handle, &v->permissions, sizeof(v->permissions))) return false;
        
        uint32_t val_size = (uint32_t)v->data_len;
        if (!reg_write_exact(file_handle, &val_size, sizeof(val_size))) return false;
        
        if (val_size > 0 && v->data) {
            if (!reg_write_exact(file_handle, v->data, val_size)) return false;
        }
        v = v->next;
    }
    
    // Count and write the number of subkeys to know how many branches follow
    uint32_t key_count = 0;
    struct RegistryKey* child = key->child_keys;
    while (child) {
        key_count++;
        child = child->next;
    }
    if (!reg_write_exact(file_handle, &key_count, sizeof(key_count))) return false;
    
    // Recursively export all child branches deeper in the tree
    child = key->child_keys;
    while (child) {
        if (!registry_export_key_recursive(child, file_handle)) return false;
        child = child->next;
    }
    
    return true;
}

// Returns the new key, or NULL on any error. On error the partially built
// subtree is either already linked under `parent` (and will be released when
// the caller frees the hive root) or, for the root itself, freed here.
static struct RegistryKey* registry_import_key_recursive(struct RegistryKey* parent, File file_handle, uint32_t depth) {
    char key_name[REGISTRY_MAX_NAME_LEN];
    uint16_t key_permissions;
    uint32_t val_count = 0;
    uint32_t key_count = 0;
    
    if (depth > REGISTRY_MAX_DEPTH) return NULL;
    
    // Read key metadata
    if (!reg_read_exact(file_handle, key_name, REGISTRY_MAX_NAME_LEN)) return NULL;
    if (!reg_read_exact(file_handle, &key_permissions, sizeof(key_permissions))) return NULL;
    key_name[REGISTRY_MAX_NAME_LEN - 1] = '\0';
    
    // Allocate current node and establish parent linkage automatically 
    struct RegistryKey* current_key = registry_create_key(parent, key_name, key_permissions);
    if (!current_key) return NULL;
    
    // Read values attached to this node
    if (!reg_read_exact(file_handle, &val_count, sizeof(val_count))) goto fail;
    for (uint32_t i = 0; i < val_count; i++) {
        char val_name[REGISTRY_MAX_NAME_LEN];
        uint16_t val_permissions;
        uint32_t val_size;
        void* val_data = NULL;
        
        if (!reg_read_exact(file_handle, val_name, REGISTRY_MAX_NAME_LEN)) goto fail;
        if (!reg_read_exact(file_handle, &val_permissions, sizeof(val_permissions))) goto fail;
        if (!reg_read_exact(file_handle, &val_size, sizeof(val_size))) goto fail;
        val_name[REGISTRY_MAX_NAME_LEN - 1] = '\0';
        
        if (val_size > REGISTRY_MAX_VALUE_SIZE) goto fail;
        
        if (val_size > 0) {
            val_data = malloc(val_size);
            if (!val_data) goto fail;
            if (!reg_read_exact(file_handle, val_data, val_size)) {
                free(val_data);
                goto fail;
            }
        }
        
        struct RegistryValue* created = registry_create_value(current_key, val_name, val_permissions, val_data, val_size);
        if (val_data) {
            free(val_data);
        }
        if (!created) goto fail;
    }
    
    // Process nested child blocks matching the exported topology structure
    if (!reg_read_exact(file_handle, &key_count, sizeof(key_count))) goto fail;
    for (uint32_t i = 0; i < key_count; i++) {
        if (!registry_import_key_recursive(current_key, file_handle, depth + 1)) goto fail;
    }
    
    return current_key;
    
fail:
    // Children are linked into current_key, so freeing the root of the
    // failed import releases everything. Non-root keys stay linked under
    // their parent and are released by the caller.
    if (parent == NULL) {
        registry_free_key(current_key);
    }
    return NULL;
}

bool registry_hive_import(struct RegistryHive* hive, const char* path) {
    if (!hive || !path) return false;
    
    File file = vfs_open(path, VFS_OPEN_READ);
    if (file == VFS_INVALID_FILE) return false;
    
    // Verify the 4-byte magic header
    char magic[4];
    if (!reg_read_exact(file, magic, 4) || memcmp(magic, _REGISTRY_MAGIC_, 4) != 0) {
        vfs_close(file);
        return false; // Invalid or missing header
    }
    
    // Clean current state if a hive tree structure exists before overwriting
    if (hive->root) {
        registry_free_key(hive->root);
        hive->root = NULL;
    }
    
    uint8_t has_root = 0;
    // Read the presence indicator for this specific hive
    if (!reg_read_exact(file, &has_root, sizeof(has_root))) {
        vfs_close(file);
        return false;
    }
    if (has_root) {
        hive->root = registry_import_key_recursive(NULL, file, 0);
        if (!hive->root) { // The hive import failed
            vfs_close(file);
            return false;
        }
    }
    
    vfs_close(file);
    return true;
}

bool registry_hive_export(struct RegistryHive* hive, const char* path) {
    if (!hive || !path) return false;
    
    // Include 4 extra bytes for the magic header in the total size calculation
    size_t total_required_size = registry_calculate_total_export_size(hive) + 4;
    
    if (!vfs_exists(path)) {
        File file = vfs_open(path, VFS_OPEN_WRITE | VFS_OPEN_CREATE);
        if (file == VFS_INVALID_FILE) return false;
        
        vfs_close(file);
    }
    
    if (!vfs_truncate(path, total_required_size)) 
        return false;
    
    File file = vfs_open(path, VFS_OPEN_WRITE);
    if (file == VFS_INVALID_FILE) return false;
    
    // Write the 4-byte magic header string
    if (!reg_write_exact(file, _REGISTRY_MAGIC_, 4)) {
        vfs_close(file);
        return false;
    }
    
    uint8_t has_root = (hive->root != NULL) ? 1 : 0;
    
    if (!reg_write_exact(file, &has_root, sizeof(has_root))) {
        vfs_close(file);
        return false;
    }
    
    if (hive->root) {
        if (!registry_export_key_recursive(hive->root, file)) {
            vfs_close(file);
            return false;
        }
    }
    
    vfs_close(file);
    return true;
}

static size_t registry_calculate_key_size_recursive(struct RegistryKey* key) {
    if (!key) return 0;
    
    size_t total_size = 0;
    
    total_size += REGISTRY_MAX_NAME_LEN;            // key->name
    total_size += sizeof(key->permissions);         // key->permissions
    total_size += sizeof(uint32_t);                 // val_count indicator
    
    // Size of all attached values
    uint32_t val_count = 0;
    struct RegistryValue* v = key->values;
    while (v) {
        total_size += REGISTRY_MAX_NAME_LEN;        // v->name
        total_size += sizeof(v->permissions);       // v->permissions
        total_size += sizeof(uint32_t);             // v->data_len (serialized as uint32_t)
        total_size += v->data_len;                  // Actual dynamic payload data
        
        val_count++;
        v = v->next;
    }
    
    // Subkey count indicator
    total_size += sizeof(uint32_t);                 // key_count indicator
    
    // Recursively add the sizes of all subkeys
    struct RegistryKey* child = key->child_keys;
    while (child) {
        total_size += registry_calculate_key_size_recursive(child);
        child = child->next;
    }
    
    return total_size;
}

static size_t registry_calculate_total_export_size(struct RegistryHive* hive) {
    size_t total_file_size = 0;
    
    if (!hive) return 0;
    
    // Every hive file gets 1 byte for its 'has_root' presence indicator flag
    total_file_size++;
    
    if (hive->root) {
        total_file_size += registry_calculate_key_size_recursive(hive->root);
    }
    
    return total_file_size;
}

bool registry_hive_initiate(const char* path) {
    if (!path) return false;
    
    char hkr_fpath[REGISTRY_MAX_PATH_LEN];
    char hku_fpath[REGISTRY_MAX_PATH_LEN];
    
    strncpy(hkr_fpath, path, REGISTRY_MAX_PATH_LEN);
    strncpy(hku_fpath, path, REGISTRY_MAX_PATH_LEN);
    hkr_fpath[REGISTRY_MAX_PATH_LEN - 1] = '\0';
    hku_fpath[REGISTRY_MAX_PATH_LEN - 1] = '\0';
    
    // Note: kernel strncat takes the TOTAL buffer size (strlcat semantics)
    strncat(hkr_fpath, "/hkr", REGISTRY_MAX_PATH_LEN);
    strncat(hku_fpath, "/hku", REGISTRY_MAX_PATH_LEN);
    
    // Remember where each hive lives so registry_hive_save_all() can write it back
    strncpy(hkr_hive_path, hkr_fpath, REGISTRY_MAX_PATH_LEN);
    strncpy(hku_hive_path, hku_fpath, REGISTRY_MAX_PATH_LEN);
    hkr_hive_path[REGISTRY_MAX_PATH_LEN - 1] = '\0';
    hku_hive_path[REGISTRY_MAX_PATH_LEN - 1] = '\0';
    
    uint16_t perms = (REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE);
    bool ok = true;
    
    if (!registry_hive_import(&hkey_root, hkr_fpath)) {
        if (vfs_exists(hkr_fpath)) 
            vfs_remove(hkr_fpath);
        
        File file = vfs_open(hkr_fpath, VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE);
        if (file != VFS_INVALID_FILE) vfs_close(file);
        
        // Initiate root registry
        if (!hkey_root.root)  hkey_root.root  = registry_create_key(NULL, "hkey_root",  perms);
        
        if (hkey_root.root) {
            if (!registry_get_key(hkey_root.root, "software"))
                registry_create_key(hkey_root.root, "software", perms);
            if (!registry_hive_export(&hkey_root, hkr_fpath)) ok = false;
        } else {
            ok = false;
        }
    }
    
    if (!registry_hive_import(&hkey_user, hku_fpath)) {
        if (vfs_exists(hku_fpath)) 
            vfs_remove(hku_fpath);
        
        File file = vfs_open(hku_fpath, VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE);
        if (file != VFS_INVALID_FILE) vfs_close(file);
        
        if (!hkey_user.root)  hkey_user.root  = registry_create_key(NULL, "hkey_user", perms);
        
        if (hkey_user.root) {
            if (!registry_hive_export(&hkey_user, hku_fpath)) ok = false;
        } else {
            ok = false;
        }
    }
    
    return ok;
}

const char* registry_hive_path(const struct RegistryHive* hive) {
    if (hive == &hkey_root) return hkr_hive_path;
    if (hive == &hkey_user) return hku_hive_path;
    return "";
}

bool registry_hive_save_all(void) {
    // Never initiated: there is nothing loaded and no known file to write to
    if (hkr_hive_path[0] == '\0' || hku_hive_path[0] == '\0')
        return false;
    
    bool ok = true;
    
    // Save both even if the first fails, so one bad hive doesn't cost the other
    if (!registry_hive_export(&hkey_root, hkr_hive_path)) ok = false;
    if (!registry_hive_export(&hkey_user, hku_hive_path)) ok = false;
    
    return ok;
}
