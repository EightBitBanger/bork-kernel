#ifndef _KERNEL_SYS_RANDOM_H_
#define _KERNEL_SYS_RANDOM_H_

#include <stdint.h>

#include <kernel/syscall/syscall.h>
#include <kernel/syscall/calls/sys_types.h>
#include <kernel/syscall/calls/sys_user.h>
#include <kernel/scheduler/process.h>
#include <kernel/arch/x86/drivers/rng.h>
#include <kernel/crypt/random.h>
#include <kernel/crypt/crypt.h>
#include <kernel/util/timer.h>

//
// random(buf, length) -> bytes written
//
// Random bytes for keys and nonces (TLS), from the kernel's pool
// (crypt/random.h): SHA-1 over everything stirred in so far. Each call
// stirs first with what this machine has that is hard to guess: RDSEED /
// RDRAND when the CPU has them, the cycle counter, the time, and the
// caller. The net thread stirs in every frame's arrival time, the Wi-Fi
// driver its radio noise. At most SYS_RANDOM_MAX bytes a call (fewer are
// written, and said, for a longer buffer). Always allowed: the bytes say
// nothing about anything.
//
//   SYS_EFAULT   buf is not writable user memory
//

static int32_t sys_random(const SyscallArgs* args) {
    uint32_t length = args->arg2;
    if (length > SYS_RANDOM_MAX)
        length = SYS_RANDOM_MAX;
    if (length == 0)
        return 0;

    uint32_t mix[12];
    for (int i = 0; i < 8; i++) {
        uint32_t v = 0;
        if (!rng_get_seed(&v))
            (void)rng_get_rand(&v);
        mix[i] = v;
    }
    uint64_t cycles = rng_cycles();
    mix[8]  = (uint32_t)cycles;
    mix[9]  = (uint32_t)(cycles >> 32);
    mix[10] = (uint32_t)timer_get_ms();
    mix[11] = (uint32_t)process_current_id();
    crypt_random_add(mix, sizeof(mix));
    crypt_wipe(mix, sizeof(mix));

    uint8_t bytes[SYS_RANDOM_MAX];
    crypt_random_bytes(bytes, length);

    AddressSpace* space = process_current_space();
    bool ok = space != NULL && sys_user_put(space, args->arg1, bytes, length);
    crypt_wipe(bytes, sizeof(bytes));
    return ok ? (int32_t)length : SYS_EFAULT;
}

#endif
