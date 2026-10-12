#include <stdint.h>
#include <kernel/kernel.h>
#include <kernel/emulation/x4/x4.h>

struct X4Pointer x4_pointer_from_low16(uint8_t low, uint8_t high);

uint32_t AssembleJoin(uint8_t* buffer, uint32_t begin_address, uint8_t* source, uint32_t length) {
    
    for (uint32_t i=0; i < length; i++) 
        buffer[begin_address + i] = source[i];
    
    return begin_address + length;
}

void x4_read_string_from_dx(struct X4Thread* thread, char* str) {
    struct X4Pointer data_offset = x4_pointer_from_low16(thread->cache.dl, thread->cache.dh);
    for (uint32_t i=0;i < 16; i++) {
        kmem_read(&str[i], thread->cache.cs + data_offset.address + i, 1);
        if (str[i] == '\0') 
            break;
    }
    str[16] = '\0';
}

struct X4Pointer get_pointer_from_bytes(uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3) {
    struct X4Pointer pointer;
    ((uint8_t*)&pointer.address)[0] = byte0;
    ((uint8_t*)&pointer.address)[1] = byte1;
    ((uint8_t*)&pointer.address)[2] = byte2;
    ((uint8_t*)&pointer.address)[3] = byte3;
    return pointer;
}
