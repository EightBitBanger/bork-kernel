#ifndef _OPCODE_FUNCTIONS_H
#define _OPCODE_FUNCTIONS_H

#include <stdio.h>
#include <stdint.h>

#include <kernel/util/string.h>
#include <kernel/util/delay.h>
#include <kernel/kernel.h>

#include <kernel/emulation/x4/x4.h>

// Opcodes

#define  NOP   0x90

#define  MOVR    0x88  // Move a byte from a register into a register
#define  MOVB    0x89  // Move a byte into a register
#define  MOVA    0x83  // Move an address into a register word
#define  MOVMW   0x82  // Move a register byte into a memory address
#define  MOVMR   0x81  // Move a byte from a memory address into a register

#define  INC     0xFD
#define  DEC     0xFC

#define  IN      0x85
#define  OUT     0x84

#define  ADD     0x00
#define  SUB     0x80
#define  MUL     0xF6
#define  DIV     0xF4

#define  AND     0x20
#define  OR      0x21
#define  XOR     0x22
#define  NOT     0x23

#define  POP     0x0F
#define  PUSH    0xF0

#define  CMP     0x38
#define  CMPR    0x39

#define  JMP     0xFE
#define  JE      0x74
#define  JNE     0xF3
#define  JG      0x75
#define  JL      0xF1
#define  JZ      0x72
#define  JNZ     0xF9

#define  CALL    0x9A
#define  RET     0xCB

#define  INT     0xCD

#define  STI     0xFB
#define  CLI     0xFA

// Registers

#define  rAL       0x00
#define  rAH       0x01
#define  rBL       0x02
#define  rBH       0x03
#define  rCL       0x04
#define  rCH       0x05
#define  rDL       0x06
#define  rDH       0x07
#define  rEP       0x08

uint8_t isc_display_routine(struct X4Thread* thread, char** op_args, uint8_t arg_count);
uint8_t isc_keyboard_routine(struct X4Thread* thread, char** op_args, uint8_t arg_count);
uint8_t isc_network_routine(struct X4Thread* thread, char** op_args, uint8_t arg_count);
uint8_t isc_filesystem_routine(struct X4Thread* thread, char** op_args, uint8_t arg_count);
uint8_t isc_operating_system(struct X4Thread* thread, char** op_args, uint8_t arg_count);

struct Bus mem_bus;

static inline void x4_writeb(uint32_t address, uint8_t byte) {
    mmio_writeb(&mem_bus, address, &byte);
}

static inline void x4_readb(uint32_t address, uint8_t* byte) {
    mmio_readb(&mem_bus, address, byte);
}

struct X4Pointer get_pointer_from_bytes(uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3);

// Debug

static inline void debug_print_opcode_hex(const char* name) {
#ifdef X4_DEBUG_ENABLE
    print(name);
    print("\n");
#endif
}

static inline void debug_print_register_8_by_index(uint8_t reg) {
    switch (reg) {
        case rAH: print("AH"); break;
        case rAL: print("AL"); break;
        case rBH: print("BH"); break;
        case rBL: print("BL"); break;
        case rCH: print("CH"); break;
        case rCL: print("CL"); break;
        case rDH: print("DH"); break;
        case rDL: print("DL"); break;
        default: print_hex(reg); break;
    }
}

static inline void debug_print_register_16_by_index(uint8_t reg) {
    switch (reg) {
        case rAL: print("AX"); break;
        case rBL: print("BX"); break;
        case rCL: print("CX"); break;
        case rDL: print("DX"); break;
        default: print_hex(reg); break;
    }
}

static inline void debug_print_opcode_hex8x1(const char* name, uint8_t reg) {
#ifdef X4_DEBUG_ENABLE
    print(name);
    print(" ");
    debug_print_register_16_by_index(reg);
    print("\n");
#endif
}

static inline void debug_print_opcode_hex8x2(const char* name, uint8_t reg, uint8_t argB) {
#ifdef X4_DEBUG_ENABLE
    print(name);
    print(" ");
    debug_print_register_8_by_index(reg);
    print(", ");
    print_hex(argB);
    print("\n");
#endif
}

static inline void debug_print_opcode_hex8x3(const char* name, char operation, uint8_t argA, uint8_t argB, uint8_t argC) {
#ifdef X4_DEBUG_ENABLE
    print(name);
    print(" ");
    print_hex(argA);
    print(" = ");
    print_hex(argB);
    
    print(" ");
    char* op = " ";
    op[0] = operation;
    print(op);
    print(" ");
    
    print_hex(argC);
    print("\n");
#endif
}

static inline void debug_print_opcode_hex32(const char* name, uint8_t address) {
#ifdef X4_DEBUG_ENABLE
    print(name);
    print(" ");
    print_hex32(address);
    print("\n");
#endif
}

static inline void debug_print_opcode_hex16_8(const char* name, uint8_t reg, uint16_t address) {
#ifdef X4_DEBUG_ENABLE
    print(name);
    print(" ");
    debug_print_register_16_by_index(reg);
    print(", ");
    print_hex16(address);
    print("\n");
#endif
}

static inline void debug_print_opcode_hex32_8(const char* name, uint8_t reg, uint32_t address) {
#ifdef X4_DEBUG_ENABLE
    print(name);
    print(" ");
    debug_print_register_8_by_index(reg);
    print(", ");
    print_hex16(address);
    print("\n");
#endif
}

static inline void debug_print_unknown_opcode(uint32_t exe_ptr, uint8_t opcode, uint8_t argA, uint8_t argB, uint8_t argC, uint8_t argD) {
#ifdef X4_DEBUG_ENABLE
    print("? ");
    
    print_hex(opcode);
    print(" ");
    print_hex(argA);
    print(" ");
    print_hex(argB);
    print(" ");
    print_hex(argC);
    print(" ");
    print_hex(argD);
    
    print("\n");
    
    print_hex(exe_ptr);
    print("\n");
    
#endif
}

static inline void debug_print_line_begin(uint8_t line) {
#ifdef X4_DEBUG_ENABLE
    print_hex(line);
    print(" ");
#endif
}


// Stack

static inline uint8_t stack_pop(struct X4Thread* thread) {
    uint8_t byte;
    thread->cache.sp--;
    x4_readb(thread->cache.ss + thread->cache.sp, &byte);
    return byte;
}

static inline void stack_push(struct X4Thread* thread, uint8_t byte) {
    x4_writeb(thread->cache.ss + thread->cache.sp, byte);
    thread->cache.sp++;
}


static inline void stack_push_ptr(struct X4Thread* thread, uint32_t address) {
    uint8_t* bytes = (uint8_t*)&address;
    stack_push(thread, bytes[0]);
    stack_push(thread, bytes[1]);
    stack_push(thread, bytes[2]);
    stack_push(thread, bytes[3]);
}

static inline uint32_t stack_pop_ptr(struct X4Thread* thread) {
    uint32_t address;
    uint8_t* bytes = (uint8_t*)&address;
    bytes[3] = stack_pop(thread);
    bytes[2] = stack_pop(thread);
    bytes[1] = stack_pop(thread);
    bytes[0] = stack_pop(thread);
    return address;
}

static inline void update_compare_flags(uint8_t* state, uint8_t left, uint8_t right) {
    if (left == right) {
        *state |=  X4EMM_FLAG_CMP_EQUAL;
        *state &= ~X4EMM_FLAG_CMP_LESS;
        *state &= ~X4EMM_FLAG_CMP_GREATER;
        *state |=  X4EMM_FLAG_ZERO;
    } else if (left > right) {
        *state &= ~X4EMM_FLAG_CMP_EQUAL;
        *state |=  X4EMM_FLAG_CMP_GREATER;
        *state &= ~X4EMM_FLAG_CMP_LESS;
        *state &= ~X4EMM_FLAG_ZERO;
    } else {
        *state &= ~X4EMM_FLAG_CMP_EQUAL;
        *state &= ~X4EMM_FLAG_CMP_GREATER;
        *state |=  X4EMM_FLAG_CMP_LESS;
        *state &= ~X4EMM_FLAG_ZERO;
    }
}

static inline void update_zero_flag(uint8_t* state, uint8_t value) {
    if (value == 0) {
        *state |= X4EMM_FLAG_ZERO;
    } else {
        *state &= ~X4EMM_FLAG_ZERO;
    }
}


//
// Opcodes
//

static inline void opcode_MOVMR(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x2("MOV", op_args[0], op_args[1]);
#endif
    struct X4Pointer ptr = get_pointer_from_bytes(op_args[1], op_args[2], op_args[3], op_args[4]);
    x4_readb(ptr.address, &((uint8_t*)&thread->cache)[op_args[0]]);
}

static inline void opcode_MOVMW(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("MOV", op_args[0]);
#endif    
    struct X4Pointer ptr = get_pointer_from_bytes(op_args[1], op_args[2], op_args[3], op_args[4]);
    x4_writeb(ptr.address, ((uint8_t*)&thread->cache)[op_args[0]]);
}

static inline void opcode_MOVR(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x2("MOV", op_args[0], op_args[1]);
#endif    
    ((uint8_t*)&thread->cache)[op_args[0]] = ((uint8_t*)&thread->cache)[op_args[1]];
}

static inline void opcode_MOVB(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x2("MOV", op_args[0], op_args[1]);
#endif    
    ((uint8_t*)&thread->cache)[op_args[0]] = op_args[1];
}

static inline void opcode_MOVA(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex16_8("MOV", op_args[0], (uint32_t)op_args[1]);
#endif    
    ((uint8_t*)&thread->cache)[op_args[0] + 0] = op_args[1];// Move bytes to full 16 bit register
    ((uint8_t*)&thread->cache)[op_args[0] + 1] = op_args[2];
}



static inline void opcode_INC(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* reg = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x1("INC", op_args[0]);
#endif    
    (*reg)++;
    update_zero_flag(&thread->cache.state, *reg);
}

static inline void opcode_DEC(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* reg = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x1("DEC", op_args[0]);
#endif    
    (*reg)--;
    update_zero_flag(&thread->cache.state, *reg);
}



static inline void opcode_ADD(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* dst = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x3("ADD", '+', op_args[0], op_args[0], op_args[1]);
#endif    
    *dst += ((uint8_t*)&thread->cache)[op_args[1]];
    update_zero_flag(&thread->cache.state, *dst);
}

static inline void opcode_SUB(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* dst = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x3("SUB", '-', op_args[0], op_args[0], op_args[1]);
#endif    
    *dst -= ((uint8_t*)&thread->cache)[op_args[1]];
    update_zero_flag(&thread->cache.state, *dst);
}

static inline void opcode_MUL(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* dst = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x3("MUL", '*', op_args[0], op_args[1], op_args[2]);
#endif    
    *dst = ((uint8_t*)&thread->cache)[op_args[1]] * ((uint8_t*)&thread->cache)[op_args[2]];
    update_zero_flag(&thread->cache.state, *dst);
}

static inline void opcode_DIV(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* dst = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x3("DIV", '/', op_args[0], op_args[1], op_args[2]);
#endif    
    *dst = ((uint8_t*)&thread->cache)[op_args[1]] / ((uint8_t*)&thread->cache)[op_args[2]];
    update_zero_flag(&thread->cache.state, *dst);
}



static inline void opcode_PUSH(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x1("PUSH", op_args[0]);
#endif    
    stack_push(thread, ((uint8_t*)&thread->cache)[op_args[0]]);
}

static inline void opcode_POP(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x1("POP", op_args[0]);
#endif    
    ((uint8_t*)&thread->cache)[op_args[0]] = stack_pop(thread);
}



static inline void opcode_AND(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* dst = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x2("AND", op_args[0], op_args[1]);
#endif    
    *dst = *dst & ((uint8_t*)&thread->cache)[op_args[1]];
    update_zero_flag(&thread->cache.state, *dst);
}

static inline void opcode_OR(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* dst = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x2("OR", op_args[0], op_args[1]);
#endif    
    *dst = *dst | ((uint8_t*)&thread->cache)[op_args[1]];
    update_zero_flag(&thread->cache.state, *dst);
}

static inline void opcode_XOR(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* dst = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x2("XOR", op_args[0], op_args[1]);
#endif    
    *dst = *dst ^ ((uint8_t*)&thread->cache)[op_args[1]];
    update_zero_flag(&thread->cache.state, *dst);
}

static inline void opcode_NOT(struct X4Thread* thread, uint8_t* op_args) {
    uint8_t* dst = &((uint8_t*)&thread->cache)[op_args[0]];
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x1("NOT", op_args[0]);
#endif    
    *dst = ~(*dst);
    update_zero_flag(&thread->cache.state, *dst);
}



static inline void opcode_CMP(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x2("CMP", op_args[0], op_args[1]);
#endif    
    update_compare_flags(&thread->cache.state, ((uint8_t*)&thread->cache)[op_args[0]], op_args[1]);
}

static inline void opcode_CMPR(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x2("CMPR", op_args[0], op_args[1]);
#endif    
    update_compare_flags(&thread->cache.state, ((uint8_t*)&thread->cache)[op_args[0]], ((uint8_t*)&thread->cache)[op_args[1]]);
}



static inline void opcode_JMP(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("JMP", (uint32_t)op_args[0]);
#endif    
    struct X4Pointer ptr = get_pointer_from_bytes(op_args[0], op_args[1], op_args[2], op_args[3]);
    thread->cache.ep = ptr.address;
}

static inline void opcode_JE(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("JE", (uint32_t)op_args[0]);
#endif    
    if (thread->cache.state & X4EMM_FLAG_CMP_EQUAL) {
        struct X4Pointer ptr = get_pointer_from_bytes(op_args[0], op_args[1], op_args[2], op_args[3]);
        thread->cache.ep = ptr.address;
        return;
    }
    thread->cache.ep += 4;
}

static inline void opcode_JNE(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("JNE", (uint32_t)op_args[0]);
#endif    
    if (!(thread->cache.state & X4EMM_FLAG_CMP_EQUAL)) {
        struct X4Pointer ptr = get_pointer_from_bytes(op_args[0], op_args[1], op_args[2], op_args[3]);
        thread->cache.ep = ptr.address;
        return;
    }
    thread->cache.ep += 4;
}

static inline void opcode_JG(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("JG", (uint32_t)op_args[0]);
#endif    
    if (thread->cache.state & X4EMM_FLAG_CMP_GREATER) {
        struct X4Pointer ptr = get_pointer_from_bytes(op_args[0], op_args[1], op_args[2], op_args[3]);
        thread->cache.ep = ptr.address;
        return;
    }
    thread->cache.ep += 4;
}

static inline void opcode_JL(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("JL", (uint32_t)op_args[0]);
#endif    
    if (thread->cache.state & X4EMM_FLAG_CMP_LESS) {
        struct X4Pointer ptr = get_pointer_from_bytes(op_args[0], op_args[1], op_args[2], op_args[3]);
        thread->cache.ep = ptr.address;
        return;
    }
    thread->cache.ep += 4;
}

static inline void opcode_JZ(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("JZ", (uint32_t)op_args[0]);
#endif    
    if (thread->cache.state & X4EMM_FLAG_ZERO) {
        struct X4Pointer ptr = get_pointer_from_bytes(op_args[0], op_args[1], op_args[2], op_args[3]);
        thread->cache.ep = ptr.address;
        return;
    }
    thread->cache.ep += 4;
}

static inline void opcode_JNZ(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("JNZ", (uint32_t)op_args[0]);
#endif    
    if (!(thread->cache.state & X4EMM_FLAG_ZERO)) {
        struct X4Pointer ptr = get_pointer_from_bytes(op_args[0], op_args[1], op_args[2], op_args[3]);
        thread->cache.ep = ptr.address;
        return;
    }
    thread->cache.ep += 4;
}



static inline void opcode_CALL(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex32("CALL", (uint32_t)op_args[0]);
#endif    
    struct X4Pointer ptr = get_pointer_from_bytes(op_args[0], op_args[1], op_args[2], op_args[3]);
    uint32_t return_address = thread->cache.ep + 4;
    
    stack_push_ptr(thread, thread->cache.bp);
    stack_push_ptr(thread, return_address);
    
    thread->cache.bp = thread->cache.sp;
    thread->cache.ep = ptr.address;
}

static inline void opcode_RET(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex("RET");
#endif    
    thread->cache.sp = thread->cache.bp;
    thread->cache.ep = stack_pop_ptr(thread);
    thread->cache.bp = stack_pop_ptr(thread);
}

static inline void opcode_CLI(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex("INT");
#endif    
    __asm__("cli");
    thread->cache.state &= ~X4EMM_FLAG_INTERRUPT;
}

static inline void opcode_STI(struct X4Thread* thread, uint8_t* op_args) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex("SEI");
#endif    
    __asm__("sei");
    thread->cache.state |=  X4EMM_FLAG_INTERRUPT;
}

static inline uint8_t opcode_INT(struct X4Thread* thread, uint8_t* op_args, char** agruments, uint8_t arg_count) {
#ifdef X4_DEBUG_PRINT_INSTRUCTIONS
    debug_print_opcode_hex8x1("INT", op_args[0]);
#endif    
    uint8_t result=0;
    
    switch (op_args[0]) {
        
    case 0x10: result = isc_display_routine(thread, agruments, arg_count); break;
    case 0x13: result = isc_filesystem_routine(thread, agruments, arg_count); break;
    //case 0x14: result = isc_network_routine(thread, op_args, arg_count); break;
    //case 0x47: result = isc_operating_system(thread, op_args, arg_count); break;
    case 0x16: result = isc_keyboard_routine(thread, agruments, arg_count); break;
        
    }
    
    // INT 20 - End the program
    if (op_args[0] == 0x20) {
        if (thread->cache.state & X4EMM_FLAG_CONSOLE_DIRTY) {
            thread->cache.state &= ~X4EMM_FLAG_CONSOLE_DIRTY;
            print("\n");
        }
        
        // Quit the process
        return 0xff;
    }
    
    if (result == 0xD6) {
        // Re execute the INT next time
        thread->cache.ep -= 2;
        
        // Yield to the next process
        return 0xD6;
    }
    return 0;
}

#endif
