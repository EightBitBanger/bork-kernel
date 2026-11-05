#include <kernel/arch/x86/drivers/nvme/nvme.h>
#include <kernel/arch/x86/virtual/pmm.h>
#include <kernel/arch/x86/virtual/vmm.h>
#include <kernel/console/print.h>
#include <kernel/util/string.h>
#include <kernel/util/timer.h>
#include <kernel/mutex.h>
#include <kernel/block/block.h>

#define NVME_MAX_CONTROLLERS    4
#define NVME_MAX_NAMESPACES     16      // Across all controllers

// Entries per queue (capped by CAP.MQES). 64 x 64-byte commands fill the
// submission queue's single page.
#define NVME_QUEUE_DEPTH        64

// Bytes per command: 16 pages at most, so the PRP list never needs more
// than 16 of its 512 entries even when no two pages are adjacent
#define NVME_CHUNK_BYTES        (16U * PAGE_SIZE)
#define NVME_CHUNK_PAGES        16

// Namespace IDs tried one by one when the controller has no active
// namespace list (before NVMe 1.1)
#define NVME_SCAN_NSID_MAX      64

#define NVME_ADMIN_TIMEOUT_MS   5000
#define NVME_IO_TIMEOUT_MS      10000
#define NVME_FLUSH_TIMEOUT_MS   30000

// Controller registers (offsets into BAR0)
#define NVME_REG_CAP            0x00    // 64-bit
#define NVME_REG_VS             0x08
#define NVME_REG_INTMS          0x0C
#define NVME_REG_CC             0x14
#define NVME_REG_CSTS           0x1C
#define NVME_REG_AQA            0x24
#define NVME_REG_ASQ            0x28    // 64-bit
#define NVME_REG_ACQ            0x30    // 64-bit
#define NVME_REG_DOORBELLS      0x1000

#define NVME_CC_EN              (1U << 0)
#define NVME_CC_IOSQES          (6U << 16)  // 64-byte submission entries
#define NVME_CC_IOCQES          (4U << 20)  // 16-byte completion entries

#define NVME_CSTS_RDY           (1U << 0)
#define NVME_CSTS_CFS           (1U << 1)

// Admin opcodes
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09

#define NVME_FEATURE_QUEUES     0x07

#define NVME_CNS_NAMESPACE      0x00
#define NVME_CNS_CONTROLLER     0x01
#define NVME_CNS_ACTIVE_NSIDS   0x02

// NVM command set opcodes
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02

#define NVME_IO_QUEUE_ID        1

struct NvmeCommand {
    uint32_t cdw0;          // Opcode (7:0), command ID (31:16)
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct NvmeCompletion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;        // Phase tag (bit 0), status field (15:1)
};

_Static_assert(sizeof(struct NvmeCommand) == 64, "NVMe command is 64 bytes");
_Static_assert(sizeof(struct NvmeCompletion) == 16, "NVMe completion is 16 bytes");

struct NvmeQueue {
    uint16_t id;
    uint16_t size;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t phase;         // Phase tag the next new completion carries
    uint16_t next_cid;

    struct NvmeCommand*             sq;
    volatile struct NvmeCompletion* cq;
    uint32_t sq_phys;
    uint32_t cq_phys;

    volatile uint32_t* sq_doorbell;
    volatile uint32_t* cq_doorbell;
};

struct NvmeController {
    volatile uint8_t* regs;
    uint16_t pci_location;
    uint32_t ready_timeout_ms;      // CAP.TO
    uint32_t max_transfer_bytes;    // MDTS, capped at NVME_CHUNK_BYTES
    bool     volatile_cache;        // VWC: flush does something
    bool     failed;                // Disabled after a fault: no more commands

    struct NvmeQueue admin;
    struct NvmeQueue io;

    uint64_t* prp_list;             // One page, for transfers over two pages
    uint32_t  prp_list_phys;
    uint8_t*  bounce;               // NVME_CHUNK_BYTES, for unaligned buffers
    uint8_t*  identify;             // One page for IDENTIFY data
    uint32_t  identify_phys;

    mutex_t   lock;                 // One command at a time
    uint32_t  namespace_first;      // Index into namespaces[]
    uint32_t  namespace_count;
};

struct NvmeNamespace {
    struct NvmeController* ctrl;
    uint32_t nsid;
    uint32_t block_size;
    uint64_t block_count;
};

static struct NvmeController controllers[NVME_MAX_CONTROLLERS];
static uint32_t controller_count = 0;

static struct NvmeNamespace namespaces[NVME_MAX_NAMESPACES];
static uint32_t namespace_total = 0;

//
// Registers
//

static uint32_t nvme_read32(struct NvmeController* ctrl, uint32_t offset) {
    return *(volatile uint32_t*)(ctrl->regs + offset);
}

static void nvme_write32(struct NvmeController* ctrl, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(ctrl->regs + offset) = value;
}

// 64-bit registers as two 32-bit accesses, low half first (no 64-bit MMIO
// on i386)
static uint64_t nvme_read64(struct NvmeController* ctrl, uint32_t offset) {
    uint64_t low  = nvme_read32(ctrl, offset);
    uint64_t high = nvme_read32(ctrl, offset + 4);
    return low | (high << 32);
}

static void nvme_write64(struct NvmeController* ctrl, uint32_t offset, uint64_t value) {
    nvme_write32(ctrl, offset, (uint32_t)value);
    nvme_write32(ctrl, offset + 4, (uint32_t)(value >> 32));
}

// Wait for CSTS.RDY to read `ready`. False on a timeout or a fatal status.
static bool nvme_wait_ready(struct NvmeController* ctrl, bool ready) {
    uint64_t start = timer_get_ms();
    for (;;) {
        uint32_t csts = nvme_read32(ctrl, NVME_REG_CSTS);
        if (csts == 0xFFFFFFFF)
            return false;   // Gone from the bus
        if (((csts & NVME_CSTS_RDY) != 0) == ready)
            return true;
        if (ready && (csts & NVME_CSTS_CFS))
            return false;
        if (timer_get_ms() - start > ctrl->ready_timeout_ms)
            return false;
        __asm__ volatile ("pause");
    }
}

// Reset the controller (CC.EN = 0). This aborts whatever it is doing and
// stops its DMA. False if it never reports not-ready: then the queue pages
// may still be written by it and must never be handed out again.
static bool nvme_disable(struct NvmeController* ctrl) {
    uint32_t cc = nvme_read32(ctrl, NVME_REG_CC);
    if (cc & NVME_CC_EN)
        nvme_write32(ctrl, NVME_REG_CC, cc & ~NVME_CC_EN);
    return nvme_wait_ready(ctrl, false);
}

// Give up on a controller that misbehaved: reset it so it stops DMA, and
// refuse every later command. Its pages are never freed.
static void nvme_fail(struct NvmeController* ctrl, const char* why) {
    if (ctrl->failed) return;
    ctrl->failed = true;
    print("NVMe Error: ");
    print(why);
    print(", controller disabled.\n");
    nvme_disable(ctrl);
}

//
// Memory
//

static void* nvme_alloc_page(uint32_t* phys_out) {
    void* virt = vmm_alloc_pages(1);
    if (virt == NULL) return NULL;

    memset(virt, 0, PAGE_SIZE);
    *phys_out = vmm_get_phys_addr(virt);
    if (*phys_out == 0) {
        vmm_free_pages(virt, 1);
        return NULL;
    }
    return virt;
}

static void nvme_free_page(void* virt) {
    if (virt != NULL) vmm_free_pages(virt, 1);
}

static void nvme_free_memory(struct NvmeController* ctrl) {
    nvme_free_page(ctrl->admin.sq);
    nvme_free_page((void*)ctrl->admin.cq);
    nvme_free_page(ctrl->io.sq);
    nvme_free_page((void*)ctrl->io.cq);
    nvme_free_page(ctrl->prp_list);
    nvme_free_page(ctrl->identify);
    if (ctrl->bounce != NULL) vmm_free_pages(ctrl->bounce, NVME_CHUNK_PAGES);
}

static bool nvme_alloc_queue(struct NvmeController* ctrl, struct NvmeQueue* q, uint16_t id, uint16_t size,
                             uint32_t doorbell_stride) {
    q->id       = id;
    q->size     = size;
    q->sq_tail  = 0;
    q->cq_head  = 0;
    q->phase    = 1;
    q->next_cid = 0;
    q->sq = (struct NvmeCommand*)nvme_alloc_page(&q->sq_phys);
    q->cq = (volatile struct NvmeCompletion*)nvme_alloc_page(&q->cq_phys);

    uint32_t sq_offset = NVME_REG_DOORBELLS + (2U * id) * doorbell_stride;
    q->sq_doorbell = (volatile uint32_t*)(ctrl->regs + sq_offset);
    q->cq_doorbell = (volatile uint32_t*)(ctrl->regs + sq_offset + doorbell_stride);

    return q->sq != NULL && q->cq != NULL;
}

//
// Commands
//

// Submit one command on `q` and poll for its completion. Completions that
// do not answer it (a wrong command or queue ID) are consumed and ignored.
// `result`, if not NULL, gets completion dword 0.
static bool nvme_submit(struct NvmeController* ctrl, struct NvmeQueue* q, struct NvmeCommand* cmd,
                        uint32_t* result, uint32_t timeout_ms) {
    if (ctrl->failed) return false;

    uint16_t cid = q->next_cid++;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFFU) | ((uint32_t)cid << 16);

    memcpy(&q->sq[q->sq_tail], cmd, sizeof(struct NvmeCommand));
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->size);

    // The command must be in memory before the doorbell says it is there
    __sync_synchronize();
    *q->sq_doorbell = q->sq_tail;

    uint64_t start = timer_get_ms();
    uint64_t spins = 0;
    for (;;) {
        volatile struct NvmeCompletion* cqe = &q->cq[q->cq_head];
        uint16_t status = cqe->status;

        if ((status & 1U) == q->phase) {
            // Read the rest of the entry only after its phase tag
            __sync_synchronize();
            uint16_t got_cid = cqe->command_id;
            uint16_t got_sq  = cqe->sq_id;
            uint32_t dw0     = cqe->result;
            status = cqe->status;

            if (++q->cq_head == q->size) {
                q->cq_head = 0;
                q->phase ^= 1U;
            }
            *q->cq_doorbell = q->cq_head;

            if (got_cid == cid && got_sq == q->id) {
                uint16_t code = (uint16_t)(status >> 1);
                if (code != 0) {
                    print("NVMe Error: command failed.\n");
                    return false;
                }
                if (result != NULL) *result = dw0;
                return true;
            }
            // Not ours: keep waiting for the real answer
        }

        // Check the controller now and then: MMIO reads are slow
        if ((++spins & 0x3FF) == 0) {
            uint32_t csts = nvme_read32(ctrl, NVME_REG_CSTS);
            if (csts == 0xFFFFFFFF || (csts & NVME_CSTS_CFS)) {
                nvme_fail(ctrl, "controller fatal status");
                return false;
            }
        }

        // The spin count only matters if the timer has stopped (interrupts
        // off); it is far longer than the timeout at any real speed
        if (timer_get_ms() - start > timeout_ms || spins > (uint64_t)timeout_ms * 1000000ULL) {
            // Still pending: the controller may yet DMA into the buffer,
            // so it is reset rather than left running
            nvme_fail(ctrl, "command timed out");
            return false;
        }
        __asm__ volatile ("pause");
    }
}

static bool nvme_identify(struct NvmeController* ctrl, uint8_t cns, uint32_t nsid) {
    struct NvmeCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_ADMIN_IDENTIFY;
    cmd.nsid  = nsid;
    cmd.prp1  = ctrl->identify_phys;   // One page-aligned page: no PRP2
    cmd.cdw10 = cns;

    memset(ctrl->identify, 0, PAGE_SIZE);
    return nvme_submit(ctrl, &ctrl->admin, &cmd, NULL, NVME_ADMIN_TIMEOUT_MS);
}

// Little-endian fields of IDENTIFY data
static uint16_t nvme_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t nvme_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t nvme_le64(const uint8_t* p) {
    return (uint64_t)nvme_le32(p) | ((uint64_t)nvme_le32(p + 4) << 32);
}

// Describe `bytes` at `buffer` (dword aligned, at most NVME_CHUNK_BYTES)
// with PRP1/PRP2. Virtually contiguous memory is not physically contiguous,
// so each page is translated on its own.
static bool nvme_build_prps(struct NvmeController* ctrl, const uint8_t* buffer, uint32_t bytes,
                            uint64_t* prp1, uint64_t* prp2) {
    uint32_t vaddr = (uint32_t)buffer;
    uint32_t phys  = vmm_get_phys_addr((void*)vaddr);
    if (phys == 0) return false;

    *prp1 = phys;
    *prp2 = 0;

    uint32_t first = PAGE_SIZE - (vaddr & (PAGE_SIZE - 1));
    if (bytes <= first) return true;

    vaddr += first;
    uint32_t remaining = bytes - first;

    if (remaining <= PAGE_SIZE) {
        phys = vmm_get_phys_addr((void*)vaddr);
        if (phys == 0) return false;
        *prp2 = phys;
        return true;
    }

    uint32_t entries = 0;
    while (remaining > 0) {
        if (entries >= PAGE_SIZE / sizeof(uint64_t)) return false;
        phys = vmm_get_phys_addr((void*)vaddr);
        if (phys == 0) return false;
        ctrl->prp_list[entries++] = phys;

        uint32_t chunk = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;
        vaddr     += chunk;
        remaining -= chunk;
    }
    *prp2 = ctrl->prp_list_phys;
    return true;
}

// One read or write of at most max_transfer_bytes. Lock held.
static bool nvme_rw_chunk(struct NvmeNamespace* ns, uint8_t opcode, uint64_t lba, uint32_t count, uint8_t* buffer) {
    struct NvmeController* ctrl = ns->ctrl;
    uint32_t bytes = count * ns->block_size;

    // PRP entries must be dword aligned; anything else goes through the
    // bounce buffer
    bool bounce = ((uint32_t)buffer & 3U) != 0;
    uint8_t* target = bounce ? ctrl->bounce : buffer;
    if (bounce && opcode == NVME_CMD_WRITE)
        memcpy(target, buffer, bytes);

    struct NvmeCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = opcode;
    cmd.nsid  = ns->nsid;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (count - 1) & 0xFFFFU;     // 0-based block count

    if (!nvme_build_prps(ctrl, target, bytes, &cmd.prp1, &cmd.prp2)) {
        print("NVMe Error: buffer cannot be described for DMA.\n");
        return false;
    }

    if (!nvme_submit(ctrl, &ctrl->io, &cmd, NULL, NVME_IO_TIMEOUT_MS))
        return false;

    if (bounce && opcode == NVME_CMD_READ)
        memcpy(buffer, target, bytes);
    return true;
}

static bool nvme_transfer(struct NvmeNamespace* ns, uint8_t opcode, uint64_t lba, uint32_t count, uint8_t* buffer) {
    if (ns == NULL || buffer == NULL || count == 0) return false;
    if (lba >= ns->block_count || count > ns->block_count - lba) return false;

    struct NvmeController* ctrl = ns->ctrl;
    uint32_t chunk_blocks = ctrl->max_transfer_bytes / ns->block_size;

    mutex_lock(&ctrl->lock);
    bool ok = true;
    while (count > 0 && ok) {
        uint32_t chunk = count < chunk_blocks ? count : chunk_blocks;
        ok = nvme_rw_chunk(ns, opcode, lba, chunk, buffer);
        lba    += chunk;
        count  -= chunk;
        buffer += chunk * ns->block_size;
    }
    mutex_unlock(&ctrl->lock);
    return ok;
}

bool nvme_read_blocks(struct NvmeNamespace* ns, uint64_t lba, uint32_t count, void* buffer) {
    return nvme_transfer(ns, NVME_CMD_READ, lba, count, (uint8_t*)buffer);
}

bool nvme_write_blocks(struct NvmeNamespace* ns, uint64_t lba, uint32_t count, const void* buffer) {
    return nvme_transfer(ns, NVME_CMD_WRITE, lba, count, (uint8_t*)buffer);
}

bool nvme_flush(struct NvmeNamespace* ns) {
    if (ns == NULL) return false;

    struct NvmeController* ctrl = ns->ctrl;
    if (!ctrl->volatile_cache) return !ctrl->failed;

    struct NvmeCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_CMD_FLUSH;
    cmd.nsid = ns->nsid;

    mutex_lock(&ctrl->lock);
    bool ok = nvme_submit(ctrl, &ctrl->io, &cmd, NULL, NVME_FLUSH_TIMEOUT_MS);
    mutex_unlock(&ctrl->lock);
    return ok;
}

//
// Bring-up
//

// Identify one namespace and add it to the table if it can be used: a
// non-zero size and a 512..4096-byte block format without metadata or
// protection information (neither of which the block layer could carry)
static void nvme_add_namespace(struct NvmeController* ctrl, uint32_t nsid) {
    if (namespace_total >= NVME_MAX_NAMESPACES) return;
    if (!nvme_identify(ctrl, NVME_CNS_NAMESPACE, nsid)) return;

    const uint8_t* id = ctrl->identify;
    uint64_t nsze  = nvme_le64(&id[0]);
    uint8_t  nlbaf = id[25];
    uint8_t  flbas = id[26];
    uint8_t  dps   = id[29];

    if (nsze == 0) return;      // Inactive

    // Format index: bits 3:0, plus bits 6:5 above them (NVMe 2.0)
    uint32_t format = (flbas & 0x0FU) | ((uint32_t)((flbas >> 5) & 0x03U) << 4);
    if (format > nlbaf) return;

    uint32_t lbaf  = nvme_le32(&id[128 + format * 4]);
    uint16_t ms    = (uint16_t)(lbaf & 0xFFFFU);
    uint8_t  lbads = (uint8_t)((lbaf >> 16) & 0xFFU);

    if (ms != 0 || (dps & 0x07U) != 0) return;
    if (lbads < 9 || lbads > 12) return;

    uint32_t block_size = 1U << lbads;
    if (ctrl->max_transfer_bytes < block_size) return;

    // Keep sizes in bytes well inside 64 bits (256 PB at 4 KB blocks)
    if (nsze > (1ULL << 46)) return;

    struct NvmeNamespace* ns = &namespaces[namespace_total++];
    ns->ctrl        = ctrl;
    ns->nsid        = nsid;
    ns->block_size  = block_size;
    ns->block_count = nsze;
    ctrl->namespace_count++;
}

static void nvme_scan_namespaces(struct NvmeController* ctrl, uint32_t nn, uint32_t version) {
    ctrl->namespace_first = namespace_total;
    ctrl->namespace_count = 0;

    // The active list (1.1+) is up to 1024 IDs, ascending, ended by 0. Its
    // IDs are copied out first: identifying each one reuses the page.
    if (version >= 0x00010100 && nvme_identify(ctrl, NVME_CNS_ACTIVE_NSIDS, 0)) {
        uint32_t ids[NVME_MAX_NAMESPACES];
        uint32_t count = 0;
        uint32_t previous = 0;
        for (uint32_t i = 0; i < PAGE_SIZE / 4 && count < NVME_MAX_NAMESPACES; i++) {
            uint32_t nsid = nvme_le32(&ctrl->identify[i * 4]);
            if (nsid == 0) break;
            // Ascending, so none repeats, and within NN
            if (nsid <= previous || nsid > nn) break;
            ids[count++] = nsid;
            previous = nsid;
        }
        for (uint32_t i = 0; i < count; i++)
            nvme_add_namespace(ctrl, ids[i]);
        return;
    }

    uint32_t last = nn < NVME_SCAN_NSID_MAX ? nn : NVME_SCAN_NSID_MAX;
    for (uint32_t nsid = 1; nsid <= last; nsid++)
        nvme_add_namespace(ctrl, nsid);
}

static bool nvme_create_io_queues(struct NvmeController* ctrl) {
    struct NvmeCommand cmd;

    // One submission and one completion queue (0-based counts)
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10 = NVME_FEATURE_QUEUES;
    cmd.cdw11 = 0;
    if (!nvme_submit(ctrl, &ctrl->admin, &cmd, NULL, NVME_ADMIN_TIMEOUT_MS))
        return false;

    uint32_t size_and_id = ((uint32_t)(ctrl->io.size - 1) << 16) | NVME_IO_QUEUE_ID;

    // Completion queue first: the submission queue names it. Physically
    // contiguous (one page), interrupts off.
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_ADMIN_CREATE_CQ;
    cmd.prp1  = ctrl->io.cq_phys;
    cmd.cdw10 = size_and_id;
    cmd.cdw11 = 1U;
    if (!nvme_submit(ctrl, &ctrl->admin, &cmd, NULL, NVME_ADMIN_TIMEOUT_MS))
        return false;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_ADMIN_CREATE_SQ;
    cmd.prp1  = ctrl->io.sq_phys;
    cmd.cdw10 = size_and_id;
    cmd.cdw11 = ((uint32_t)NVME_IO_QUEUE_ID << 16) | 1U;
    return nvme_submit(ctrl, &ctrl->admin, &cmd, NULL, NVME_ADMIN_TIMEOUT_MS);
}

struct NvmeController* nvme_find_controller(uint16_t pci_location) {
    for (uint32_t i = 0; i < controller_count; i++) {
        if (controllers[i].pci_location == pci_location)
            return &controllers[i];
    }
    return NULL;
}

struct NvmeController* nvme_init(volatile void* regs, uint32_t regs_size, uint16_t pci_location) {
    if (regs == NULL) return NULL;

    struct NvmeController* existing = nvme_find_controller(pci_location);
    if (existing != NULL) return existing;

    if (controller_count >= NVME_MAX_CONTROLLERS) {
        print("NVMe Error: too many controllers.\n");
        return NULL;
    }

    struct NvmeController* ctrl = &controllers[controller_count];
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->regs = (volatile uint8_t*)regs;
    ctrl->pci_location = pci_location;
    mutex_init(&ctrl->lock);

    uint64_t cap = nvme_read64(ctrl, NVME_REG_CAP);
    if (cap == 0xFFFFFFFFFFFFFFFFULL) return NULL;

    uint32_t mqes   = (uint32_t)(cap & 0xFFFFU);           // 0-based
    uint32_t to     = (uint32_t)((cap >> 24) & 0xFFU);      // 500 ms units
    uint32_t dstrd  = (uint32_t)((cap >> 32) & 0x0FU);
    bool     nvm    = ((cap >> 37) & 1U) != 0;
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0x0FU);

    uint32_t doorbell_stride = 4U << dstrd;
    uint32_t doorbells_end = NVME_REG_DOORBELLS + (2U * NVME_IO_QUEUE_ID + 2U) * doorbell_stride;

    if (mqes == 0 || !nvm || mpsmin != 0 || doorbells_end > regs_size) {
        print("NVMe Error: unsupported controller.\n");
        return NULL;
    }

    ctrl->ready_timeout_ms = to * 500U;
    if (ctrl->ready_timeout_ms < 1000U) ctrl->ready_timeout_ms = 1000U;

    uint16_t depth = (uint16_t)(mqes + 1 < NVME_QUEUE_DEPTH ? mqes + 1 : NVME_QUEUE_DEPTH);

    // Interrupts stay masked: every queue is polled
    if (!nvme_disable(ctrl)) {
        print("NVMe Error: controller did not reset.\n");
        return NULL;   // Nothing allocated yet
    }
    nvme_write32(ctrl, NVME_REG_INTMS, 0xFFFFFFFFU);

    bool queues = nvme_alloc_queue(ctrl, &ctrl->admin, 0, depth, doorbell_stride) &&
                  nvme_alloc_queue(ctrl, &ctrl->io, NVME_IO_QUEUE_ID, depth, doorbell_stride);
    ctrl->prp_list = (uint64_t*)nvme_alloc_page(&ctrl->prp_list_phys);
    ctrl->identify = (uint8_t*)nvme_alloc_page(&ctrl->identify_phys);
    ctrl->bounce   = (uint8_t*)vmm_alloc_pages(NVME_CHUNK_PAGES);
    if (!queues || ctrl->prp_list == NULL || ctrl->identify == NULL || ctrl->bounce == NULL) {
        print("NVMe Error: failed to allocate queues.\n");
        nvme_free_memory(ctrl);
        return NULL;
    }

    nvme_write32(ctrl, NVME_REG_AQA, ((uint32_t)(depth - 1) << 16) | (uint32_t)(depth - 1));
    nvme_write64(ctrl, NVME_REG_ASQ, ctrl->admin.sq_phys);
    nvme_write64(ctrl, NVME_REG_ACQ, ctrl->admin.cq_phys);

    // NVM command set, 4 KB memory pages, round-robin arbitration
    nvme_write32(ctrl, NVME_REG_CC, NVME_CC_IOSQES | NVME_CC_IOCQES | NVME_CC_EN);

    // Counts as a controller from here on: once enabled it may DMA into
    // the queue pages, which are only freed if it can be reset again
    controller_count++;

    if (!nvme_wait_ready(ctrl, true)) {
        nvme_fail(ctrl, "controller did not start");
        return NULL;
    }

    if (!nvme_identify(ctrl, NVME_CNS_CONTROLLER, 0))
        goto fail;

    const uint8_t* id = ctrl->identify;
    uint8_t  mdts = id[77];
    uint8_t  sqes = id[512];
    uint8_t  cqes = id[513];
    uint32_t nn   = nvme_le32(&id[516]);
    ctrl->volatile_cache = (id[525] & 1U) != 0;

    // Entry sizes: required (3:0) at most, and maximum (7:4) at least, ours
    if ((sqes & 0x0FU) > 6 || (sqes >> 4) < 6 || (cqes & 0x0FU) > 4 || (cqes >> 4) < 4) {
        nvme_fail(ctrl, "unsupported queue entry sizes");
        goto fail;
    }

    // MDTS: 2^n minimum-size (4 KB) pages, 0 for no limit
    ctrl->max_transfer_bytes = NVME_CHUNK_BYTES;
    if (mdts != 0 && mdts < 5 && (PAGE_SIZE << mdts) < NVME_CHUNK_BYTES)
        ctrl->max_transfer_bytes = PAGE_SIZE << mdts;

    if (!nvme_create_io_queues(ctrl)) {
        nvme_fail(ctrl, "could not create I/O queues");
        goto fail;
    }

    // NN 0xFFFFFFFF would make the broadcast ID a namespace
    if (nn == 0xFFFFFFFFU) nn = 0xFFFFFFFEU;
    nvme_scan_namespaces(ctrl, nn, nvme_read32(ctrl, NVME_REG_VS));
    return ctrl;

fail:
    nvme_fail(ctrl, "initialization failed");
    // Left in the table, failed: its pages stay allocated in case the
    // reset did not stop it, and a rescan does not try it again
    return NULL;
}

uint32_t nvme_namespace_count(const struct NvmeController* ctrl) {
    return (ctrl == NULL || ctrl->failed) ? 0 : ctrl->namespace_count;
}

struct NvmeNamespace* nvme_get_namespace(struct NvmeController* ctrl, uint32_t index) {
    if (ctrl == NULL || index >= ctrl->namespace_count) return NULL;
    return &namespaces[ctrl->namespace_first + index];
}

//
// Block layer adapter
//

static bool nvme_block_read(struct BlockDevice* bd, uint64_t lba, uint32_t count, void* buffer) {
    return nvme_read_blocks((struct NvmeNamespace*)bd->driver_data, lba, count, buffer);
}

static bool nvme_block_write(struct BlockDevice* bd, uint64_t lba, uint32_t count, const void* buffer) {
    return nvme_write_blocks((struct NvmeNamespace*)bd->driver_data, lba, count, buffer);
}

static bool nvme_block_flush(struct BlockDevice* bd) {
    return nvme_flush((struct NvmeNamespace*)bd->driver_data);
}

static const struct BlockDeviceOps nvme_block_ops = {
    .read  = nvme_block_read,
    .write = nvme_block_write,
    .flush = nvme_block_flush,
};

struct BlockDevice* nvme_create_block_device(struct NvmeNamespace* ns) {
    if (ns == NULL || ns->ctrl->failed) return NULL;

    struct BlockDevice* existing = block_find_driver(ns);
    if (existing != NULL) return existing;

    return block_register("nvme", &nvme_block_ops, ns, ns->block_size, ns->block_count);
}
