
// Handles on kernel nodes that are about to be freed
//
// Kernel subsystems that publish data as kernel-node files (/proc/<pid>)
// free those blocks themselves. A handle still open on one would read
// freed memory, so they revoke it first.

#include <stdint.h>
#include <stdbool.h>
#include <kernel/malloc.h>

#include <kernel/vfs/vfs.h>
#include <kernel/vfs/vfs_internal.h>

#include <kernel/util/list.h>

void vfs_knode_revoke_handles(const void* address) {
    if (address == NULL)
        return;

    vfs_lock();
    struct list_node* n = open_files_head;
    while (n != NULL) {
        struct list_node* following = n->next;
        OpenFileDescriptor* desc = (OpenFileDescriptor*)n->data;
        // Every vfs_* call holds the lock, so nobody is using `desc` now
        if (desc != NULL && !desc->in_file_system && desc->knode == address) {
            list_remove(&open_files_head, &open_files_tail, desc);
            free(desc);
        }
        n = following;
    }
    vfs_unlock();
}
