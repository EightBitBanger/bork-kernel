#include <kernel/dwm/dwm_actions.h>
#include <kernel/dwm/dwm_core_internal.h>

#include <kernel/util/string.h>

// A flat table: a few dozen entries, looked up once per menu click. A hash
// map would be faster and bring nothing a user could notice.

struct DWMAction {
    char        name[DWM_ACTION_NAME_MAX];
    DWMActionFn fn;
    void*       user;
};

static struct DWMAction actions[DWM_ACTION_MAX];
static uint32_t         action_count = 0;

static bool action_name_valid(const char* name) {
    if (name == NULL || name[0] == '\0') return false;
    return strnlen(name, DWM_ACTION_NAME_MAX) < DWM_ACTION_NAME_MAX;
}

// DWM lock held
static int action_find(const char* name) {
    for (uint32_t i = 0; i < action_count; i++) {
        if (strcmp(actions[i].name, name) == 0) return (int)i;
    }
    return -1;
}

bool dwm_action_register(const char* name, DWMActionFn fn, void* user) {
    DWM_GUARD();
    if (!action_name_valid(name) || fn == NULL) return false;

    int index = action_find(name);
    if (index < 0) {
        if (action_count >= DWM_ACTION_MAX) return false;
        index = (int)action_count++;

        memset(&actions[index], 0, sizeof(struct DWMAction));
        strncpy(actions[index].name, name, DWM_ACTION_NAME_MAX - 1);
    }

    actions[index].fn   = fn;
    actions[index].user = user;
    return true;
}

bool dwm_action_unregister(const char* name) {
    DWM_GUARD();
    if (!action_name_valid(name)) return false;

    int index = action_find(name);
    if (index < 0) return false;

    // Order does not matter: move the last entry into the hole
    action_count--;
    if ((uint32_t)index != action_count)
        actions[index] = actions[action_count];
    memset(&actions[action_count], 0, sizeof(struct DWMAction));
    return true;
}

bool dwm_action_exists(const char* name) {
    DWM_GUARD();
    return action_name_valid(name) && action_find(name) >= 0;
}

bool dwm_action_invoke(const char* name, const DWMActionContext* ctx) {
    DWM_GUARD();
    if (!action_name_valid(name)) return false;

    int index = action_find(name);
    if (index < 0) return false;

    static const DWMActionContext empty = { DWM_ACTION_SOURCE_DESKTOP, NULL, "", -1, -1 };

    // Copied first: the action may (un)register actions and move the table
    DWMActionFn fn   = actions[index].fn;
    void*       user = actions[index].user;

    fn((ctx != NULL) ? ctx : &empty, user);
    return true;
}
