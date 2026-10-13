#ifndef _KERNEL_VIRTUAL_MEMORY_MGR_H_
#define _KERNEL_VIRTUAL_MEMORY_MGR_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/virtual/pmm.h>
#include <kernel/boot/x86/multiboot_info.h>

// Core Control Flags
#define VM_PRESENT       0x001U  // Bit 0: Page is in memory
#define VM_READWRITE     0x002U  // Bit 1: 0 = Read-Only, 1 = Read-Write
#define VM_USER          0x004U  // Bit 2: 0 = Supervisor, 1 = User space

// Cache Configuration Flags
#define VM_PWT           0x008U  // Bit 3: Write-Through caching
#define VM_PCD           0x010U  // Bit 4: Cache Disable

// CPU Profiling Flags
#define VM_ACCESSED      0x020U  // Bit 5: Read or written by CPU
#define VM_DIRTY         0x040U  // Bit 6: Written by CPU

// Advanced Attributes
#define VM_PAT_PTE       0x080U  // Bit 7: Page Attribute Table index (for 4KB pages)
#define VM_GLOBAL        0x100U  // Bit 8: Global page (requires CR4.PGE)
#define VM_PAT_PDE       0x800U  // Bit 12: Page Attribute Table index (for 4MB pages)

// Software-defined (available bit 9). The frame behind this PTE was allocated
// by the VMM for a user address space and goes back to the PMM when the page
// is freed or the space is destroyed. Frames mapped in with
// vmm_user_map_page() (shared memory, MMIO) never carry it.
#define VM_OWNED         0x200U

// Write-combining needs the PAT. Without PAT reprogramming, PWT|PCD selects
// UC (uncached), which is what this macro used to expand to. vmm_init() now
// reprograms PAT entry 1 (PWT=1, PCD=0) from WT to WC, and this macro yields
// the PTE flags that select it. On CPUs without PAT it falls back to UC.
uint32_t vmm_get_write_combining_flags(void);
bool     vmm_has_write_combining(void);

#define VM_WRITE_COMBINING (vmm_get_write_combining_flags())

//
// Address space layout
//
//   0x00000000 - 0x3FFFFFFF   kernel: identity map, kernel VM window
//                             (0x10000000 - 0x1FFFFFFF)
//   0x40000000 - 0xBFFFFFFF   user space, private to each address space
//   0xC0000000 - 0xFFBFFFFF   kernel: identity-mapped MMIO (IOAPIC, ...)
//   0xFFC00000 - 0xFFFFFFFF   kernel page tables (recursive directory entry)
//
// Kernel page tables are allocated from the PMM on demand, the first time
// something is mapped in their 4 MB range, and are never freed. Each new
// kernel directory entry is written into the kernel directory and into every
// live address space at once, so a kernel mapping made at any time shows up
// in every address space.
//
// User PDEs point at page tables owned by one AddressSpace. vmm_map_page()
// and friends only work on the kernel half; use the vmm_user_* calls below
// for user addresses.
//
#define VM_USER_START       0x40000000U
#define VM_USER_END         0xC0000000U
#define VM_USER_STACK_TOP   VM_USER_END                             // Stacks grow down from here
#define VM_USER_PDE_FIRST   (VM_USER_START >> 22)                   // 256
#define VM_USER_PDE_COUNT   ((VM_USER_END - VM_USER_START) >> 22)   // 512

static inline bool vmm_is_user_address(uint32_t addr) {
    return addr >= VM_USER_START && addr < VM_USER_END;
}

// True if [addr, addr + len) lies entirely inside user space (no wraparound)
static inline bool vmm_is_user_range(uint32_t addr, uint32_t len) {
    if (!vmm_is_user_address(addr)) return false;
    return len <= VM_USER_END - addr;
}

// One user address space. Lives in a single kernel page.
typedef struct AddressSpace {
    uint32_t* directory;                    // Kernel-virtual address of the page directory
    uint32_t  directory_phys;               // Physical address (the CR3 value)
    uint32_t* tables[VM_USER_PDE_COUNT];    // Kernel-virtual user page tables, NULL = none yet
    struct AddressSpace* prev;              // Live-space list, so new kernel directory
    struct AddressSpace* next;              // entries reach every directory (VMM-private)
} AddressSpace;

extern uint32_t page_directory[];

void vmm_init(struct MultibootInfo* mbi, uint32_t identity_map_size);

//
// Kernel half (shared by every address space)
//

void* vmm_alloc_pages(size_t num_pages);
void vmm_free_pages(void* virtual_addr, size_t num_pages);
void vmm_unmap_page(uint32_t virtual_addr);

// Map a physical address into kernel space. Panics on a user-space address.
void vmm_map_page(uint32_t physical_addr, uint32_t virtual_addr, uint32_t flags);

// Map a physical hardware address into kernel space
void vmm_map_hardware_region(uint32_t phys_addr, uint32_t virt_addr, uint32_t size, uint32_t flags);

// Map an MMIO region to a kernel virtual address
void* vmm_map_mmio_region(uint32_t phys_addr, uint32_t size_bytes, uint32_t flags);

// Translate a kernel virtual address into a physical address (0 if unmapped
// or a user address; use vmm_user_get_phys() for those)
uint32_t vmm_get_phys_addr(void* virtual_addr);

// Translate a physical address into a kernel virtual address
void* vmm_get_virt_addr(uint32_t physical_addr);

//
// User address spaces
//

// New address space: the kernel half shared, the user half empty.
// Returns NULL when out of memory.
AddressSpace* vmm_space_create(void);

// Free every user page the space owns, its page tables, its directory and
// the AddressSpace itself. If it is the loaded address space, CR3 is switched
// to the kernel directory first.
void vmm_space_destroy(AddressSpace* space);

// Load an address space into CR3 (NULL = kernel directory only)
void vmm_switch_address_space(AddressSpace* space);

// CR3 value of the kernel-only directory
uint32_t vmm_kernel_directory_phys(void);

// Allocate num_pages zeroed frames and map them at vaddr (page aligned) with
// VM_USER set. flags may add VM_READWRITE, VM_PWT, VM_PCD, VM_PAT_PTE; without
// VM_READWRITE the pages are read-only to ring 3. Fails, leaving nothing
// mapped, if any page in the range is already mapped or memory runs out.
bool vmm_user_alloc(AddressSpace* space, uint32_t vaddr, size_t num_pages, uint32_t flags);

// Map a frame the caller owns (shared memory, MMIO) at vaddr with VM_USER set.
// The frame is NOT freed when the page is unmapped. Fails if vaddr is already
// mapped.
bool vmm_user_map_page(AddressSpace* space, uint32_t physical_addr, uint32_t vaddr, uint32_t flags);

// Unmap num_pages starting at vaddr. Frames from vmm_user_alloc() go back to
// the PMM. Unmapped pages in the range are skipped.
void vmm_user_free(AddressSpace* space, uint32_t vaddr, size_t num_pages);

// Physical address behind a user virtual address, 0 if not mapped
uint32_t vmm_user_get_phys(AddressSpace* space, uint32_t vaddr);

// Syscall pointer check: every page of [addr, addr + len) is mapped, user
// accessible and (if write) writable. Only valid until the mapping changes;
// once processes have several threads, hold the space's lock across the use.
bool vmm_user_check(AddressSpace* space, uint32_t addr, uint32_t len, bool write);

// Copy between a kernel buffer and a user address space, which does not need
// to be the loaded one. Ignores page permissions (the loader uses it to fill
// read-only text), so syscalls must vmm_user_check() first. Returns false at
// the first unmapped page; bytes before it have already been copied.
bool vmm_user_copy_to(AddressSpace* space, uint32_t vaddr, const void* src, uint32_t len);
bool vmm_user_copy_from(AddressSpace* space, void* dst, uint32_t vaddr, uint32_t len);

#endif

