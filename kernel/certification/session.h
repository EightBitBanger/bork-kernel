#ifndef _CRYPTOGRAPHY_H_
#define _CRYPTOGRAPHY_H_

#include <kernel/crypt/certification.h>

// Generates a 32-bit FNV-1a hash of the block metadata.
bool crypt_verify_checksum(const struct FSBlockHeader* block);

// Verifies if the block's embedded certificate matches its current contents.
uint32_t crypt_checksum(const struct FSBlockHeader* block);

#endif
