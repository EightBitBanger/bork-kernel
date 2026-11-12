// boot/rpi4/boot.c  --  replaces boot/x86/boot.c (only the parts that differ)
//
// Big differences from the x86 kmain:
//   * No MultibootInfo. Memory size comes from the device tree, and the
//     framebuffer is *requested* from the GPU firmware over the mailbox.
//   * No gdt_init(). AArch64 has no segmentation.
//   * idt_init() is just `msr vbar_el1` in start.S plus gic_init().
//   * The MMU must be on before caches work, and caches must be on before
//     anything runs at a reasonable speed (on x86 GRUB left you with caching
//     enabled; here, with the MMU off, every access is uncached Device memory).
//   * cli / sti  ->  msr daifset, #2 / msr daifclr, #2
//
// Everything from registry_hive_initiate() down, and the two worker threads,
// stays exactly as it is.

#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/boot/rpi4/mailbox.h>
#include <kernel/boot/rpi4/fdt.h>
#include <kernel/memory/malloc.h>
#include <kernel/scheduler/scheduler.h>
#include <kernel/mutex.h>

extern char _kernel_program_end[];

void gic_init(void);
void arch_timer_init(void);
void mmu_init(uint64_t ram_size);                       // identity map + caches on
void vmm_map_hardware_region(uint64_t phys, uint64_t virt, uint64_t size, int attr);

static inline uint64_t align_page_up(uint64_t v) { return (v + 0xFFFUL) & ~0xFFFUL; }

void kmain(uint64_t dtb) {
    // Replaces the MULTIBOOT_BOOTLOADER_MAGIC check
    if (!fdt_valid(dtb))
        return;

    uint64_t ram_base, ram_size;
    fdt_get_memory(dtb, &ram_base, &ram_size);          // replaces mmap_addr walk

    // --- Framebuffer: ask the VideoCore for one (replaces the Multiboot header
    // video request + mbi->framebuffer_* fields)
    struct FramebufferInfo fb = {
        .width  = 1600,
        .height = 900,
        .bpp    = 32,
    };
    if (!mailbox_framebuffer_alloc(&fb))                // fills addr, pitch, size
        return;

    // The firmware hands back a bus address; strip the VideoCore alias bits
    uint64_t framebuffer_phys = fb.addr & 0x3FFFFFFF;
    uint64_t vram_bytes       = (uint64_t)fb.pitch * fb.height;
    uint64_t back_buffer_bytes = (uint64_t)fb.width * fb.height * 4;

    // Same layout math as x86 -- this part ports unchanged
    uint64_t heap_start   = ((uint64_t)_kernel_program_end + 0xF) & ~0xFUL;
    uint64_t heap_size    = 4UL * 1024 * 1024;
    uint64_t front_buffer = align_page_up(heap_start + heap_size) + (framebuffer_phys & 0xFFF);
    uint64_t back_buffer  = align_page_up(front_buffer + vram_bytes);
    uint64_t kernel_memory_end = align_page_up(back_buffer + back_buffer_bytes);

    // MMU first: with it off, unaligned accesses fault and nothing is cached
    mmu_init(ram_size);

    gic_init();                                         // idt_init + pic_remap
    arch_timer_init();                                  // timer_init
    __asm__ volatile("msr daifclr, #2");                // sti

    pmm_init_range(ram_base, ram_size, kernel_memory_end);
    vmm_init(kernel_memory_end);

    rand_init();                                        // RDRAND -> BCM2711 RNG200
    heap_set_base_address(heap_start);
    heap_init(16, heap_size);

    scheduler_init();
    kernel_event_init();

    draw_set_info_raw(fb.width, fb.height, fb.pitch);   // was draw_set_info(mbi)
    display_init();
    draw_set_clip_rect(0, 0, fb.width, fb.height);
    draw_set_frame_front_buffer(front_buffer);
    draw_set_frame_back_buffer(back_buffer);
    draw_set_buffer_default();

    // Normal non-cacheable is the ARM analogue of write-combining
    vmm_map_hardware_region(framebuffer_phys, front_buffer, vram_bytes, VM_NORMAL_NC);

    // PS/2 is gone. Keyboard and mouse sit behind PCIe -> VL805 xHCI -> USB HID,
    // which is by far the biggest single piece of work in this port.
    pcie_init();
    usb_xhci_init();
    usb_hid_init();

    // ... console_init, kernel_init, registry, dwm_initiate, desktop icons,
    // thread_create(thread_dwm_main) and thread_create(thread_kernel_main):
    // identical to boot/x86/boot.c.
    //
    // ata.h has no counterpart; storage becomes the EMMC2 SD controller.
}
