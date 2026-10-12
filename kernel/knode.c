#include <kernel/knode.h>
#include <kernel/kernel.h>
#include <kernel/util/string.h>

uint32_t knode_root = KNODE_NULL;

static uint8_t knode_is_valid_address(uint32_t directory_address) {
    if (directory_address == KMALLOC_NULL || directory_address == 0) 
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

static uint8_t knode_contains_reference_address(uint32_t directory_address, uint32_t reference_ptr) {
    struct KernelDirectory directory;
    
    if (knode_is_valid_address(directory_address) == 0) 
        return 0;
    
    if (reference_ptr == 0 || reference_ptr == KMALLOC_NULL) 
        return 0;
    
    kmem_read(&directory, directory_address, sizeof(struct KernelDirectory));
    
    uint16_t reference_index;
    for (reference_index = 0; reference_index < directory.size; reference_index++) {
        if (directory.reference[reference_index] == reference_ptr) 
            return 1;
    }
    
    uint32_t current_extent_address = directory.next;
    
    while (current_extent_address != 0 && current_extent_address != KMALLOC_NULL) {
        struct KernelDirectoryExtent extent;
        kmem_read(&extent, current_extent_address, sizeof(struct KernelDirectoryExtent));
        
        uint16_t reference_index;
        for (reference_index = 0; reference_index < extent.size; reference_index++) {
            if (extent.reference[reference_index] == reference_ptr) {
                return 1;
            }
        }
        
        current_extent_address = extent.next;
    }
    
    return 0;
}

uint32_t create_knode(const char* name, uint32_t parent_address) {
    struct KernelDirectory kdir;
    memset(&kdir, 0x00, sizeof(struct KernelDirectory));
    
    strncpy(kdir.name, name, sizeof(kdir.name) - 1);
    kdir.name[sizeof(kdir.name) - 1] = '\0';
    
    kdir.parent = parent_address;
    kdir.next   = 0;
    kdir.prev   = 0;
    kdir.size   = 0;
    
    uint32_t knode_address = kmalloc(sizeof(struct KernelDirectory));
    if (knode_address == KMALLOC_NULL)
        return KMALLOC_NULL;
    
    kmalloc_set_flags(knode_address, KMALLOC_FLAG_DIRECTORY);
    kmalloc_set_permissions(knode_address, KMALLOC_PERMISSION_READ | KMALLOC_PERMISSION_WRITE);
    
    kmem_write(knode_address, &kdir, sizeof(struct KernelDirectory));
    
    knode_add_reference(parent_address, knode_address);
    return knode_address;
}

bool destroy_knode(uint32_t address, uint32_t parent_address) {
    if (!knode_is_valid_address(address)) 
        return false;
    knode_remove_reference(parent_address, address);
    kfree(address);
    return true;
}

void knode_get_name(uint32_t address, char* name) {
    for (uint8_t i=0; i < KNODE_NAME_LENGTH_MAX; i++) {
        kmem_read(&name[i], address + i, 1);
        if (name[i] == '\0') 
            return;
    }
    name[KNODE_NAME_LENGTH_MAX-1] = '\0';
}

void knode_set_name(uint32_t address, const char* name) {
    uint8_t permissions = kmalloc_get_permissions(address);
    if (!(permissions & KMALLOC_PERMISSION_WRITE)) 
        return;
    for (uint8_t i=0; i < KNODE_NAME_LENGTH_MAX; i++) {
        kmem_write(address + i, &name[i], 1);
        if (name[i] == '\0') 
            return;
    }
}

void knode_get_permissions(uint32_t address, uint8_t* permissions) {
    *permissions = kmalloc_get_permissions(address);
}

void knode_set_permissions(uint32_t address, uint8_t permissions) {
    kmalloc_set_permissions(address, permissions);
}

uint32_t create_extent(void) {
    struct KernelDirectoryExtent kextent;
    memset(&kextent, 0x00, sizeof(struct KernelDirectoryExtent));
    kextent.next = 0;
    kextent.prev = 0;
    kextent.size = 0;
    kextent.reference[0] = 0;
    
    uint32_t device_address = kmalloc(sizeof(struct KernelDirectoryExtent));
    
    kmalloc_set_flags(device_address, KMALLOC_FLAG_DIRECTORY | KMALLOC_FLAG_EXTENT);
    kmalloc_set_permissions(device_address, KMALLOC_PERMISSION_READ | KMALLOC_PERMISSION_WRITE);
    
    kmem_write(device_address, &kextent, sizeof(struct KernelDirectoryExtent));
    return device_address;
}

void destroy_extent(uint32_t address) {
    kfree(address);
}

uint8_t knode_add_reference(uint32_t directory_address, uint32_t reference_ptr) {
    if (knode_is_valid_address(directory_address) == 0)
        return 0;
    
    if (reference_ptr == 0 || reference_ptr == KMALLOC_NULL)
        return 0;
    
    if (knode_contains_reference_address(directory_address, reference_ptr) != 0)
        return 0;
    
    struct KernelDirectory directory;
    kmem_read(&directory, directory_address, sizeof(struct KernelDirectory));
    
    if (directory.size < KDIRECTORY_REF_MAX) {
        directory.reference[directory.size] = reference_ptr;
        directory.size++;
        kmem_write(directory_address, &directory, sizeof(struct KernelDirectory));
        return 1;
    }
    
    uint32_t current_extent_address  = directory.next;
    uint32_t previous_extent_address = 0;
    
    while (current_extent_address != 0 && current_extent_address != KMALLOC_NULL) {
        struct KernelDirectoryExtent extent;
        kmem_read(&extent, current_extent_address, sizeof(struct KernelDirectoryExtent));
    
        if (extent.size < KDIRECTORYEX_REF_MAX) {
            extent.reference[extent.size] = reference_ptr;
            extent.size++;
            kmem_write(current_extent_address, &extent, sizeof(struct KernelDirectoryExtent));
            return 1;
        }
    
        previous_extent_address = current_extent_address;
        current_extent_address  = extent.next;
    }
    
    uint32_t new_extent_address = create_extent();
    if (new_extent_address == KMALLOC_NULL)
        return 0;
    
    struct KernelDirectoryExtent new_extent;
    kmem_read(&new_extent, new_extent_address, sizeof(struct KernelDirectoryExtent));
    
    new_extent.prev         = previous_extent_address;
    new_extent.next         = 0;
    new_extent.size         = 1;
    new_extent.reference[0] = reference_ptr;
    
    kmem_write(new_extent_address, &new_extent, sizeof(struct KernelDirectoryExtent));
    
    if (previous_extent_address != 0) {
        struct KernelDirectoryExtent previous_extent;
        kmem_read(&previous_extent, previous_extent_address, sizeof(struct KernelDirectoryExtent));
        previous_extent.next = new_extent_address;
        kmem_write(previous_extent_address, &previous_extent, sizeof(struct KernelDirectoryExtent));
    } else {
        directory.next = new_extent_address;
    }
    
    directory.prev = new_extent_address;
    kmem_write(directory_address, &directory, sizeof(struct KernelDirectory));
    
    return 1;
}

uint8_t knode_remove_reference(uint32_t directory_address, uint32_t reference_ptr) {
    if (knode_is_valid_address(directory_address) == 0)
        return 0;
    
    struct KernelDirectory directory;
    kmem_read(&directory, directory_address, sizeof(struct KernelDirectory));
    
    uint16_t reference_index;
    for (reference_index = 0; reference_index < directory.size; reference_index++) {
        if (directory.reference[reference_index] == reference_ptr) {
            uint16_t shift_index;
            for (shift_index = reference_index; shift_index + 1 < directory.size; shift_index++) {
                directory.reference[shift_index] = directory.reference[shift_index + 1];
            }
            
            directory.size--;
            directory.reference[directory.size] = 0;
            
            kmem_write(directory_address, &directory, sizeof(struct KernelDirectory));
            return 1;
        }
    }
    
    uint32_t current_extent_address = directory.next;
    
    while (current_extent_address != 0 && current_extent_address != KMALLOC_NULL) {
        struct KernelDirectoryExtent extent;
        kmem_read(&extent, current_extent_address, sizeof(struct KernelDirectoryExtent));
        
        for (reference_index = 0; reference_index < extent.size; reference_index++) {
            if (extent.reference[reference_index] == reference_ptr) {
                uint16_t shift_index;
                for (shift_index = reference_index; shift_index + 1 < extent.size; shift_index++) {
                    extent.reference[shift_index] = extent.reference[shift_index + 1];
                }
                
                extent.size--;
                extent.reference[extent.size] = 0;
                
                if (extent.size == 0) {
                    uint32_t previous_extent_address = extent.prev;
                    uint32_t next_extent_address     = extent.next;
                    
                    if (previous_extent_address != 0 && previous_extent_address != KMALLOC_NULL) {
                        struct KernelDirectoryExtent previous_extent;
                        kmem_read(&previous_extent, previous_extent_address, sizeof(struct KernelDirectoryExtent));
                        previous_extent.next = next_extent_address;
                        kmem_write(previous_extent_address, &previous_extent, sizeof(struct KernelDirectoryExtent));
                    } else {
                        directory.next = next_extent_address;
                    }
                    
                    if (next_extent_address != 0 && next_extent_address != KMALLOC_NULL) {
                        struct KernelDirectoryExtent next_extent;
                        kmem_read(&next_extent, next_extent_address, sizeof(struct KernelDirectoryExtent));
                        next_extent.prev = previous_extent_address;
                        kmem_write(next_extent_address, &next_extent, sizeof(struct KernelDirectoryExtent));
                    } else {
                        directory.prev = previous_extent_address;
                    }
                    
                    if (directory.next == 0 || directory.next == KMALLOC_NULL) {
                        directory.prev = 0;
                    }
                    
                    kmem_write(directory_address, &directory, sizeof(struct KernelDirectory));
                    destroy_extent(current_extent_address);
                } else {
                    kmem_write(current_extent_address, &extent, sizeof(struct KernelDirectoryExtent));
                }
                
                return 1;
            }
        }
        
        current_extent_address = extent.next;
    }
    
    return 0;
}

uint32_t knode_get_reference(uint32_t directory_address, uint32_t index) {
    if (knode_is_valid_address(directory_address) == 0) 
        return KMALLOC_NULL;
    
    struct KernelDirectory directory;
    kmem_read(&directory, directory_address, sizeof(struct KernelDirectory));
    
    if (index < directory.size) 
        return directory.reference[index];
    
    index -= directory.size;
    uint32_t current_extent_address = directory.next;
    
    while (current_extent_address != 0 && current_extent_address != KMALLOC_NULL) {
        struct KernelDirectoryExtent extent;
        kmem_read(&extent, current_extent_address, sizeof(struct KernelDirectoryExtent));
        
        if (index < extent.size) 
            return extent.reference[index];
        
        index -= extent.size;
        current_extent_address = extent.next;
    }
    return KMALLOC_NULL;
}

uint32_t knode_get_reference_count(uint32_t directory_address) {
    if (knode_is_valid_address(directory_address) == 0) 
        return KMALLOC_NULL;
    
    struct KernelDirectory directory;
    kmem_read(&directory, directory_address, sizeof(struct KernelDirectory));
    
    // Start with the number of references in the primary directory node
    uint32_t total_count = directory.size;
    
    // Traverse through the extents linked list to accumulate additional references
    uint32_t current_extent_address = directory.next;
    
    while (current_extent_address != 0 && current_extent_address != KMALLOC_NULL) {
        struct KernelDirectoryExtent extent;
        kmem_read(&extent, current_extent_address, sizeof(struct KernelDirectoryExtent));
        
        total_count += extent.size;
        current_extent_address = extent.next;
    }
    
    return total_count;
}

uint32_t knode_get_root(void) {
    return knode_root;
}

void knode_set_root(uint32_t address) {
    knode_root = address;
}

uint32_t knode_get_parent(uint32_t directory_address) {
    struct KernelDirectory directory;
    
    if (knode_is_valid_address(directory_address) == 0)
        return KMALLOC_NULL;
    
    kmem_read(&directory, directory_address, sizeof(struct KernelDirectory));
    
    if (directory.parent == 0 || directory.parent == KMALLOC_NULL)
        return directory_address;
    
    if (knode_is_valid_address(directory.parent) == 0)
        return directory_address;
    return directory.parent;
}

uint32_t knode_find_by_name(uint32_t directory_address, const char* name) {
    uint32_t reference_index = 0;
    
    if (knode_is_valid_address(directory_address) == 0) 
        return KMALLOC_NULL;
    
    if (name == NULL || name[0] == '\0') 
        return KMALLOC_NULL;
    
    while (1) {
        uint32_t reference_address =
            knode_get_reference(directory_address, reference_index);
        
        if (reference_address == KMALLOC_NULL || reference_address == 0) 
            break;
        
        char object_name[16];
        memset(object_name, 0x00, sizeof(object_name));
        
        kmem_read(object_name, reference_address, sizeof(object_name));
        
        if (strcmp(object_name, name) == 0) 
            return reference_address;
        
        reference_index++;
    }
    return KMALLOC_NULL;
}

bool knode_check_is_valid(uint32_t address) {
    if (knode_is_valid_address(address) == 0) 
        return false;
    return true;
}

