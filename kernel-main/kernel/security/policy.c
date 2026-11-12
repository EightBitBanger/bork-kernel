#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kernel/security/policy.h>
#include <kernel/scheduler/process.h>
#include <kernel/registry/registry.h>
#include <kernel/util/snprint.h>
#include <kernel/util/string.h>

_Static_assert((POLICY_USER_PERMISSIONS & ~PROCESS_SECURITY_DEFAULT) == 0,
               "the user policy is within what programs are started with");

static const Policy policy_table[] = {
    { POLICY_USER,    "user",    POLICY_USER_PERMISSIONS,
      "What programs started from the desktop hold" },
    { POLICY_ADMIN,   "admin",   POLICY_USER_PERMISSIONS | SYS_SEC_ADMIN,
      "User and administration" },
    { POLICY_SYSTEM,  "system",  SYS_SEC_VALID,
      "Every permission" },
    { POLICY_NETWORK, "network", POLICY_USER_PERMISSIONS | SYS_SEC_NET,
      "User and network: for files network programs use" },
};

#define POLICY_COUNT (sizeof(policy_table) / sizeof(policy_table[0]))

const Policy* policy_find(uint32_t id) {
    for (size_t i = 0; i < POLICY_COUNT; i++) {
        if (policy_table[i].id == id)
            return &policy_table[i];
    }
    return NULL;
}

const Policy* policy_find_name(const char* name) {
    if (name == NULL)
        return NULL;
    for (size_t i = 0; i < POLICY_COUNT; i++) {
        if (strcmp(policy_table[i].name, name) == 0)
            return &policy_table[i];
    }
    return NULL;
}

const Policy* policy_at(uint32_t index) {
    return (index < POLICY_COUNT) ? &policy_table[index] : NULL;
}

uint32_t policy_clearance(uint32_t id) {
    const Policy* policy = policy_find(id);
    return (policy != NULL) ? (policy->permissions & ~POLICY_USER_PERMISSIONS) : 0;
}

bool policy_is_valid(uint32_t id) {
    return id == POLICY_NONE || policy_find(id) != NULL;
}

//
// The registry listing
//

#define POLICY_VALUE_COUNT 4

typedef struct {
    const char* name;
    char        text[64];
} PolicyValue;

static void policy_values(const Policy* policy, PolicyValue out[POLICY_VALUE_COUNT]) {
    out[0].name = "id";
    snprintf(out[0].text, sizeof(out[0].text), "%u", (unsigned)policy->id);
    out[1].name = "permissions";
    snprintf(out[1].text, sizeof(out[1].text), "0x%08x", (unsigned)policy->permissions);
    out[2].name = "clearance";
    snprintf(out[2].text, sizeof(out[2].text), "0x%08x", (unsigned)policy_clearance(policy->id));
    out[3].name = "description";
    strncpy(out[3].text, policy->description, sizeof(out[3].text) - 1);
    out[3].text[sizeof(out[3].text) - 1] = '\0';
}

static uint32_t policy_count_keys(const struct RegistryKey* key) {
    uint32_t n = 0;
    for (const struct RegistryKey* k = key->child_keys; k != NULL; k = k->next) n++;
    return n;
}

static uint32_t policy_count_values(const struct RegistryKey* key) {
    uint32_t n = 0;
    for (const struct RegistryValue* v = key->values; v != NULL; v = v->next) n++;
    return n;
}

// `key` holds exactly the table, read-only: nothing to write
static bool policy_registry_matches(struct RegistryKey* key) {
    if (registry_get_permissions(key) != REGISTRY_PERMISSION_READ ||
        key->values != NULL || policy_count_keys(key) != POLICY_COUNT)
        return false;

    for (size_t i = 0; i < POLICY_COUNT; i++) {
        struct RegistryKey* entry = registry_get_key(key, policy_table[i].name);
        if (entry == NULL || entry->child_keys != NULL ||
            registry_get_permissions(entry) != REGISTRY_PERMISSION_READ ||
            policy_count_values(entry) != POLICY_VALUE_COUNT)
            return false;

        PolicyValue want[POLICY_VALUE_COUNT];
        policy_values(&policy_table[i], want);
        for (size_t v = 0; v < POLICY_VALUE_COUNT; v++) {
            struct RegistryValue* value = registry_get_value(entry, want[v].name);
            size_t length = strlen(want[v].text) + 1;
            if (value == NULL || registry_get_permissions(value) != REGISTRY_PERMISSION_READ ||
                value->data == NULL || value->data_len != length ||
                memcmp(value->data, want[v].text, length) != 0)
                return false;
        }
    }
    return true;
}

bool policy_registry_publish(struct RegistryHive* hive, bool* failed) {
    const uint16_t rw = REGISTRY_PERMISSION_READ | REGISTRY_PERMISSION_WRITE;
    const uint16_t ro = REGISTRY_PERMISSION_READ;

    if (hive == NULL || hive->root == NULL) {
        if (failed != NULL) *failed = true;
        return false;
    }

    struct RegistryKey* system = registry_open_key(hive->root, "system", true, rw);
    if (system == NULL) {
        if (failed != NULL) *failed = true;
        return false;
    }

    struct RegistryKey* key = registry_get_key(system, "policy");
    if (key != NULL && policy_registry_matches(key))
        return false;

    if (key == NULL)
        key = registry_create_key(system, "policy", ro);
    if (key == NULL) {
        if (failed != NULL) *failed = true;
        return false;
    }

    // Start over: whatever was there (an older table, or a hive edited
    // outside the kernel) goes
    registry_clear_key(key);
    registry_set_permissions(key, ro);

    bool ok = true;
    for (size_t i = 0; i < POLICY_COUNT; i++) {
        struct RegistryKey* entry = registry_create_key(key, policy_table[i].name, ro);
        if (entry == NULL) {
            ok = false;
            continue;
        }
        PolicyValue values[POLICY_VALUE_COUNT];
        policy_values(&policy_table[i], values);
        for (size_t v = 0; v < POLICY_VALUE_COUNT; v++)
            ok = registry_set_string(entry, values[v].name, ro, values[v].text) && ok;
    }

    if (!ok && failed != NULL)
        *failed = true;
    return true;
}
