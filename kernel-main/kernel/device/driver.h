#ifndef _DRIVER_H_
#define _DRIVER_H_

#include <stdint.h>

struct Driver {
    char name[16];
    
    uint8_t is_linked;
    
    
    
    void (*read)(uint32_t address, uint8_t* buffer);
    void (*write)(uint32_t address, uint8_t buffer);
    
    //void (*probe)(uint32_t address, uint8_t buffer);
    
};

#endif
