#ifndef CONSOLE_CMD_POLICY_H
#define CONSOLE_CMD_POLICY_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/kernel.h>
#include <kernel/vfs/vfs.h>
#include <kernel/security/policy.h>
#include <kernel/util/snprint.h>
#include <kernel/console/commands/path_util.h>

// Group policies (kernel/security/policy.h)
//
//   policy                          list the policies
//   policy <path> [path...]         the policy and security mask of each
//   policy set <name> <path...>     put them in policy <name> ("admin", or
//                                   "none" to take them out of their group)
//
// Setting a policy sets the security mask to the policy's clearance (what a
// process needs to use the file). Only files and folders on a storage
// device that keeps policies (basefs). The console runs with every
// clearance, so it may set any of them.

static void policy_print_line(const char* name, const char* a, const char* b) {
    char line[160];
    snprintf(line, sizeof(line), "%-24s %-10s %s\n", name, a, b);
    print(line);
}

static void policy_print_table(void) {
    print("id  name     permissions  clearance\n");
    for (uint32_t i = 0; policy_at(i) != NULL; i++) {
        const Policy* p = policy_at(i);
        char line[160];
        snprintf(line, sizeof(line), "%-3u %-8s 0x%08x   0x%08x  %s\n", (unsigned)p->id, p->name,
                 (unsigned)p->permissions, (unsigned)policy_clearance(p->id), p->description);
        print(line);
    }
}

static int cmd_policy_run(int arg_count, char** args) {
    if (arg_count == 0) {
        policy_print_table();
        return 0;
    }

    bool set = strcmp(args[0], "set") == 0;
    uint32_t id = POLICY_NONE;
    int first = 0;
    if (set) {
        if (arg_count < 3) {
            print("policy set <name> <path> [path...]\n");
            return 1;
        }
        const Policy* p = policy_find_name(args[1]);
        if (p == NULL && strcmp(args[1], "none") != 0) {
            print(args[1]);
            print(": no such policy (policy lists them)\n");
            return 1;
        }
        id = (p != NULL) ? p->id : POLICY_NONE;
        first = 2;
    }

    int result = 0;
    for (int i = first; i < arg_count; i++) {
        char path[VFS_PATH_MAX];
        if (!command_resolve_path(args[i], path, sizeof(path))) {
            result = 2;
            continue;
        }
        uint32_t current, mask;
        if (!vfs_exists(path)) {
            print(args[i]);
            print(": not found\n");
            result = 3;
            continue;
        }
        if (!vfs_get_policy(path, &current) || !vfs_get_security(path, &mask)) {
            print(args[i]);
            print(": not on a storage device that keeps policies\n");
            result = 4;
            continue;
        }
        if (set) {
            if (!vfs_set_policy(path, id)) {
                print(args[i]);
                print(": cannot change its policy\n");
                result = 5;
                continue;
            }
            vfs_get_policy(path, &current);
            vfs_get_security(path, &mask);
        }
        const Policy* p = policy_find(current);
        char group[16], security[48];
        uint32_t write_mask = 0;
        vfs_get_write_security(path, &write_mask);
        if (p != NULL)
            snprintf(group, sizeof(group), "%s", p->name);
        else if (current == POLICY_NONE)
            snprintf(group, sizeof(group), "none");
        else
            snprintf(group, sizeof(group), "? (%u)", (unsigned)current);
        snprintf(security, sizeof(security), "security 0x%08x write +0x%08x", (unsigned)mask, (unsigned)write_mask);
        policy_print_line(args[i], group, security);
    }
    return result;
}

int console_cmd_policy(int arg_count, char** args) {
    int result = cmd_policy_run(arg_count, args);
    console_command_end();
    return result;
}

#endif
