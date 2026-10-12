#ifndef _EMULATOR_X4__
#define _EMULATOR_X4__

#define  X4EMM_FLAG_INTERRUPT          0x01
#define  X4EMM_FLAG_ZERO               0x02
#define  X4EMM_FLAG_CMP_EQUAL          0x04
#define  X4EMM_FLAG_CMP_GREATER        0x08
#define  X4EMM_FLAG_CMP_LESS           0x10
#define  X4EMM_FLAG_CONSOLE_DIRTY      0x20

#include <stdint.h>

// Debug

//#define X4_DEBUG_ENABLE

//#define X4_DEBUG_PRINT_INSTRUCTIONS
//#define X4_DEBUG_PRINT_STACK_POINTER
//#define X4_DEBUG_PRINT_CALL_STACK

//#define X4_DEBUG_STEP_ENABLE
//#define X4_DEBUG_STEP_DELAY



struct __attribute__((packed)) X4Pointer {
    uint32_t address;
};

struct __attribute__((packed)) X4CpuRegisters {
    
    // General base registers
    
    uint8_t al;
    uint8_t ah;
    uint8_t bl;
    uint8_t bh;
    uint8_t cl;
    uint8_t ch;
    uint8_t dl;
    uint8_t dh;
    
    // Pointers
    
    uint32_t ep;    // Execution pointer
    uint32_t bp;    // Stack frame base pointer
    uint32_t sp;    // Stack pointer
    
    // Flags
    
    uint8_t state;
    
    // Segments
    
    uint32_t cs;    // Code segment
    uint32_t ds;    // Data segment
    uint32_t ss;    // Stack segment
};

struct X4Thread {
    struct X4CpuRegisters cache;
};

void x4_init(void);

uint8_t x4_emulate(struct X4Thread* thread, char** args, uint8_t arg_count, uint32_t steps);

uint32_t AssembleJoin(uint8_t* buffer, uint32_t begin_address, uint8_t* source, uint32_t length);

#endif
