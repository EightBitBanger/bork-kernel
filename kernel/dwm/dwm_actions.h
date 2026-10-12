#ifndef _DWM_ACTIONS_H_
#define _DWM_ACTIONS_H_

// Named action events.
//
// An action is a function registered under a short name ("open", "paste",
// "new_folder", ...). Anything that stores *what to do* (the registry's
// context menu entries, later maybe key bindings or toolbar buttons) stores
// the NAME, never a function pointer. The name is resolved when the action
// runs, so a stale or misspelled entry simply does nothing.
//
// Registering a name that already exists replaces it. A program can
// override a built-in action ("open" -> its own viewer), or take over an
// action that nothing provided yet.
//
// Threading: every function takes the DWM lock. Actions run on the DWM
// thread with the DWM lock held, exactly like window procedures, so they
// must not block (no event_wait, no long disk work: hand that to a thread).

#include <stdint.h>
#include <stdbool.h>

struct IconObject;

#define DWM_ACTION_NAME_MAX     32      // Including the '\0'
#define DWM_ACTION_MAX          64      // Registered at once

typedef enum {
    DWM_ACTION_SOURCE_DESKTOP = 0,      // Right click on empty desktop
    DWM_ACTION_SOURCE_ICON    = 1,      // Right click on a desktop icon
} DWMActionSource;

// What an action is being applied to
typedef struct {
    DWMActionSource    source;
    struct IconObject* icon;    // Icon the menu was opened on. NULL for the
                                // desktop, or when the icon is gone by now.
    const char*        path;    // icon->path, or the desktop folder. Never
                                // NULL; "" when there is none.
    int                x, y;    // Screen point the menu was opened at
} DWMActionContext;

typedef void (*DWMActionFn)(const DWMActionContext* ctx, void* user);

// Register (or replace) `name`. `user` is handed back on every call.
// Fails on an empty or too long name, a NULL fn, or a full table.
bool dwm_action_register(const char* name, DWMActionFn fn, void* user);

// Remove `name`. Returns false when it was not registered.
bool dwm_action_unregister(const char* name);

bool dwm_action_exists(const char* name);

// Run `name`. Returns false when no such action is registered.
// ctx may be NULL (the action then sees an empty desktop context).
bool dwm_action_invoke(const char* name, const DWMActionContext* ctx);

#endif
