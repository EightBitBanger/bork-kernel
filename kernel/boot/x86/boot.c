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
#include <kernel/console/display.h>

// Graphics
#include <kernel/gfx/gfx.h>
#include <kernel/gfx/gfx_display.h>
#include <kernel/console/console.h>
#include <kernel/console/virtual_key.h>

#include <kernel/vfs/vfs.h>
#include <kernel/resources/resource_manager.h>
#include <kernel/registry/registry.h>
#include <kernel/panic/panic_error.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/scheduler/watchdog.h>
#include <kernel/syscall/syscall.h>
#include <kernel/mutex.h>
#include <kernel/kernel_lock.h>
#include <kernel/arch/x86/irq.h>
#include <kernel/console/mouse.h>
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

// Align a page up to the nearest page boundary
static inline uint32_t align_page_up(uint32_t value);

// True if [start, start + size) lies inside one usable region of the BIOS
// memory map. Physical addresses; works with paging off.
static bool boot_memory_usable(struct MultibootInfo* mbi, uint32_t start, uint32_t size);

// The first Multiboot module called `name` (see boot_module_named), or NULL
static struct MultibootModule* boot_find_module(struct MultibootInfo* mbi, const char* name);

// Move `module` to *end and advance *end past it (page aligned). Returns
// the new address, or 0 if it is empty or there is no usable RAM there.
static uint32_t boot_keep_module(struct MultibootInfo* mbi, struct MultibootModule* module, uint32_t* end);

static bool boot_ranges_overlap(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size);


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
    
    // Keep the modules the loader handed over. Where the loader left them,
    // the back buffer or the PMM may reuse them, so move them right after
    // everything reserved above and reserve them too. Paging is still off:
    // all of this is physical memory.
    //
    //   "bootarea"  from stage 2: MBR, stage 2, kernel file and asset archive
    //               as read off the boot disk, for "boot install"
    //   "assets"    the asset archive "boot install" / "boot repair" extract.
    //               From stage 2 it lies inside the boot area and moves with
    //               it; from GRUB it is a file of its own
    if ((mbi->flags & MULTIBOOT_INFO_MODS) && mbi->mods_count > 0) {
        struct MultibootModule* area   = boot_find_module(mbi, "bootarea");
        struct MultibootModule* assets = boot_find_module(mbi, "assets");
        
        uint32_t area_copy = 0;
        if (area != NULL) {
            area_copy = boot_keep_module(mbi, area, &_kernel_memory_end);
            if (area_copy != 0)
                boot_image_set(area_copy, area->mod_end - area->mod_start);
        }
        
        if (assets != NULL && assets->mod_end > assets->mod_start) {
            uint32_t size = assets->mod_end - assets->mod_start;
            bool inside = (area != NULL && assets->mod_start >= area->mod_start &&
                           assets->mod_end <= area->mod_end);
            
            if (inside) {
                if (area_copy != 0)
                    boot_assets_set(area_copy + (assets->mod_start - area->mod_start), size);
            } else if (area_copy == 0 || !boot_ranges_overlap(assets->mod_start, size, area_copy,
                                                              area->mod_end - area->mod_start)) {
                // (A separate archive the boot area copy landed on is lost)
                uint32_t copy = boot_keep_module(mbi, assets, &_kernel_memory_end);
                if (copy != 0)
                    boot_assets_set(copy, size);
            }
        }
    }
    
    // Watchdog serial mirror (COM1). Harmless when no UART is present.
    wd_init();
    
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
    
    // Graphics: front surface = the VRAM window (hardware pitch), back
    // surface = the packed buffer right after it. Nothing is presented until
    // the console's first print, which is after the VRAM mapping below.
    gfx_display_init(front_buffer, mbi->framebuffer_pitch, back_buffer,
                     (int32_t)mbi->framebuffer_width, (int32_t)mbi->framebuffer_height);
    
    // Text console (its own context on the back surface)
    display_init();
    
    display_cursor_set_line(0);
    display_cursor_set_position(0);
    
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
    
    // Interrupts are already on, and the keyboard IRQ reads port 0x60 too,
    // so it could swallow the mouse's ACK bytes and leave mouse_read()
    // waiting. Configure the mouse with them held off.
    {
        uint32_t flags = irq_save();
        mouse_initiate();
        irq_restore(flags);
    }
    
    // Prepare the console and fire up the kernel
    console_init(keyboard_string, prompt_string, sizeof(keyboard_string), sizeof(prompt_string));
    
    kernel_init();
    
    print("kernel v0.1.0\n");
    gfx_display_present_all();
    
    //
    // Command console / boot options
    const uint8_t escape_key_console = VK_F7;
    
    uint64_t old_ms = timer_get_ms();
    bool activate_console = false;
    while ((timer_get_ms() - old_ms) <= BOOT_DELAY_MS) {
        
        if (kb_vkey_check(escape_key_console)) {
            activate_console = true;
            
            pci_init();
            
            // Set the console path to the home disk
            struct LocalPaths paths;
            kernel_get_local_paths(&paths);
            kernel_set_working_directory(paths.home);
            
            print("\n");
            console_prompt_print();
            
            gfx_display_present_all();
            break;
        }
    }
    
    // Run the dedicated console mode if activated
    if (activate_console) {
        kb_flush();
        
        while (activate_console) {
            ps2_route_console();
        }
    }
    
    // Scan PCI bus for available hardware
    pci_init();
    
    // Set the console path to the home disk
    struct LocalPaths paths;
    kernel_get_local_paths(&paths);
    kernel_set_working_directory(paths.home);
    
    // Booted from a USB stick or an install disk: stay on the console (to
    // install or repair a system), the desktop only starts from the system
    // disk. Without a boot area (another loader) there is nothing to tell
    // them apart, and the desktop starts as before.
    bool start_desktop = (pci_boot_media() != PCI_BOOT_MEDIA);
    
    //
    // Load the registry
    
    const char path_sys[] = "/sys";
    {
        char sys_path[128];
        strncpy(sys_path, paths.home, sizeof(sys_path) - 1);
        sys_path[sizeof(sys_path) - 1] = '\0'; // Ensure null-termination
        
        // Remaining space, so the append cannot overflow
        size_t remaining = sizeof(sys_path) - strlen(sys_path) - 1;
        strncat(sys_path, path_sys, remaining);
        
        // Load registry hives
        registry_hive_initiate(sys_path);
        
        // Launch the dwm with executive level permissions (admin + file).
        // SYS_SEC_ELEVATE is only there to be handed down: programs started
        // from the desktop may ask the user for admin, and the dwm is the
        // one that shows them the prompt (SYS_ELEVATE_AGENT).
        if (start_desktop) {
            strncat(sys_path, "/dwm", sizeof(sys_path) - strlen(sys_path) - 1);
            char* args[] = { sys_path, NULL };
            
            uint32_t proc_sec = SYS_SEC_FILE_RW | SYS_SEC_ADMIN | 
                                SYS_SEC_GFX | SYS_SEC_GFX_SCREEN | 
                                SYS_SEC_IPC | SYS_SEC_INPUT | SYS_SEC_EXEC |
                                SYS_SEC_ELEVATE;
            
            console_cmd_execute(1, args, proc_sec, CONSOLE_THREAD_BLOCK | CONSOLE_THREAD_NO_BREAK);
        }
    }
    
    //
    // TODOs
    
    // TODO key combination binding
    
    uint32_t irq_flags = irq_save();
    
    ThreadBlock* kernel_thread = thread_create(thread_kernel_main, PRIORITY_HIGH);
    wd_watch(kernel_thread, "kernel", 3000, 15000);
    
    irq_restore(irq_flags);
    
    print("\n");
    console_prompt_print();
    
    gfx_display_present_all();
    kb_flush();
    
    while (1) {
        ps2_route_console();
    }
}

//
// Auxiliary
//

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

// A module's string is its command line: stage 2 passes "bootarea" and
// "assets", GRUB the path and any words after it ("/boot/assets.tar" or
// "/boot/assets.tar assets"). Any word equal to `name`, or a path whose
// file name is "<name>.tar", matches.
static bool boot_module_named(const char* string, const char* name) {
    size_t name_len = strlen(name);
    const char* p = string;
    
    while (*p != '\0') {
        while (*p == ' ')
            p++;
        const char* word = p;
        while (*p != '\0' && *p != ' ')
            p++;
        size_t len = (size_t)(p - word);
        if (len == 0)
            break;
        
        if (len == name_len && strncmp(word, name, name_len) == 0)
            return true;
        
        // File name of a path
        const char* file = word;
        for (const char* c = word; c < p; c++) {
            if (*c == '/')
                file = c + 1;
        }
        size_t file_len = (size_t)(p - file);
        if (file_len == name_len + 4 && strncmp(file, name, name_len) == 0 &&
            strncmp(file + name_len, ".tar", 4) == 0)
            return true;
    }
    return false;
}

static struct MultibootModule* boot_find_module(struct MultibootInfo* mbi, const char* name) {
    struct MultibootModule* modules = (struct MultibootModule*)mbi->mods_addr;
    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        if (modules[i].string != 0 && boot_module_named((const char*)modules[i].string, name))
            return &modules[i];
    }
    return NULL;
}

static uint32_t boot_keep_module(struct MultibootInfo* mbi, struct MultibootModule* module, uint32_t* end) {
    if (module->mod_end <= module->mod_start)
        return 0;
    uint32_t size = module->mod_end - module->mod_start;
    uint32_t copy = *end;
    if (!boot_memory_usable(mbi, copy, size))
        return 0;
    
    memmove((void*)copy, (const void*)module->mod_start, size);   // Ranges may overlap
    *end = align_page_up(copy + size);
    return copy;
}

static bool boot_ranges_overlap(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
    return (uint64_t)a < (uint64_t)b + b_size && (uint64_t)b < (uint64_t)a + a_size;
}

static inline uint32_t align_page_up(uint32_t value) {
    return (value + 0xFFFU) & ~0xFFFU;
}
