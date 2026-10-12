#ifndef _MULTIBOOT_INFO_TYPE_H_
#define _MULTIBOOT_INFO_TYPE_H_

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002
#define MULTIBOOT_MEMORY_AVAILABLE  1

// MultibootInfo.flags bits (Multiboot 1 spec, section 3.3)
#define MULTIBOOT_INFO_MEMORY           (1U << 0)   // mem_lower / mem_upper
#define MULTIBOOT_INFO_MODS             (1U << 3)   // mods_count / mods_addr
#define MULTIBOOT_INFO_MEM_MAP          (1U << 6)   // mmap_addr / mmap_length
#define MULTIBOOT_INFO_VBE_INFO         (1U << 11)  // vbe_* fields
#define MULTIBOOT_INFO_FRAMEBUFFER_INFO (1U << 12)  // framebuffer_* fields

#define MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED  0
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB      1
#define MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT 2

struct MultibootInfo {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    
    // Elf section headers / drive info
    uint32_t num;
    uint32_t size;
    uint32_t addr;
    uint32_t shndx;
    
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    
    // Video Info Fields
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    
    // Linear Framebuffer Info (populated when bit 11 of 'flags' is set)
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type; // 0 = Indexed, 1 = RGB, 2 = Text
    
    // Color info details
    uint8_t  framebuffer_red_field_position;
    uint8_t  framebuffer_red_mask_size;
    uint8_t  framebuffer_green_field_position;
    uint8_t  framebuffer_green_mask_size;
    uint8_t  framebuffer_blue_field_position;
    uint8_t  framebuffer_blue_mask_size;
};

// One entry of the mods_addr array
struct MultibootModule {
    uint32_t mod_start;     // Physical address of the first byte
    uint32_t mod_end;       // One past the last byte
    uint32_t string;        // Physical address of a NUL-terminated name
    uint32_t reserved;
};

struct MultibootMmapEntry {
    uint32_t size;
    uint64_t addr;   // Base address of the memory region
    uint64_t len;    // Length of the region in bytes
    uint32_t type;   // Type of region (1 = Available RAM)
} __attribute__((packed));

#endif
