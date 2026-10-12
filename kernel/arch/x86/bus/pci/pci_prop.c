#include <kernel/arch/x86/io.h>
#include <kernel/arch/x86/heap.h>
#include <kernel/arch/x86/bus/pci/pci.h>
#include <kernel/arch/x86/bus/pci/pci_internal.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/arch/x86/drivers/ahci/ahci.h>
#include <kernel/arch/x86/drivers/ata/ata.h>

#include <kernel/kernel.h>
#include <kernel/knode.h>
#include <kernel/fs/fs.h>

#include <kernel/console/print.h>
#include <kernel/util/string.h>

uint32_t pci_create_property_node(uint32_t parent_knode, const char* name, const char* value) {
    uint32_t prop_dir = create_knode(name, parent_knode);
    uint32_t device_val = create_device(value);
    knode_add_reference(prop_dir, device_val);
    knode_set_permissions(prop_dir, KMALLOC_PERMISSION_READ);
    knode_set_permissions(device_val, KMALLOC_PERMISSION_READ);
    return prop_dir;
}

void pci_export_vfs_properties(uint32_t type_knode, const struct PCIDeviceInfo* dev_info) {
    char buf[16];
    
    // BAR (physical address of the first memory BAR; BARs are mapped on demand)
    if (dev_info->bar_physical_address != 0) {
        memset(buf, 0, sizeof(buf));
        if ((dev_info->bar_physical_address >> 32) != 0) {
            format_hex32_string(buf, (uint32_t)(dev_info->bar_physical_address >> 32));
            pci_create_property_node(type_knode, "bar_high", buf);
            memset(buf, 0, sizeof(buf));
        }
        format_hex32_string(buf, (uint32_t)dev_info->bar_physical_address);
        pci_create_property_node(type_knode, "bar", buf);
    }
    
    // Vendor ID
    memset(buf, 0, sizeof(buf));
    format_hex16_string(buf, dev_info->vendor_id);
    pci_create_property_node(type_knode, "vendor", buf);
    
    // Device ID
    memset(buf, 0, sizeof(buf));
    format_hex16_string(buf, dev_info->device_id);
    pci_create_property_node(type_knode, "device", buf);
    
    // Class
    memset(buf, 0, sizeof(buf));
    format_dec8_string(buf, dev_info->class_code);
    pci_create_property_node(type_knode, "class", buf);
    
    // Subclass
    memset(buf, 0, sizeof(buf));
    format_dec8_string(buf, dev_info->subclass);
    pci_create_property_node(type_knode, "subclass", buf);
    
    // Sub-vendor & Sub-device ID
    if (dev_info->sub_vendor_id != 0x0000 && dev_info->sub_vendor_id != 0xFFFF) {
        format_hex16_string(buf, dev_info->sub_vendor_id);
        pci_create_property_node(type_knode, "sub_vendor", buf);
    
        format_hex16_string(buf, dev_info->sub_device_id);
        pci_create_property_node(type_knode, "sub_id", buf);
    }
    
    // IRQ Pin & Line
    if (dev_info->irq_pin > 0 && dev_info->irq_pin <= 4) {
        format_dec8_string(buf, dev_info->irq_pin);
        pci_create_property_node(type_knode, "pin", buf);
        
        if (dev_info->irq_line != 0xFF) {
            format_dec8_string(buf, dev_info->irq_line);
            pci_create_property_node(type_knode, "irq", buf);
        }
    }
}
