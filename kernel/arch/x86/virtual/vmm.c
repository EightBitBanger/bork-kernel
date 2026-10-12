#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/panic/panic_error.h>
#include <kernel/util/string.h>
#include <kernel/arch/x86/irq.h>
#include <stdbool.h>

#define VM_START          0x10000000U
#define VM_END            0x20000000U
#define VM_SIZE           (VM_END - VM_START)
#define VIRT_BITMAP_SIZE  (VM_SIZE / PAGE_SIZE / 8)

#define VM_PAGE_DIR_SIZE     1024
#define VM_PAGE_TABLE_SIZE   1024

// One kernel page that vmm_alloc_pages() never hands out. It is how the
// kernel reaches a physical frame with no kernel mapping (every user frame
// comes from above the identity map). Only used with interrupts off.
#define VM_SCRATCH        (VM_END - PAGE_SIZE)

// Flags a caller may set on a user page; P and U are always added
#define VM_USER_FLAG_MASK (VM_READWRITE | VM_PWT | VM_PCD | VM_PAT_PTE)

#define CR0_PG            0x80000000U
#define CR0_WP            0x00010000U

_Static_assert(sizeof(AddressSpace) <= PAGE_SIZE, "AddressSpace must fit in one page");
_Static_assert(VM_END <= VM_USER_START, "kernel VM window overlaps user space");

static uint8_t virt_bitmap[VIRT_BITMAP_SIZE];

static int find_contiguous_bits(uint8_t* bitmap, size_t bitmap_size, size_t num_bits);

// Statically reserve a full page directory and all page tables in BSS
uint32_t page_directory[VM_PAGE_DIR_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint32_t static_page_tables[VM_PAGE_DIR_SIZE][VM_PAGE_TABLE_SIZE] __attribute__((aligned(PAGE_SIZE)));

//
// CPU helpers
//

static inline uint32_t read_cr3(void) {
    uint32_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void write_cr3(uint32_t value) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void invlpg(uint32_t vaddr) {
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline bool pde_is_user(uint32_t pd_index) {
    return pd_index >= VM_USER_PDE_FIRST && pd_index < VM_USER_PDE_FIRST + VM_USER_PDE_COUNT;
}

//
// Page Attribute Table (write-combining support)
//

#define MSR_IA32_PAT        0x277U
#define PAT_TYPE_WC         0x01U
#define CPUID_EDX_PAT       (1U << 16)
#define CR0_CD              (1U << 30)
#define CR0_NW              (1U << 29)

// Fallback when PAT is missing: uncached, the previous behavior
static uint32_t vm_wc_flags = VM_PRESENT | VM_READWRITE | VM_PWT | VM_PCD;
static bool     vm_wc_available = false;

uint32_t vmm_get_write_combining_flags(void) {
    return vm_wc_flags;
}

bool vmm_has_write_combining(void) {
    return vm_wc_available;
}

// Reprogram PAT entry 1 (selected by PWT=1, PCD=0, PAT=0) from its power-on
// default WT to WC. Entries 0, 2 and 3 keep their defaults (WB, UC-, UC), so
// existing mappings (plain = WB, MMIO with PCD = UC-) are unaffected. Nothing
// maps with PWT alone before this runs.
//
// Follows the SDM sequence: caches off, flush, write MSR, flush TLB, caches on.
static void vmm_pat_init(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & CPUID_EDX_PAT))
        return;

    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_IA32_PAT));
    lo = (lo & ~0x0000FF00U) | (PAT_TYPE_WC << 8);

    uint32_t irq_flags = irq_save();

    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"((cr0 | CR0_CD) & ~CR0_NW) : "memory");
    __asm__ volatile("wbinvd" ::: "memory");

    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(MSR_IA32_PAT) : "memory");

    // Flush the whole (non-global) TLB by reloading CR3
    write_cr3(read_cr3());

    __asm__ volatile("wbinvd" ::: "memory");
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    irq_restore(irq_flags);

    vm_wc_flags     = VM_PRESENT | VM_READWRITE | VM_PWT;
    vm_wc_available = true;
}

void vmm_init(struct MultibootInfo* mbi, uint32_t identity_map_size) {
    (void)mbi;

    // The identity map has to end below the kernel VM window, or
    // vmm_alloc_pages() would remap pages the kernel image lives in
    if (identity_map_size > VM_START) {
        kernel_crashout(0x00, identity_map_size, 0x03, "Kernel memory overlaps the VM window");
        return;
    }

    memset(virt_bitmap, 0x00, VIRT_BITMAP_SIZE);

    // Completely clear the page directory. Everything begins as "not present"
    memset(page_directory, 0, sizeof(page_directory));

    // Clear out all static page tables to prevent garbage entries
    memset(static_page_tables, 0, sizeof(static_page_tables));

    // Identity map [0, identity_map_size)
    for (uint32_t addr = 0; addr < identity_map_size; addr += PAGE_SIZE) {
        static_page_tables[addr >> 22][(addr >> 12) & 0x3FFU] = addr | VM_PRESENT | VM_READWRITE;
    }

    // Link every kernel directory entry to its static page table now, once.
    // Previously entries were created on first use, so a kernel mapping made
    // after a process copied the directory (a driver mapping MMIO, say) was
    // missing from that process. Supervisor-only (no VM_USER): ring 3 can
    // never reach anything behind these entries.
    for (uint32_t pd_idx = 0; pd_idx < VM_PAGE_DIR_SIZE; pd_idx++) {
        if (!pde_is_user(pd_idx))
            page_directory[pd_idx] = ((uint32_t)&static_page_tables[pd_idx]) | VM_PRESENT | VM_READWRITE;
    }

    // Keep the scratch page out of the allocator
    size_t scratch_bit = (VM_SCRATCH - VM_START) / PAGE_SIZE;
    virt_bitmap[scratch_bit / 8] |= (uint8_t)(1U << (scratch_bit % 8));

    //
    // Initiate MMU
    //

    // Hand the root directory pointer over to the CPU's CR3 control register
    write_cr3((uint32_t)page_directory);

    // Paging on. WP makes ring 0 honour read-only pages too, so a kernel
    // write into a read-only user page faults (needed for copy-on-write, and
    // it catches kernel bugs). Every kernel mapping is read/write, so nothing
    // else changes.
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_PG | CR0_WP;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    // Enable write-combining memory type before anything maps the framebuffer
    vmm_pat_init();
}

void* vmm_alloc_pages(size_t num_pages) {
    uint32_t irq_flags = irq_save();

    int virt_start_page = find_contiguous_bits(virt_bitmap, VIRT_BITMAP_SIZE, num_pages);
    if (virt_start_page == -1) {
        irq_restore(irq_flags);
        kernel_crashout(0x00, 0x00000000, 0x03, "Out of physical memory");
        return NULL;
    }

    uint32_t start_vaddr = VM_START + (virt_start_page * PAGE_SIZE);

    for (size_t i = 0; i < num_pages; i++) {
        uint32_t current_vaddr = start_vaddr + (i * PAGE_SIZE);
        uint32_t phys_frame = pmm_alloc_frame();

        if (phys_frame == PMM_NO_FRAME) {
            vmm_free_pages((void*)start_vaddr, i);
            irq_restore(irq_flags);

            kernel_crashout(0x00, 0x00000000, 0x03, "Out of physical memory");
            return NULL;
        }

        vmm_map_page(phys_frame, current_vaddr, VM_PRESENT | VM_READWRITE);

        size_t bit = (size_t)virt_start_page + i;
        virt_bitmap[bit / 8] |= (1U << (bit % 8));
    }

    irq_restore(irq_flags);
    return (void*)start_vaddr;
}

void vmm_free_pages(void* virtual_addr, size_t num_pages) {
    // Round DOWN to the containing page (rounding up would skip a page)
    uint32_t vaddr = (uint32_t)virtual_addr & ~0xFFFU;
    if (vaddr < VM_START || vaddr >= VM_END) return;

    uint32_t irq_flags = irq_save();
    size_t virt_start_page = (vaddr - VM_START) / PAGE_SIZE;

    for (size_t i = 0; i < num_pages; i++) {
        uint32_t current_vaddr = vaddr + (i * PAGE_SIZE);

        // Get the physical frame so we can return it to the PMM
        uint32_t pd_index = current_vaddr >> 22;
        uint32_t pt_index = (current_vaddr >> 12) & 0x3FFU;
        uint32_t pte = static_page_tables[pd_index][pt_index];

        if (pte & VM_PRESENT) {
            uint32_t phys_frame = pte & ~0xFFFU;
            pmm_free_frame(phys_frame);
        }

        // Clear the mapping from the page tables
        vmm_unmap_page(current_vaddr);

        // Clear our virtual allocation bitmap track tracker
        size_t bit = virt_start_page + i;
        virt_bitmap[bit / 8] &= ~(1U << (bit % 8));
    }

    irq_restore(irq_flags);
}

void vmm_unmap_page(uint32_t virtual_addr) {
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x3FFU;

    // User pages live in per-space tables: see vmm_user_free()
    if (pde_is_user(pd_index))
        return;

    // Only clear if the directory table says a page table actually exists
    if (page_directory[pd_index] & VM_PRESENT) {
        // Break the mapping by zeroing the entry out (clears VM_PRESENT)
        static_page_tables[pd_index][pt_index] = 0;

        // Flush the TLB for this address
        invlpg(virtual_addr);
    }
}

void vmm_map_page(uint32_t physical_addr, uint32_t virtual_addr, uint32_t flags) {
    uint32_t pd_index = virtual_addr >> 22;
    uint32_t pt_index = (virtual_addr >> 12) & 0x3FFU;

    // A mapping written here would land in the shared kernel tables, i.e. in
    // every process at once. User pages go through vmm_user_* instead. If
    // this fires for an identity-mapped device, map it with
    // vmm_map_mmio_region() instead.
    if (pde_is_user(pd_index)) {
        kernel_crashout(0x00, virtual_addr, PT_SEG_FAULT, "vmm_map_page: user address");
        return;
    }

    // Kernel entries are all linked in vmm_init(); kept as a safety net
    if (!(page_directory[pd_index] & VM_PRESENT)) {
        page_directory[pd_index] = ((uint32_t)&static_page_tables[pd_index]) | VM_PRESENT | VM_READWRITE;
    }

    // Update the table entry directly via its pointer
    static_page_tables[pd_index][pt_index] = (physical_addr & ~0xFFFU) | flags;

    // Flush the TLB
    invlpg(virtual_addr);
}

void vmm_map_hardware_region(uint32_t phys_addr, uint32_t virt_addr, uint32_t size, uint32_t flags) {
    uint32_t page_offset = phys_addr & (PAGE_SIZE - 1);
    uint32_t start_phys  = phys_addr & ~(PAGE_SIZE - 1);
    uint32_t start_virt  = virt_addr & ~(PAGE_SIZE - 1);

    uint32_t total_size = size + page_offset;
    uint32_t num_pages = (total_size + (PAGE_SIZE - 1)) / PAGE_SIZE;

    // Map pages to kernel space
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t current_phys = start_phys + (i * PAGE_SIZE);
        uint32_t current_virt = start_virt + (i * PAGE_SIZE);

        vmm_map_page(current_phys, current_virt, flags);
    }
}

void* vmm_map_mmio_region(uint32_t phys_addr, uint32_t size_bytes, uint32_t flags) {
    uint32_t page_offset = phys_addr & (PAGE_SIZE - 1);
    uint32_t start_phys  = phys_addr & ~(PAGE_SIZE - 1);
    uint32_t total_size  = size_bytes + page_offset;
    uint32_t num_pages   = (total_size + (PAGE_SIZE - 1)) / PAGE_SIZE;

    uint32_t irq_flags = irq_save();

    // Allocate a chunk of virtual address space inside VM_START -> VM_END
    int virt_start_page = find_contiguous_bits(virt_bitmap, VIRT_BITMAP_SIZE, num_pages);
    if (virt_start_page == -1) {
        irq_restore(irq_flags);
        return NULL;
    }

    uint32_t start_vaddr = VM_START + (virt_start_page * PAGE_SIZE);

    // Map the virtual pages explicitly to the hardware BAR frames
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t current_phys = start_phys + (i * PAGE_SIZE);
        uint32_t current_virt = start_vaddr + (i * PAGE_SIZE);

        vmm_map_page(current_phys, current_virt, flags);

        // Track the virtual pages as allocated in your bitmap tracker
        size_t bit = (size_t)virt_start_page + i;
        virt_bitmap[bit / 8] |= (1U << (bit % 8));
    }

    irq_restore(irq_flags);
    return (void*)(start_vaddr + page_offset);
}

static int find_contiguous_bits(uint8_t* bitmap, size_t bitmap_size, size_t num_bits) {
    size_t count = 0;
    int start_bit = -1;
    size_t total_bits = bitmap_size * 8;

    for (size_t i = 0; i < total_bits; i++) {
        bool is_allocated = (bitmap[i / 8] & (1U << (i % 8))) != 0;
        if (!is_allocated) {
            if (count == 0) start_bit = (int)i;
            count++;
            if (count == num_bits) return start_bit;
        } else {
            count = 0;
            start_bit = -1;
        }
    }
    return -1;
}

uint32_t vmm_get_phys_addr(void* virtual_addr) {
    uint32_t vaddr = (uint32_t)virtual_addr;

    // Extract the page directory (top 10 bits) and page table (middle 10 bits) indexes
    uint32_t pd_index = vaddr >> 22;
    uint32_t pt_index = (vaddr >> 12) & 0x3FFU;

    // Extract the lower 12 bits (the offset within the 4KB page)
    uint32_t page_offset = vaddr & 0xFFFU;

    // Not in the kernel tables: see vmm_user_get_phys()
    if (pde_is_user(pd_index))
        return 0;

    // Check if the page directory entry is present
    if (!(page_directory[pd_index] & VM_PRESENT))
        return 0;

    // Lookup the page table entry from your static multi-dimensional array
    uint32_t pte = static_page_tables[pd_index][pt_index];

    // Check if the individual page is actually marked present
    if (!(pte & VM_PRESENT))
        return 0;

    // Mask out the page flags to get the base physical frame address,
    // then append the original page offset.
    return (pte & ~0xFFFU) + page_offset;
}

void* vmm_get_virt_addr(uint32_t physical_addr) {
    uint32_t target_frame = physical_addr & ~0xFFFU;
    uint32_t page_offset  = physical_addr & 0xFFFU;

    // Scan through the kernel half of the page directory
    for (uint32_t pd_index = 0; pd_index < VM_PAGE_DIR_SIZE; pd_index++) {
        if (pde_is_user(pd_index))
            continue;

        // Check if this page table is present
        if (page_directory[pd_index] & VM_PRESENT) {

            // Check through this specific page table of entries
            for (uint32_t pt_index = 0; pt_index < VM_PAGE_TABLE_SIZE; pt_index++) {
                uint32_t pte = static_page_tables[pd_index][pt_index];

                // If the page is present and matches our target physical frame
                if ((pte & VM_PRESENT) && ((pte & ~0xFFFU) == target_frame)) {

                    // Reconstruct the virtual address from the indices and offset
                    uint32_t virtual_addr = (pd_index << 22) | (pt_index << 12) | page_offset;
                    return (void*)virtual_addr;
                }
            }
        }
    }

    // Return NULL if no virtual mapping exists for this physical address
    return NULL;
}

//
// User address spaces
//

// Map a frame at the scratch page and return its kernel address.
// Interrupts must be off until scratch_unmap().
static void* scratch_map(uint32_t phys) {
    static_page_tables[VM_SCRATCH >> 22][(VM_SCRATCH >> 12) & 0x3FFU] =
        (phys & ~0xFFFU) | VM_PRESENT | VM_READWRITE;
    invlpg(VM_SCRATCH);
    return (void*)VM_SCRATCH;
}

static void scratch_unmap(void) {
    static_page_tables[VM_SCRATCH >> 22][(VM_SCRATCH >> 12) & 0x3FFU] = 0;
    invlpg(VM_SCRATCH);
}

static bool space_is_current(const AddressSpace* space) {
    return (read_cr3() & ~0xFFFU) == space->directory_phys;
}

// Page table covering vaddr, created on demand. Interrupts off.
static uint32_t* space_table(AddressSpace* space, uint32_t vaddr, bool create) {
    uint32_t pd_index = vaddr >> 22;
    uint32_t slot = pd_index - VM_USER_PDE_FIRST;

    uint32_t* table = space->tables[slot];
    if (table != NULL || !create)
        return table;

    table = (uint32_t*)vmm_alloc_pages(1);
    if (table == NULL)
        return NULL;
    memset(table, 0, PAGE_SIZE);

    space->tables[slot] = table;

    // The directory entry allows everything; each PTE decides user access
    // and writability (the CPU combines both levels, most restrictive wins)
    space->directory[pd_index] = vmm_get_phys_addr(table) | VM_PRESENT | VM_READWRITE | VM_USER;
    return table;
}

// The PTE for vaddr, or 0 if there is none. Interrupts off.
static uint32_t space_pte(AddressSpace* space, uint32_t vaddr) {
    uint32_t* table = space_table(space, vaddr, false);
    return table ? table[(vaddr >> 12) & 0x3FFU] : 0;
}

// Interrupts off. Refuses to replace an existing mapping.
static bool user_map_locked(AddressSpace* space, uint32_t phys, uint32_t vaddr, uint32_t flags) {
    uint32_t* table = space_table(space, vaddr, true);
    if (table == NULL)
        return false;

    uint32_t index = (vaddr >> 12) & 0x3FFU;
    if (table[index] & VM_PRESENT)
        return false;

    table[index] = (phys & ~0xFFFU) | flags | VM_PRESENT | VM_USER;

    // A not-present entry is never cached, but stale entries from an earlier
    // mapping of this address are, on some CPUs, until invalidated
    if (space_is_current(space))
        invlpg(vaddr);
    return true;
}

AddressSpace* vmm_space_create(void) {
    AddressSpace* space = (AddressSpace*)vmm_alloc_pages(1);
    if (space == NULL)
        return NULL;
    memset(space, 0, PAGE_SIZE);

    uint32_t* dir = (uint32_t*)vmm_alloc_pages(1);
    if (dir == NULL) {
        vmm_free_pages(space, 1);
        return NULL;
    }

    // Kernel entries never change after vmm_init(), so this copy stays valid
    memcpy(dir, page_directory, PAGE_SIZE);
    for (uint32_t i = 0; i < VM_USER_PDE_COUNT; i++)
        dir[VM_USER_PDE_FIRST + i] = 0;

    space->directory      = dir;
    space->directory_phys = vmm_get_phys_addr(dir);
    return space;
}

void vmm_space_destroy(AddressSpace* space) {
    if (space == NULL)
        return;

    uint32_t irq_flags = irq_save();

    // Never free the directory the CPU is walking
    if (space_is_current(space))
        write_cr3((uint32_t)page_directory);

    for (uint32_t slot = 0; slot < VM_USER_PDE_COUNT; slot++) {
        uint32_t* table = space->tables[slot];
        if (table == NULL)
            continue;

        for (uint32_t i = 0; i < VM_PAGE_TABLE_SIZE; i++) {
            uint32_t pte = table[i];
            if ((pte & VM_PRESENT) && (pte & VM_OWNED))
                pmm_free_frame(pte & ~0xFFFU);
        }
        vmm_free_pages(table, 1);
    }

    vmm_free_pages(space->directory, 1);
    irq_restore(irq_flags);

    vmm_free_pages(space, 1);
}

void vmm_switch_address_space(AddressSpace* space) {
    write_cr3(space ? space->directory_phys : (uint32_t)page_directory);
}

uint32_t vmm_kernel_directory_phys(void) {
    return (uint32_t)page_directory;
}

bool vmm_user_map_page(AddressSpace* space, uint32_t physical_addr, uint32_t vaddr, uint32_t flags) {
    if (space == NULL || (vaddr & 0xFFFU) || !vmm_is_user_address(vaddr))
        return false;

    uint32_t irq_flags = irq_save();
    bool ok = user_map_locked(space, physical_addr, vaddr, flags & VM_USER_FLAG_MASK);
    irq_restore(irq_flags);
    return ok;
}

bool vmm_user_alloc(AddressSpace* space, uint32_t vaddr, size_t num_pages, uint32_t flags) {
    if (space == NULL || (vaddr & 0xFFFU) || num_pages == 0)
        return false;
    if (num_pages > (VM_USER_END - VM_USER_START) / PAGE_SIZE)
        return false;
    if (!vmm_is_user_range(vaddr, (uint32_t)num_pages * PAGE_SIZE))
        return false;

    flags = (flags & VM_USER_FLAG_MASK) | VM_OWNED;

    size_t done = 0;
    for (; done < num_pages; done++) {
        uint32_t page  = vaddr + (uint32_t)done * PAGE_SIZE;
        uint32_t frame = pmm_alloc_frame();
        if (frame == PMM_NO_FRAME)
            break;

        uint32_t irq_flags = irq_save();

        // Never hand ring 3 a frame that still holds someone else's data
        memset(scratch_map(frame), 0, PAGE_SIZE);
        scratch_unmap();

        bool ok = user_map_locked(space, frame, page, flags);
        irq_restore(irq_flags);

        if (!ok) {
            pmm_free_frame(frame);
            break;
        }
    }

    if (done == num_pages)
        return true;

    // Undo the pages mapped so far
    vmm_user_free(space, vaddr, done);
    return false;
}

void vmm_user_free(AddressSpace* space, uint32_t vaddr, size_t num_pages) {
    if (space == NULL || (vaddr & 0xFFFU))
        return;

    for (size_t i = 0; i < num_pages; i++) {
        uint32_t page = vaddr + (uint32_t)i * PAGE_SIZE;
        if (!vmm_is_user_address(page))
            break;

        uint32_t irq_flags = irq_save();
        uint32_t* table = space_table(space, page, false);
        if (table != NULL) {
            uint32_t index = (page >> 12) & 0x3FFU;
            uint32_t pte = table[index];
            if (pte & VM_PRESENT) {
                table[index] = 0;
                if (space_is_current(space))
                    invlpg(page);   // Before the frame can be reused
                if (pte & VM_OWNED)
                    pmm_free_frame(pte & ~0xFFFU);
            }
        }
        irq_restore(irq_flags);
    }
}

uint32_t vmm_user_get_phys(AddressSpace* space, uint32_t vaddr) {
    if (space == NULL || !vmm_is_user_address(vaddr))
        return 0;

    uint32_t irq_flags = irq_save();
    uint32_t pte = space_pte(space, vaddr);
    irq_restore(irq_flags);

    if (!(pte & VM_PRESENT))
        return 0;
    return (pte & ~0xFFFU) | (vaddr & 0xFFFU);
}

bool vmm_user_check(AddressSpace* space, uint32_t addr, uint32_t len, bool write) {
    if (space == NULL || !vmm_is_user_range(addr, len))
        return false;
    if (len == 0)
        return true;

    uint32_t first = addr & ~0xFFFU;
    uint32_t last  = (addr + len - 1) & ~0xFFFU;   // Cannot wrap: range checked above
    bool ok = true;

    uint32_t irq_flags = irq_save();
    for (uint32_t page = first; ; page += PAGE_SIZE) {
        uint32_t pte = space_pte(space, page);
        if (!(pte & VM_PRESENT) || !(pte & VM_USER) || (write && !(pte & VM_READWRITE))) {
            ok = false;
            break;
        }
        if (page == last)
            break;
    }
    irq_restore(irq_flags);
    return ok;
}

static bool user_copy(AddressSpace* space, uint32_t vaddr, uint8_t* kbuf, uint32_t len, bool to_user) {
    if (space == NULL || !vmm_is_user_range(vaddr, len))
        return false;

    while (len > 0) {
        uint32_t offset = vaddr & 0xFFFU;
        uint32_t chunk  = PAGE_SIZE - offset;
        if (chunk > len)
            chunk = len;

        // One page per critical section, so interrupts are never off long
        uint32_t irq_flags = irq_save();
        uint32_t pte = space_pte(space, vaddr);
        if (!(pte & VM_PRESENT)) {
            irq_restore(irq_flags);
            return false;
        }

        uint8_t* window = (uint8_t*)scratch_map(pte & ~0xFFFU) + offset;
        if (to_user)
            memcpy(window, kbuf, chunk);
        else
            memcpy(kbuf, window, chunk);
        scratch_unmap();
        irq_restore(irq_flags);

        vaddr += chunk;
        kbuf  += chunk;
        len   -= chunk;
    }
    return true;
}

bool vmm_user_copy_to(AddressSpace* space, uint32_t vaddr, const void* src, uint32_t len) {
    return user_copy(space, vaddr, (uint8_t*)src, len, true);
}

bool vmm_user_copy_from(AddressSpace* space, void* dst, uint32_t vaddr, uint32_t len) {
    return user_copy(space, vaddr, (uint8_t*)dst, len, false);
}
