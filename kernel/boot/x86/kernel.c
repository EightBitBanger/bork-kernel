#include <kernel/util/string.h>
#include <stdint.h>
#include <stddef.h>

#include <kernel/kernel.h>

#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/heap.h>

// The root of the in-memory node tree. Storage devices are mounted under
// /mnt when the PCI scan finds them, hardware is described under /dev.
void kernel_init(void) {
    uint32_t root_node = create_knode("", 0);
    knode_set_root(root_node);
    
    uint32_t proc = create_knode("proc", root_node);
    uint32_t dev  = create_knode("dev", root_node);
    uint32_t mnt  = create_knode("mnt", root_node);
    
    knode_set_permissions(proc, KMALLOC_PERMISSION_READ);
    knode_set_permissions(dev,  KMALLOC_PERMISSION_READ);
    knode_set_permissions(mnt,  KMALLOC_PERMISSION_READ);
    
    hardware_identify_devices(dev, mnt, dev);
    
    // No storage yet: empty PATH and home until the PCI scan mounts a device
    struct LocalPaths fs_paths;
    memset(&fs_paths, 0, sizeof(fs_paths));
    kernel_set_local_paths(&fs_paths);
    
    kernel_set_working_directory("/");
}

void hardware_identify_devices(uint32_t knode_device, uint32_t knode_mount, uint32_t nt_device) {
    
}

uint32_t device_get_hardware_address(const char* name) {
    return 0;
}

void device_get_hardware_data(uint32_t address, char* data_buffer, uint8_t size) {
    
}

