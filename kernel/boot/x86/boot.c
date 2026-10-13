#include <stddef.h>
#include <stdint.h>

#include <kernel/kernel.h>

// Platform
#include <kernel/arch/x86/io.h>
#include <kernel/boot/x86/gdt.h>
#include <kernel/boot/x86/interrupt.h>
#include <kernel/boot/x86/multiboot_info.h>

// Memory
#include <kernel/malloc.h>
#include <kernel/arch/x86/virtual/vmm.h>

// Buses
#include <kernel/arch/x86/bus/pci/pci.h>

// Drivers
#include <kernel/arch/x86/drivers/ata/ata.h>
#include <kernel/arch/x86/drivers/ps2.h>
#include <kernel/arch/x86/drivers/rng.h>

// Utility
#include <kernel/util/math.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>
#include <kernel/util/random.h>

// Console
#include <kernel/console/print.h>
#include <kernel/console/console.h>
#include <kernel/console/virtual_key.h>

#include <kernel/vfs/vfs.h>
#include <kernel/resources/resource_manager.h>
#include <kernel/registry/registry.h>
#include <kernel/dwm/dwm.h>
#include <kernel/panic/panic_error.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/watchdog.h>
#include <kernel/syscall/syscall.h>
#include <kernel/mutex.h>
#include <kernel/kernel_lock.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/boot/x86/boot_image.h>

#define BOOT_DELAY_MS          800
#define KERNEL_EVENT_POLL_MS   10

extern char _kernel_program_end[];

extern ThreadBlock* current_thread;

extern void vfs_lock_debug_register(void) __attribute__((weak));

Event kernel_wakeup_event = EVENT_INITIALIZER;

static void thread_kernel_main(void) {
    while (1) {
        event_wait(&kernel_wakeup_event, KERNEL_EVENT_POLL_MS);
        
        kernel_lock_acquire();
        kernel_event_update();
        kernel_lock_release();
        
        wd_pet();
    }
}

static inline uint32_t align_page_up(uint32_t value) {
    return (value + 0xFFFU) & ~0xFFFU;
}

// True if [start, start + size) lies inside one usable region of the BIOS
// memory map. Physical addresses; works with paging off.
static bool boot_memory_usable(struct MultibootInfo* mbi, uint32_t start, uint32_t size) {
    if (!(mbi->flags & MULTIBOOT_INFO_MEM_MAP))
        return false;
    uint64_t end = (uint64_t)start + size;
    uint32_t addr = mbi->mmap_addr;
    uint32_t last = mbi->mmap_addr + mbi->mmap_length;
    while (addr < last) {
        struct MultibootMmapEntry* e = (struct MultibootMmapEntry*)addr;
        if (e->type == MULTIBOOT_MEMORY_AVAILABLE && e->addr <= start && e->addr + e->len >= end)
            return true;
        addr += e->size + sizeof(e->size);
    }
    return false;
}

void kmain(uint32_t magic, struct MultibootInfo* mbi) {
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        return;
    
    // The framebuffer_* fields are only valid when bit 12 is set. Bit 11 only
    // describes the VBE fields, which this kernel never reads.
    if (!(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO))
        return;
    
    // The drawing code assumes a direct-color 32 bpp linear framebuffer
    if (mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB || mbi->framebuffer_bpp != 32)
        return;
    
    // Without PAE a 32-bit kernel cannot map a framebuffer above 4 GB
    if ((mbi->framebuffer_addr >> 32) != 0)
        return;
    
    if (mbi->framebuffer_pitch < mbi->framebuffer_width * sizeof(uint32_t))
        return;
    
    uint32_t framebuffer_phys        = (uint32_t)mbi->framebuffer_addr;
    uint32_t framebuffer_page_offset = framebuffer_phys & 0xFFFU;
    
    // The hardware framebuffer spans pitch * height bytes. The pitch may be
    // padded beyond width * 4, so the front buffer reservation must be sized
    // from the pitch, or the VRAM mapping spills over the back buffer.
    uint32_t vram_bytes              = mbi->framebuffer_pitch * mbi->framebuffer_height;
    
    // The back buffer is tightly packed (stride = width, see draw.c)
    uint32_t back_buffer_bytes       = mbi->framebuffer_width * mbi->framebuffer_height * sizeof(uint32_t);
    
    // 4k page aligned virtual window for VRAM, right after the kernel image.
    uint32_t front_buffer_region     = align_page_up((uint32_t)_kernel_program_end);
    uint32_t front_buffer            = front_buffer_region + framebuffer_page_offset;
    uint32_t back_buffer             = align_page_up(front_buffer + vram_bytes);
    
    uint32_t _kernel_memory_end      = align_page_up(back_buffer + back_buffer_bytes);
    
    // Keep the boot area module from stage 2 (MBR, stage 2, kernel file) for
    // "boot install". Where the loader left it, the back buffer or the PMM
    // may reuse it, so move it right after everything reserved above and
    // reserve it too. Paging is still off: all of this is physical memory.
    if ((mbi->flags & MULTIBOOT_INFO_MODS) && mbi->mods_count > 0) {
        struct MultibootModule* module = (struct MultibootModule*)mbi->mods_addr;
        uint32_t size = module->mod_end - module->mod_start;
        uint32_t copy = _kernel_memory_end;
        
        if (module->mod_end > module->mod_start && boot_memory_usable(mbi, copy, size)) {
            memmove((void*)copy, (const void*)module->mod_start, size);   // Ranges may overlap
            boot_image_set(copy, size);
            _kernel_memory_end = align_page_up(copy + size);
        }
    }
    
    // Watchdog serial mirror (COM1). Harmless when no UART is present.
    wd_init();
    
    // COM1 for serial_puts() (process loader and fault reports, sys_print)
    // and the locks the syscalls use. The int 0x81 gate is set in idt_init().
    syscall_init();
    
    gdt_init();
    idt_init();
    
    // Set millisecond timer
    timer_init();
    __asm__ __volatile__("sti");
    
    // Paging
    pmm_init(mbi, _kernel_memory_end);
    vmm_init(mbi, _kernel_memory_end);
    
    // Random number generation
    rand_init();
    
    // Fire up the scheduler
    scheduler_init();
    kernel_lock_init();
    
    // Watchdog - thread naming
    wd_name_thread(current_thread, "boot");
    wd_register_lock("kernel", kernel_lock_mutex());
    
    if (vfs_lock_debug_register)
        vfs_lock_debug_register();
    
    // Event messenger
    kernel_event_init();
    
    // Initiate display and drawing
    draw_set_info((uint32_t)mbi);
    display_init();
    
    display_cursor_set_line(0);
    display_cursor_set_position(0);
    
    // Set drawing frame buffers
    draw_set_clip_rect(0, 0, mbi->framebuffer_width, mbi->framebuffer_height);
    draw_set_frame_front_buffer(front_buffer);
    draw_set_frame_back_buffer(back_buffer);
    draw_set_buffer_default();
    
    // Map the hardware framebuffer over the reserved window as write-combining
    // (falls back to uncached when the CPU has no PAT). The reservation above
    // covers exactly vram_bytes plus the page offset, so this cannot overlap
    // the back buffer.
    vmm_map_hardware_region(framebuffer_phys, front_buffer, vram_bytes, VM_WRITE_COMBINING);
    
    // Hard-lockup NMI watchdog: needs the scheduler (progress counter) and the
    // display (panic screen), and must come before any process is created
    // (it maps the IOAPIC into the kernel page directory)
    wd_nmi_init();
    
    // Initiate keyboard and mouse
    static char keyboard_string[255];
    static char prompt_string[255];
    static char virtual_key_map[255];
    
    kb_init();
    kb_map_init(virtual_key_map, sizeof(virtual_key_map));
    
    mouse_initiate();
    mouse_set_cursor_speed(14, 14);
    mouse_set_cursor_acceleration(2);
    
    // Prepare the console and fire up the kernel
    console_init(keyboard_string, prompt_string, sizeof(keyboard_string), sizeof(prompt_string));
    
    kernel_init();
    
    print("kernel v0.1.0\n");
    draw_flush_display();
    
    
    //
    // Command console / boot options
    
    {
        bool activate_console = false;
        
        uint64_t old_ms = timer_get_ms();
        while ((timer_get_ms() - old_ms) <= BOOT_DELAY_MS) {
            
            // Check keyboard data ready
            if (kb_get_current_char() == 'c') {
                activate_console = true;
                
                // Scan PCI bus for available hardware
                // before entering the console
                pci_init();
                
                kb_flush();
                
                console_prompt_print();
                draw_flush_display();
                break;
            }
        }
        
        // Run the dedicated console mode if activated
        while (activate_console) {
            ps2_route_console();
        }
    }
    
    // Scan PCI bus for available hardware
    pci_init();
    
    // Get primary knode directories
    
    const char path_mnt[] = "/mnt";
    const char path_sys[] = "/sys";
    const char root_dev[] = "dev";
    const char root_mnt[] = "mnt";
    
    uint32_t root_node = knode_get_root();
    uint32_t dev_directory = knode_find_by_name(root_node, root_dev);
    uint32_t mnt_directory = knode_find_by_name(root_node, root_mnt);
    
    // Courtesy delay for boot output
    uint64_t old_ms = timer_get_ms();
    while ((timer_get_ms() - old_ms) <= BOOT_DELAY_MS);
    
    // Blank the screen in preparation for pure graphics mode
    draw_rect_filled(0, 0, display_get_width(), display_get_height(), 0xFF000000);
    draw_flush_region(0, 0, display_get_width(), display_get_height());
    
    //
    // Load the registry
    
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);
    
    {
        char sys_path[128];
        strncpy(sys_path, paths.home, 128);
        strncat(sys_path, path_sys, 128);
        
        registry_hive_initiate(sys_path);
    }
    
    //
    // Initiate the DWM graphical environment and associated resources
    
    dwm_initiate();
    
    //
    // Load desktop icons
    
    uint16_t sep = 90;
    uint16_t posx = 30;
    uint16_t posy = 30;
    
    // Mounted device icons
    uint32_t number_of_mounts = knode_get_reference_count(mnt_directory);
    for (unsigned int i=0; i < number_of_mounts; i++) {
        uint32_t address = knode_get_reference(mnt_directory, i);
        
        char name[16];
        knode_get_name(address, name);
        
        char path[128];
        strncpy(path, path_mnt, 128);
        strncat(path, "/", 128);
        strncat(path, name, 128);
        
        dwm_create_mount(posx, posy, name, path);
        posx += sep;
    }
    
    // File / folder icons
    
    uint32_t item_count = vfs_directory_get_item_count(path_mnt);
    for (unsigned int i=0; i < item_count; i++) {
        
        // Check user directory
        char user_path[128];
        memset(user_path, '\0', sizeof(user_path));
        strncpy(user_path, path_mnt, 128);
        
        char device_name[128];
        if (!vfs_directory_get_item(path_mnt, i, device_name, sizeof(device_name))) 
            continue;
        
        strncat(user_path, "/", 128);
        strncat(user_path, device_name, 128);
        strncat(user_path, "/usr/desktop", 128);
        
        // Find the desktop directory under user
        if (!vfs_directory_check(user_path)) 
            continue;
        
        // Load desktop icons
        uint32_t desktop_item_count = vfs_directory_get_item_count(user_path);
        for (unsigned int d=0; d < desktop_item_count; d++) {
            char desktop_item_path[128];
            char desktop_item_name[128];
            if (!vfs_directory_get_item(user_path, d, desktop_item_name, sizeof(desktop_item_name))) 
                continue;
            
            memcpy(desktop_item_path, user_path, 128);
            strncat(desktop_item_path, "/", 128);
            strncat(desktop_item_path, desktop_item_name, 128);
            
            // Folder
            if (vfs_directory_check(desktop_item_path)) {
                dwm_create_folder(posx, posy, desktop_item_name, desktop_item_path);
                posx += sep;
            } 
            
            // File
            else {
                dwm_create_file(posx, posy, desktop_item_name, desktop_item_path);
                posx += sep;
            }
        }
    }
    
    // Restore saved icon positions from <home>/usr/icons (if present)
    dwm_desktop_layout_load();
    
    //
    // TODOs
    
    // TODO scalable vector font or MSDF
    
    // TODO key combination binding
    
    // Fire up the DWM thread
    dwm_start(PRIORITY_HIGH);
    
    uint32_t irq_flags = irq_save();
    
    ThreadBlock* kernel_thread = thread_create(thread_kernel_main, PRIORITY_HIGH);
    wd_watch(kernel_thread, "kernel", 3000, 15000);
    
    irq_restore(irq_flags);
    
    while(1) {
        
        //dwm_launch_program("/mnt/ata0p1/usr/desktop/test");
        
        thread_sleep(100);
    }
}
