#ifndef _KERNEL_REGISTRY_H_
#define _KERNEL_REGISTRY_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/vfs/vfs.h>

#define REGISTRY_PERMISSION_READ        0x01
#define REGISTRY_PERMISSION_WRITE       0x02

#define REGISTRY_MAX_NAME_LEN     16
#define REGISTRY_MAX_PATH_LEN    128

// Limits applied when importing a hive file (protects against corrupt/hostile files)
#define REGISTRY_MAX_VALUE_SIZE   (64U * 1024U)
#define REGISTRY_MAX_DEPTH        32

struct RegistryValue {
    char name[REGISTRY_MAX_NAME_LEN];
    struct RegistryValue* next;        // Sibling values under the same key
    void* data;                        // Dynamic data payload
    size_t data_len;                   // Length of the payload
    uint16_t permissions;
};

struct RegistryKey {
    char name[REGISTRY_MAX_NAME_LEN];
    struct RegistryKey* next;          // Sibling keys under the same parent
    struct RegistryKey* child_keys;    // Head of child keys list
    struct RegistryValue* values;      // Head of values list inside this key
    uint16_t permissions;
};

struct RegistryHive {
    struct RegistryKey* root;
};

// registry_get/set_permissions() accept either a RegistryKey* or a RegistryValue*
// and read the field through RegistryKey. That only works while the field sits
// at the same offset in both structs, so enforce it at compile time.
_Static_assert(offsetof(struct RegistryKey, permissions) == offsetof(struct RegistryValue, permissions),
               "RegistryKey/RegistryValue permissions must share an offset");

extern struct RegistryHive hkey_root;
extern struct RegistryHive hkey_user;

bool registry_hive_initiate(const char* path);

struct RegistryKey* registry_create_key(struct RegistryKey* parent, const char* name, uint16_t permissions);
struct RegistryValue* registry_create_value(struct RegistryKey* parent, const char* name, uint16_t permissions, const void* data, size_t size);
void registry_free_key(struct RegistryKey* key);

uint16_t registry_get_permissions(void* ptr);
void registry_set_permissions(void* ptr, uint16_t permissions);

struct RegistryValue* registry_get_value(struct RegistryKey* parent, const char* name);
struct RegistryKey* registry_get_key(struct RegistryKey* parent, const char* name);

bool registry_hive_import(struct RegistryHive* hive, const char* path);
bool registry_hive_export(struct RegistryHive* hive, const char* path);

// Write both hives back to the files they were loaded from by
// registry_hive_initiate(). Returns false if either hive failed to save
// (or the registry was never initiated).
bool registry_hive_save_all(void);

//
// Convenience helpers (registry_util.c)
//
// The registry has no lock of its own. Today only the boot thread (load)
// touches it; anything else that starts using it needs a lock first.
//

// Walk a '/'-separated path below `root` ("software/shell/menus"). Every
// segment must be shorter than REGISTRY_MAX_NAME_LEN. With `create`, missing
// keys are made with `permissions`; without it, NULL means "not there".
struct RegistryKey* registry_open_key(struct RegistryKey* root, const char* path,
                                      bool create, uint16_t permissions);

// Create the value, or replace the data of an existing one (no duplicate
// values with the same name, unlike registry_create_value). On failure the
// old data is kept.
bool registry_set_value(struct RegistryKey* parent, const char* name, uint16_t permissions,
                        const void* data, size_t size);

// Store a string with its '\0'
bool registry_set_string(struct RegistryKey* parent, const char* name, uint16_t permissions,
                         const char* text);

// Read a value as a string into `out` (always terminated, cut to fit).
// Returns false, with out = "", when the value does not exist.
bool registry_get_string(struct RegistryKey* parent, const char* name, char* out, size_t size);

#endif

