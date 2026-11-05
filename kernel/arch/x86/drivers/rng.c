#include <kernel/arch/x86/drivers/rng.h>

#define CPUID1_ECX_RDRAND   (1U << 30)
#define CPUID1_EDX_TSC      (1U << 4)
#define CPUID7_EBX_RDSEED   (1U << 18)

// CPUID is asked once; until then nothing is assumed
static bool features_known = false;
static bool has_rdrand = false;
static bool has_rdseed = false;
static bool has_tsc = false;

static void rng_detect(void) {
    if (features_known)
        return;
    uint32_t max, eax, ebx, ecx, edx;
    eax = 0;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    max = eax;
    if (max >= 1) {
        eax = 1;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
        has_rdrand = (ecx & CPUID1_ECX_RDRAND) != 0;
        has_tsc    = (edx & CPUID1_EDX_TSC) != 0;
    }
    if (max >= 7) {
        eax = 7;
        ecx = 0;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
        has_rdseed = (ebx & CPUID7_EBX_RDSEED) != 0;
    }
    features_known = true;
}

bool rng_get_rand(uint32_t* out_val) {
    rng_detect();
    if (!has_rdrand)
        return false;   // Executing it would be an invalid opcode (#UD)

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
    rng_detect();
    if (!has_rdseed)
        return false;

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

uint64_t rng_cycles(void) {
    rng_detect();
    if (!has_tsc)
        return 0;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
