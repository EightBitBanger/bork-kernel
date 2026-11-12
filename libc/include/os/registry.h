#ifndef _OS_REGISTRY_H
#define _OS_REGISTRY_H

/*
 * The system registry: reading (SYS_REG_GET / SYS_REG_KEYS / SYS_REG_VALUES)
 * and changing it (SYS_REG_SET / SYS_REG_DELETE / SYS_REG_MKKEY /
 * SYS_REG_RENAME).
 *
 * A key is named by a path that starts with its hive's root key, then the
 * keys below it, '/'-separated:
 *
 *     "hkey_root/software/shell/menus/desktop"
 *
 * Key and value names are at most OS_REG_NAME_MAX - 1 bytes and match
 * exactly. Reading needs file read clearance (EPERM without it). Keys and
 * values the registry marks unreadable are EACCES.
 *
 * Per-user settings live under "hkey_user" (saved in <home>/sys/hku), the
 * system's under "hkey_root" (<home>/sys/hkr). Any change needs file
 * write clearance; in hkey_root, admin as well. Each change is saved to the
 * hive's file before the call returns.
 *
 * A value is bytes with no type of its own. By convention a string is
 * stored with its '\0' (os_reg_set_string).
 */

#include <stddef.h>
#include <sys/types.h>

#define OS_REG_NAME_MAX     16      /* Key or value name, terminator included */
#define OS_REG_VALUE_MAX    4096    /* os_reg_set: bytes in one value, at most */

/* Value `name` of the key at `path`, all of it, into buf. Returns its
   length in bytes (strings count their '\0'), or -1 with errno: ENOENT (no
   such key or value), ERANGE (size too small: nothing copied), EACCES. */
ssize_t os_reg_get(const char* path, const char* name, void* buf, size_t size);

/* The same for a string value, always terminated in out. A value too long
   for out is ERANGE (out ""); one stored without its '\0' is fine. */
int os_reg_get_string(const char* path, const char* name, char* out, size_t size);

/* The name of the key's index-th subkey (0 first, in no particular order)
   into out. Returns the name's length, 0 past the last subkey, or -1 with
   errno (ENOENT: no such key). out holds OS_REG_NAME_MAX bytes at most. */
int os_reg_key(const char* path, unsigned index, char* out, size_t size);

/* Create value `name` under the key at `path`, or replace its data, and
   save the hive to disk (missing keys on the way are made). 0, or -1 with
   errno: EPERM (no file write clearance, or hkey_root without admin),
   EACCES (key or value not writable), ENOENT (no such hive), EINVAL (empty
   name, or over OS_REG_VALUE_MAX bytes), ENAMETOOLONG, ENOMEM, EIO (set,
   but the hive file could not be written). */
int os_reg_set(const char* path, const char* name, const void* data, size_t size);

/* The same for a string, stored with its '\0' */
int os_reg_set_string(const char* path, const char* name, const char* text);

/* The name of the key's index-th value, like os_reg_key does subkeys.
   A value listed here may still be EACCES to os_reg_get. */
int os_reg_value(const char* path, unsigned index, char* out, size_t size);

/* Remove value `name` from the key at `path`; with name NULL or "", the key
   itself and everything below it (not a hive's root key: EINVAL). 0, or -1
   with errno: ENOENT, EACCES (it, or the key holding it, not writable),
   EPERM, EIO (removed, but the hive could not be saved). */
int os_reg_delete(const char* path, const char* name);

/* Make the key at `path` (and the keys on the way). 0, or -1 with errno:
   EEXIST (it is there already), EACCES, EPERM, ENOENT (no such hive),
   ENAMETOOLONG, ENOMEM, EIO. */
int os_reg_mkkey(const char* path);

/* Rename value `name` of the key at `path` to new_name; with name NULL or
   "", the key at `path` itself. 0, or -1 with errno: EEXIST (a sibling has
   that name), EINVAL (empty new name, or one with '/'), ENOENT, EACCES,
   EPERM, ENAMETOOLONG, EIO. */
int os_reg_rename(const char* path, const char* name, const char* new_name);

#endif
