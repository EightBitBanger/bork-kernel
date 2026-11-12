#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <stdint.h>

struct Device {
    char name[16];
    
    uint8_t id;
    uint8_t class;
    
    uint8_t  hardware_slot;
    uint32_t hardware_address;
    
    uint32_t block_address;
};

#endif
