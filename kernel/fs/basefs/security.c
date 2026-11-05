#include <kernel/fs/fs.h>
#include <kernel/util/string.h>

// These only touch the shared FSBlockHeader, so they work on directories too

// The object's group policy id (POLICY_* in kernel/security/policy.h).
// basefs only stores it: what an id means, and who may set it, is decided
// above (vfs_policy.c). 0 is "none".
bool fs_file_get_policy(struct FSDeviceContext* ctx, uint64_t address, uint32_t* policy) {
    if (!ctx || address == FS_NULL || policy == NULL) return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    *policy = header.policy;
    return true;
}

bool fs_file_set_policy(struct FSDeviceContext* ctx, uint64_t address, uint32_t policy) {
    if (!ctx || address == FS_NULL) return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    header.policy = policy;
    fs_mem_write(ctx, address, &header, sizeof(struct FSBlockHeader));
    return true;
}

bool fs_file_get_security_flag(struct FSDeviceContext* ctx, uint64_t address, uint8_t* security) {
    if (!ctx || address == FS_NULL || security == NULL) return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    // Note: the stored security field is 32 bits; this getter returns only
    // its low byte (the setter takes the full 32-bit value)
    *security = (uint8_t)header.security;
    return true;
}

bool fs_file_get_security(struct FSDeviceContext* ctx, uint64_t address, uint32_t* security) {
    if (!ctx || address == FS_NULL || security == NULL) return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    *security = header.security;
    return true;
}

bool fs_file_set_security_flag(struct FSDeviceContext* ctx, uint64_t address, uint32_t security) {
    if (!ctx || address == FS_NULL) return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    header.security = security;
    fs_mem_write(ctx, address, &header, sizeof(struct FSBlockHeader));
    return true;
}


