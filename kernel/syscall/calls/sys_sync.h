#ifndef _KERNEL_SYS_SYNC_H_
#define _KERNEL_SYS_SYNC_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/vfs/vfs.h>

//
// sync(): write everything the file systems still hold in memory (cached
// blocks, directory entries, allocation tables) out to their devices.
// Returns 0, or SYS_EIO if a device refused a write.
//

// ==========================================================================
// VFS adapter. The one VFS call this file makes: if your vfs.h names the
// "flush every mounted file system" call differently, change it here.
//
//   vfs_sync(void) -> bool     true once every mount's dirty state is on disk
// ==========================================================================

static inline bool sys_sync_all(void) {
    return vfs_sync_all();
}

// ==========================================================================

static int32_t sys_sync(const SyscallArgs* args) {
    (void)args;
    vfs_lock();
    bool ok = sys_sync_all();
    vfs_unlock();
    return ok ? SYS_OK : SYS_EIO;
}

#endif
