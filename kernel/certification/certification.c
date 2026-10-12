#include <stdint.h>
#include <stddef.h>

#include <kernel/fs/fs.h>

uint32_t crypt_checksum(const struct FSBlockHeader* block) {
    // FNV-1a constants for 32-bit hash
    uint32_t hash = 2166136261U;
    const uint8_t* data = (const uint8_t*)block;
    
    // Skip the 4-byte certificate at the end
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









//
// TESTING...
//




static uint32_t KERNEL_BOOT_SECRET = 0;

/**
 * @brief Called once during kernel boot to initialize the security layer.
 * If you don't have a hardware RNG component yet, you can seed this
 * using a timer tick or a CMOS value.
 */
void security_init(uint32_t seed) {
    // A simple placeholder LCG to mix up the boot seed
    KERNEL_BOOT_SECRET = (seed * 1103515245U + 12345U) ^ 0xDEADBEEF;
}

/**
 * @brief Generates a certificate tied to this specific kernel instance session.
 */
uint32_t security_gen_session_cert(const struct FSBlockHeader* block) {
    uint32_t hash = KERNEL_BOOT_SECRET;
    const uint8_t *data = (const uint8_t *)block;
    size_t hash_length = sizeof(struct FSBlockHeader) - sizeof(block->certificate);

    // Mix the boot secret with the struct data
    for (size_t i = 0; i < hash_length; i++) {
        hash = ((hash << 5) + hash) + data[i]; 
    }

    return hash;
}

/**
 * @brief Verifies the block was signed during this kernel session.
 */
bool security_verify_session_cert(const struct FSBlockHeader* block) {
    return (block->certificate == security_gen_session_cert(block));
}
