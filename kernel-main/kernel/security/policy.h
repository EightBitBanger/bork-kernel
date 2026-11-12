#ifndef _KERNEL_POLICY_H_
#define _KERNEL_POLICY_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/registry/registry.h>

//
// Group policies
//
// Every object on a basefs volume carries a 32-bit policy id
// (FSBlockHeader.policy): the group it belongs to. A policy is a named set
// of SYS_SEC_* permissions. The table is built into the kernel and is the
// only thing the kernel goes by; it is listed, read-only, in the registry
// under hkey_root/system/policy so programs and the user can see it:
//
//     id  name     permissions                         clearance a file asks for
//     0   (none)   -                                   - (security is all there is)
//     1   user     what the desktop starts programs    0 (any process)
//                  with: PROCESS_SECURITY_DEFAULT
//                  without SYS_SEC_CONSOLE
//     2   admin    user + SYS_SEC_ADMIN                SYS_SEC_ADMIN
//     3   system   every permission (SYS_SEC_VALID)    everything user lacks
//     4   network  user + SYS_SEC_NET                  SYS_SEC_NET
//
// "Clearance" is what a process must hold to use a file in that group: the
// policy's permissions beyond the user policy's. SYS_SEC_CONSOLE (printing
// on the kernel's own console) counts as one of those: a file can't ask
// for it in "user", only in "system". Every program holds the
// user permissions, so they would only shut out a console tool started
// with a narrower mask, which is not what "user" means.
//
// How it fits with the file's `security` mask (vfs_security.c, unchanged):
// the security mask is still what every access is checked against. Setting
// a policy (vfs_set_policy) adds the policy's clearance to the mask, so a
// file put in "admin" needs admin, one in "network" needs SYS_SEC_NET, and
// clears the bits the policy doesn't hold (a smaller policy takes them
// away). From then on the mask is the file's current state: bits the
// policy holds can be set or cleared by themselves (SYS_SETSECURITY
// refuses others) without changing the group.
//

#define POLICY_NONE         0u
#define POLICY_USER         1u
#define POLICY_ADMIN        2u
#define POLICY_SYSTEM       3u
#define POLICY_NETWORK      4u

// What the desktop, the explorer and the console start programs with (their
// LAUNCH_MASK). Within the scheduler's PROCESS_SECURITY_DEFAULT (checked in
// policy.c), which also has SYS_SEC_CONSOLE for programs the kernel
// console starts.
#define POLICY_USER_PERMISSIONS     (SYS_SEC_FILE_RW | SYS_SEC_GFX | SYS_SEC_IPC | \
                                     SYS_SEC_EXEC | SYS_SEC_ELEVATE)

typedef struct {
    uint32_t    id;             // POLICY_*, never 0
    const char* name;           // Its key under hkey_root/system/policy
    uint32_t    permissions;    // SYS_SEC_* the group holds
    const char* description;
} Policy;

// The policy with this id, NULL for none (0) or an id the kernel doesn't know
const Policy* policy_find(uint32_t id);

// The policy named `name` ("admin"), NULL if none
const Policy* policy_find_name(const char* name);

// The index-th policy of the table (0 first), NULL past the last
const Policy* policy_at(uint32_t index);

// The clearance a file in the group asks for: its permissions beyond the
// user policy's (see the top). 0 for POLICY_NONE or an unknown id.
uint32_t policy_clearance(uint32_t id);

// True for POLICY_NONE and every id in the table
bool policy_is_valid(uint32_t id);

// Registry path below hkey_root
#define POLICY_REGISTRY_KEY     "system/policy"

// Write the table into `hive` under system/policy: one read-only key per
// policy, named after it, holding strings
//
//     id            "2"
//     permissions   "0x000003ef"    SYS_SEC_* the group holds
//     clearance     "0x00000100"    what a file in it asks for
//     description   "..."
//
// The key and everything below it are REGISTRY_PERMISSION_READ only, so the
// registry calls refuse to change, add to, rename or remove it, admin or
// not. Whatever was there is replaced. "system" is made (read and write)
// if missing. True when the hive changed (so the caller saves it); false
// when it already matched, or on failure (*failed set when given).
// The caller holds the registry lock, or is the boot thread.
bool policy_registry_publish(struct RegistryHive* hive, bool* failed);

#endif
