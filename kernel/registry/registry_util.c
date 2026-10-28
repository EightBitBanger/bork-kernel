#include <kernel/malloc.h>
#include <kernel/registry/registry.h>
#include <kernel/mutex.h>
#include <kernel/util/string.h>

// Zeroed is unlocked (mutex_init only clears it), so no init call is needed
static mutex_t registry_mutex;

void registry_lock(void) {
    mutex_lock(&registry_mutex);
}

void registry_unlock(void) {
    mutex_unlock(&registry_mutex);
}

struct RegistryKey* registry_open_key(struct RegistryKey* root, const char* path,
                                      bool create, uint16_t permissions) {
    if (root == NULL || path == NULL) return NULL;
    
    struct RegistryKey* key = root;
    const char* p = path;
    char segment[REGISTRY_MAX_NAME_LEN];
    
    while (*p != '\0') {
        while (*p == '/') p++;
        if (*p == '\0') break;
        
        size_t n = 0;
        while (p[n] != '\0' && p[n] != '/') n++;
        if (n >= REGISTRY_MAX_NAME_LEN) return NULL;   // Would be cut, and then never match
        
        memcpy(segment, p, n);
        segment[n] = '\0';
        p += n;
        
        struct RegistryKey* child = registry_get_key(key, segment);
        if (child == NULL) {
            if (!create) return NULL;
            child = registry_create_key(key, segment, permissions);
            if (child == NULL) return NULL;
        }
        key = child;
    }
    
    return key;
}

bool registry_set_value(struct RegistryKey* parent, const char* name, uint16_t permissions,
                        const void* data, size_t size) {
    if (parent == NULL || name == NULL) return false;
    if (data == NULL) size = 0;
    
    // Same cap the importer enforces: never write what can't be read back
    if (size > REGISTRY_MAX_VALUE_SIZE) return false;
    
    struct RegistryValue* existing = registry_get_value(parent, name);
    if (existing == NULL)
        return registry_create_value(parent, name, permissions, data, size) != NULL;
    
    void* copy = NULL;
    if (size > 0) {
        copy = malloc(size);
        if (copy == NULL) return false;
        memcpy(copy, data, size);
    }
    
    if (existing->data != NULL) free(existing->data);
    existing->data        = copy;
    existing->data_len    = size;
    existing->permissions = permissions;
    return true;
}

bool registry_set_string(struct RegistryKey* parent, const char* name, uint16_t permissions,
                         const char* text) {
    if (text == NULL) text = "";
    return registry_set_value(parent, name, permissions, text, strlen(text) + 1);
}

bool registry_get_string(struct RegistryKey* parent, const char* name, char* out, size_t size) {
    if (out == NULL || size == 0) return false;
    out[0] = '\0';
    
    struct RegistryValue* value = registry_get_value(parent, name);
    if (value == NULL) return false;
    
    // Stored strings carry their '\0'; data from elsewhere may not
    size_t n = value->data_len;
    if (n > size - 1) n = size - 1;
    if (n > 0 && value->data != NULL) memcpy(out, value->data, n);
    out[n] = '\0';
    return true;
}

void registry_clear_key(struct RegistryKey* key) {
    if (key == NULL) return;
    
    struct RegistryKey* child = key->child_keys;
    while (child != NULL) {
        struct RegistryKey* next = child->next;
        registry_free_key(child);
        child = next;
    }
    key->child_keys = NULL;
    
    struct RegistryValue* value = key->values;
    while (value != NULL) {
        struct RegistryValue* next = value->next;
        if (value->data != NULL) free(value->data);
        free(value);
        value = next;
    }
    key->values = NULL;
}
