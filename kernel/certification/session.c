#include <stdint.h>
#include <stddef.h>

#include <kernel/fs/fs.h>

uint32_t crypt_checksum(const struct FSBlockHeader* block) {
    // FNV-1a constants for 32-bit hash
    uint32_t hash = 2166136261U;
    const uint8_t* data = (const uint8_t*)block;
    
    // Total size of FSBlockHeader is 32 bytes. 
    // We hash the first 28 bytes (skipping the 4-byte certificate at the end).
    size_t hash_length = sizeof(struct FSBlockHeader) - sizeof(block->certificate);
    
    for (size_t i = 0; i < hash_length; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    
    return hash;
}

bool crypt_verify_checksum(const struct FSBlockHeader* block) {
    uint32_t expected_checksum = crypt_checksum(block);
    return (block->certificate == expected_checksum);
}
