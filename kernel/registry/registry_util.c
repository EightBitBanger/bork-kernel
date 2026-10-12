#include <kernel/malloc.h>
#include <kernel/registry/registry.h>
#include <kernel/util/string.h>

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
