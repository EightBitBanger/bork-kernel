#include <kernel/arch/x86/drivers/rng.h>

bool rng_get_rand(uint32_t* out_val) {
    int retry = 10;
    unsigned char ok;
    
    while (retry > 0) {
        // Execute rdrand. 'ok' will capture the Carry Flag state.
        __asm__ volatile("rdrand %0; setc %1"
                         : "=r"(*out_val), "=qm"(ok)
                         :
                         : "cc");
        
        if (ok) {
            return true;
        }
        retry--;
    }
    return false;
}

bool rng_get_seed(uint32_t *out_val) {
    int retry = 100;
    unsigned char ok;
    
    while (retry > 0) {
        __asm__ volatile("rdseed %0; setc %1"
                         : "=r"(*out_val), "=qm"(ok)
                         :
                         : "cc");
        
        if (ok) {
            return true;
        }
        // Emit a "pause" instruction to let the hardware refresh
        //__asm__ volatile("pause");
        retry--;
    }
    return false;
}
