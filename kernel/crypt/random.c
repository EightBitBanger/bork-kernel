#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/crypt/crypt.h>
#include <kernel/crypt/random.h>
#include <kernel/crypt/sha1.h>
#include <kernel/util/string.h>
#include <kernel/mutex.h>

static uint8_t  pool[SHA1_LEN];
static uint64_t counter;
static mutex_t  random_lock;    // All zero: unlocked, as mutex_init leaves it

static void pool_add(const void* data, size_t n) {
    sha1_t s;
    sha1_init(&s);
    sha1_update(&s, pool, sizeof(pool));
    sha1_update(&s, data, n);
    sha1_final(&s, pool);
}

void crypt_random_add(const void* data, size_t n) {
    mutex_lock(&random_lock);
    pool_add(data, n);
    mutex_unlock(&random_lock);
}

void crypt_random_bytes(void* out, size_t n) {
    mutex_lock(&random_lock);
    uint8_t* p = (uint8_t*)out;
    uint8_t block[SHA1_LEN];
    while (n > 0) {
        sha1_t s;
        counter++;
        sha1_init(&s);
        sha1_update(&s, pool, sizeof(pool));
        sha1_update(&s, &counter, sizeof(counter));
        sha1_final(&s, block);
        size_t take = n < SHA1_LEN / 2 ? n : SHA1_LEN / 2;     // Half of each hash out ...
        memcpy(p, block, take);
        p += take;
        n -= take;
        pool_add(block + SHA1_LEN / 2, SHA1_LEN / 2);   // ... the other half stirs the pool
    }
    crypt_wipe(block, sizeof(block));
    mutex_unlock(&random_lock);
}
