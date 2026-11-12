#include <kernel/knode.h>
#include <kernel/kernel.h>
#include <kernel/util/string.h>

static struct KernelDirectory* knode_root = NULL;

static uint8_t knode_is_valid_address(const void* directory_address) {
    if (directory_address == NULL)
        return 0;

    uint8_t flags = kmalloc_get_flags(directory_address);

    // Check if its not a directory
    if ((flags & KMALLOC_FLAG_DIRECTORY) == 0)
        return 0;

    // Check if its a directory extent
    if ((flags & KMALLOC_FLAG_EXTENT) != 0)
        return 0;

    return 1;
}

static uint8_t knode_contains_reference_address(const void* directory_address, const void* reference_ptr) {
    if (knode_is_valid_address(directory_address) == 0)
        return 0;

    if (reference_ptr == NULL)
        return 0;

    const struct KernelDirectory* directory = directory_address;

    uint16_t reference_index;
    for (reference_index = 0; reference_index < directory->size; reference_index++) {
        if (directory->reference[reference_index] == reference_ptr)
            return 1;
    }

    const struct KernelDirectoryExtent* extent = directory->next;

    while (extent != NULL) {
        for (reference_index = 0; reference_index < extent->size; reference_index++) {
            if (extent->reference[reference_index] == reference_ptr) {
                return 1;
            }
        }

        extent = extent->next;
    }

    return 0;
}

void* create_knode(const char* name, void* parent_address) {
    struct KernelDirectory* kdir = kmalloc(sizeof(struct KernelDirectory));
    if (kdir == NULL)
        return NULL;

    // kmalloc hands out zeroed blocks: no extents, no references
    strncpy(kdir->name, name, sizeof(kdir->name) - 1);
    kdir->name[sizeof(kdir->name) - 1] = '\0';

    kdir->parent = parent_address;

    kmalloc_set_flags(kdir, KMALLOC_FLAG_DIRECTORY);
    kmalloc_set_permissions(kdir, KMALLOC_PERMISSION_READ | KMALLOC_PERMISSION_WRITE);

    knode_add_reference(parent_address, kdir);
    return kdir;
}

bool destroy_knode(void* address, void* parent_address) {
    if (!knode_is_valid_address(address))
        return false;
    knode_remove_reference(parent_address, address);
    kfree(address);
    return true;
}

void knode_get_name(const void* address, char* name) {
    const char* source = address;
    for (uint8_t i=0; i < KNODE_NAME_LENGTH_MAX; i++) {
        name[i] = source[i];
        if (name[i] == '\0')
            return;
    }
    name[KNODE_NAME_LENGTH_MAX-1] = '\0';
}

void knode_set_name(void* address, const char* name) {
    uint8_t permissions = kmalloc_get_permissions(address);
    if (!(permissions & KMALLOC_PERMISSION_WRITE))
        return;
    char* destination = address;
    for (uint8_t i=0; i < KNODE_NAME_LENGTH_MAX; i++) {
        destination[i] = name[i];
        if (name[i] == '\0')
            return;
    }
}

void knode_get_permissions(const void* address, uint8_t* permissions) {
    *permissions = kmalloc_get_permissions(address);
}

void knode_set_permissions(void* address, uint8_t permissions) {
    kmalloc_set_permissions(address, permissions);
}

void* create_extent(void) {
    // Zeroed by kmalloc: unlinked and empty
    struct KernelDirectoryExtent* kextent = kmalloc(sizeof(struct KernelDirectoryExtent));
    if (kextent == NULL)
        return NULL;

    kmalloc_set_flags(kextent, KMALLOC_FLAG_DIRECTORY | KMALLOC_FLAG_EXTENT);
    kmalloc_set_permissions(kextent, KMALLOC_PERMISSION_READ | KMALLOC_PERMISSION_WRITE);
    return kextent;
}

void destroy_extent(void* address) {
    kfree(address);
}

uint8_t knode_add_reference(void* directory_address, void* reference_ptr) {
    if (knode_is_valid_address(directory_address) == 0)
        return 0;

    if (reference_ptr == NULL)
        return 0;

    if (knode_contains_reference_address(directory_address, reference_ptr) != 0)
        return 0;

    struct KernelDirectory* directory = directory_address;

    if (directory->size < KDIRECTORY_REF_MAX) {
        directory->reference[directory->size] = reference_ptr;
        directory->size++;
        return 1;
    }

    struct KernelDirectoryExtent* current_extent  = directory->next;
    struct KernelDirectoryExtent* previous_extent = NULL;

    while (current_extent != NULL) {
        if (current_extent->size < KDIRECTORYEX_REF_MAX) {
            current_extent->reference[current_extent->size] = reference_ptr;
            current_extent->size++;
            return 1;
        }

        previous_extent = current_extent;
        current_extent  = current_extent->next;
    }

    struct KernelDirectoryExtent* new_extent = create_extent();
    if (new_extent == NULL)
        return 0;

    new_extent->prev         = previous_extent;
    new_extent->next         = NULL;
    new_extent->size         = 1;
    new_extent->reference[0] = reference_ptr;

    if (previous_extent != NULL) {
        previous_extent->next = new_extent;
    } else {
        directory->next = new_extent;
    }

    directory->prev = new_extent;
    return 1;
}

uint8_t knode_remove_reference(void* directory_address, const void* reference_ptr) {
    if (knode_is_valid_address(directory_address) == 0)
        return 0;

    struct KernelDirectory* directory = directory_address;

    uint16_t reference_index;
    for (reference_index = 0; reference_index < directory->size; reference_index++) {
        if (directory->reference[reference_index] == reference_ptr) {
            uint16_t shift_index;
            for (shift_index = reference_index; shift_index + 1 < directory->size; shift_index++) {
                directory->reference[shift_index] = directory->reference[shift_index + 1];
            }

            directory->size--;
            directory->reference[directory->size] = NULL;
            return 1;
        }
    }

    struct KernelDirectoryExtent* extent = directory->next;

    while (extent != NULL) {
        for (reference_index = 0; reference_index < extent->size; reference_index++) {
            if (extent->reference[reference_index] == reference_ptr) {
                uint16_t shift_index;
                for (shift_index = reference_index; shift_index + 1 < extent->size; shift_index++) {
                    extent->reference[shift_index] = extent->reference[shift_index + 1];
                }

                extent->size--;
                extent->reference[extent->size] = NULL;

                if (extent->size == 0) {
                    struct KernelDirectoryExtent* previous_extent = extent->prev;
                    struct KernelDirectoryExtent* next_extent     = extent->next;

                    if (previous_extent != NULL) {
                        previous_extent->next = next_extent;
                    } else {
                        directory->next = next_extent;
                    }

                    if (next_extent != NULL) {
                        next_extent->prev = previous_extent;
                    } else {
                        directory->prev = previous_extent;
                    }

                    if (directory->next == NULL) {
                        directory->prev = NULL;
                    }

                    destroy_extent(extent);
                }

                return 1;
            }
        }

        extent = extent->next;
    }

    return 0;
}

void* knode_get_reference(const void* directory_address, uint32_t index) {
    if (knode_is_valid_address(directory_address) == 0)
        return NULL;

    const struct KernelDirectory* directory = directory_address;

    if (index < directory->size)
        return directory->reference[index];

    index -= directory->size;
    const struct KernelDirectoryExtent* extent = directory->next;

    while (extent != NULL) {
        if (index < extent->size)
            return extent->reference[index];

        index -= extent->size;
        extent = extent->next;
    }
    return NULL;
}

uint32_t knode_get_reference_count(const void* directory_address) {
    // Not a directory: no list at all
    if (knode_is_valid_address(directory_address) == 0)
        return 0;

    const struct KernelDirectory* directory = directory_address;

    // Start with the number of references in the primary directory node
    uint32_t total_count = directory->size;

    // Traverse through the extents linked list to accumulate additional references
    const struct KernelDirectoryExtent* extent = directory->next;

    while (extent != NULL) {
        total_count += extent->size;
        extent = extent->next;
    }

    return total_count;
}

void* knode_get_root(void) {
    return knode_root;
}

void knode_set_root(void* address) {
    knode_root = address;
}

void* knode_get_parent(void* directory_address) {
    if (knode_is_valid_address(directory_address) == 0)
        return NULL;

    struct KernelDirectory* directory = directory_address;

    if (directory->parent == NULL)
        return directory;

    if (knode_is_valid_address(directory->parent) == 0)
        return directory;
    return directory->parent;
}

void* knode_find_by_name(const void* directory_address, const char* name) {
    uint32_t reference_index = 0;

    if (knode_is_valid_address(directory_address) == 0)
        return NULL;

    if (name == NULL || name[0] == '\0')
        return NULL;

    while (1) {
        void* reference_address =
            knode_get_reference(directory_address, reference_index);

        if (reference_address == NULL)
            break;

        char object_name[16];
        memcpy(object_name, reference_address, sizeof(object_name));
        object_name[sizeof(object_name) - 1] = '\0';

        if (strcmp(object_name, name) == 0)
            return reference_address;

        reference_index++;
    }
    return NULL;
}

bool knode_check_is_valid(const void* address) {
    if (knode_is_valid_address(address) == 0)
        return false;
    return true;
}
