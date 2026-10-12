
#include <kernel/fs/fs.h>
#include <kernel/util/string.h>

// These only touch the shared FSBlockHeader, so they work on directories too

bool fs_file_get_certificate(struct FSDeviceContext* ctx, uint32_t address, uint32_t* cert) {
    if (!ctx || address == FS_NULL || cert == NULL) return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    *cert = header.certificate;
    return true;
}

bool fs_file_set_certificate(struct FSDeviceContext* ctx, uint32_t address, uint32_t cert) {
    if (!ctx) return false;
    if (cert == 0 || cert == 0xFFFFFFFF) 
        return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    header.certificate = cert;
    fs_mem_write(ctx, address, &header, sizeof(struct FSBlockHeader));
    return true;
}

bool fs_file_get_security_flag(struct FSDeviceContext* ctx, uint32_t address, uint8_t* security) {
    if (!ctx || address == FS_NULL || security == NULL) return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    // Note: the stored security field is 32 bits; this getter returns only
    // its low byte (the setter takes the full 32-bit value)
    *security = (uint8_t)header.security;
    return true;
}

bool fs_file_set_security_flag(struct FSDeviceContext* ctx, uint32_t address, uint32_t security) {
    if (!ctx || address == FS_NULL) return false;
    struct FSBlockHeader header;
    fs_mem_read(ctx, address, &header, sizeof(struct FSBlockHeader));
    header.security = security;
    fs_mem_write(ctx, address, &header, sizeof(struct FSBlockHeader));
    return true;
}

