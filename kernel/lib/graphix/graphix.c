#include <kernel/lib/graphix.h>

uint32_t gpu_address;

struct Bus gpu_bus;

union Resigner {
    uint8_t unsign;
    int8_t sign;
};

void SwapBuffers(void) {
    gl_busy_wait();
    uint8_t trigger_draw = 1;
    mmio_writeb(&gpu_bus, gpu_address + 0x0000F, &trigger_draw); // Trigger a buffer swap
}

int8_t gl_init(uint8_t mode) {
    gpu_address = device_get_hardware_address("display");
    if (gpu_address == 0) 
        return 0;
    
    gpu_bus.read_waitstate  = 10;
    gpu_bus.write_waitstate = 10;
    
    gl_busy_wait();
    mmio_writeb(&gpu_bus, gpu_address + 0x00000, &mode);
    return 1;
}

void glBufferSetSize(uint8_t size) {
    gl_busy_wait();
    mmio_writeb(&gpu_bus, gpu_address + 0x00008, &size);
}

void glUniformTranslate(float x, float y, float z) {
    gl_busy_wait();
    uint8_t offset = 0; // Byte offset to the uniform
    
    union FloatBytes translateX;
    union FloatBytes translateY;
    union FloatBytes translateZ;
    translateX.value = x;
    translateY.value = y;
    translateZ.value = z;
    
    uint8_t uniform_rq = 4;
    mmio_writeb(&gpu_bus, gpu_address + 0x00007, &uniform_rq); // Access uniform values
    for (uint8_t i=0; i < 4; i++) {
        mmio_writeb(&gpu_bus, gpu_address + 0x00010 + i +     offset, &translateX.bytes[i]);
        mmio_writeb(&gpu_bus, gpu_address + 0x00010 + i + 4 + offset, &translateY.bytes[i]);
        mmio_writeb(&gpu_bus, gpu_address + 0x00010 + i + 8 + offset, &translateZ.bytes[i]);
    }
    
    uint8_t zero = 0;
    mmio_writeb(&gpu_bus, gpu_address + 0x00007, &zero); // Reset to VRAM
}

void glUniformRotation(float yaw, float pitch, float roll) {
    gl_busy_wait();
    uint8_t offset = 12; // Byte offset to the uniform
    
    union FloatBytes rotationX;
    union FloatBytes rotationY;
    union FloatBytes rotationZ;
    
    rotationX.value = yaw;
    rotationY.value = pitch;
    rotationZ.value = roll;
    
    uint8_t uniform_rq = 4;
    mmio_writeb(&gpu_bus, gpu_address + 0x00007, &uniform_rq); // Access uniform values
    for (uint8_t i=0; i < 4; i++) {
        mmio_writeb(&gpu_bus, gpu_address + 0x000010 + i +     offset, &rotationX.bytes[i]);
        mmio_writeb(&gpu_bus, gpu_address + 0x000010 + i + 4 + offset, &rotationY.bytes[i]);
        mmio_writeb(&gpu_bus, gpu_address + 0x000010 + i + 8 + offset, &rotationZ.bytes[i]);
    }
    uint8_t zero = 0;
    mmio_writeb(&gpu_bus, gpu_address + 0x00007, &zero); // Reset to VRAM
}

void glUniformScale(float scale) {
    gl_busy_wait();
    uint8_t offset = 24; // Byte offset to the uniform
    
    union FloatBytes uniformScale;
    uniformScale.value = scale;
    
    uint8_t uniform_rq = 4;
    mmio_writeb(&gpu_bus, gpu_address + 0x00007, &uniform_rq); // Access uniform values
    for (uint8_t i=0; i < 4; i++) 
        mmio_writeb(&gpu_bus, gpu_address + 0x000010 + i + offset, &uniformScale.bytes[i]);
    
    uint8_t zero = 0;
    mmio_writeb(&gpu_bus, gpu_address + 0x00007, &zero); // Reset to VRAM
}

void glBegin(uint8_t primitive) {
    gl_busy_wait();
    mmio_writeb(&gpu_bus, gpu_address + 0x0000A, &primitive); // Set primitive drawing type
}

void glBufferData(int8_t* buffer, uint16_t size) {
    gl_busy_wait();
    uint8_t zero = 0;
    uint8_t buffer_rq = 3;
    mmio_writeb(&gpu_bus, gpu_address + 0x00001, &zero);
    mmio_writeb(&gpu_bus, gpu_address + 0x00002, &zero);
    mmio_writeb(&gpu_bus, gpu_address + 0x00007, &buffer_rq);
    for (uint16_t i=0; i < size; i++) {
        union Resigner signswap;
        signswap.sign = buffer[i];
        
        mmio_writeb(&gpu_bus, gpu_address + 0x00010 + i, &signswap.unsign);
    }
    uint8_t buffer_size = size / 10;
    mmio_writeb(&gpu_bus, gpu_address + 0x00007, &zero);        // Back to VRAM
    mmio_writeb(&gpu_bus, gpu_address + 0x00008, &buffer_size); // Set the buffer size
}

void glDrawBuffer(uint8_t begin, uint8_t size) {
    gl_busy_wait();
    uint8_t trigger_draw = 1;
    mmio_writeb(&gpu_bus, gpu_address + 0x0000C, &begin);   // Starting offset
    mmio_writeb(&gpu_bus, gpu_address + 0x0000D, &size);    // Number of bytes
    mmio_writeb(&gpu_bus, gpu_address + 0x0000E, &trigger_draw);  // Trigger the draw
    gl_busy_wait();
}

void glClear(uint8_t character) {
    gl_busy_wait();
    uint8_t trigger_clear = 1;
    mmio_writeb(&gpu_bus, gpu_address + 0x00009, &trigger_clear);
}


void gl_busy_wait(void) {
    uint8_t checkByte = 0;
    mmio_readb(&gpu_bus, gpu_address, &checkByte);
    while (checkByte != 0x10) 
        mmio_readb(&gpu_bus, gpu_address, &checkByte);
}
