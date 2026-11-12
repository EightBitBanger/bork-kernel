#include <stdio.h>
#include <stdint.h>

#include <kernel/util/string.h>
#include <kernel/util/delay.h>
#include <kernel/kernel.h>

#include <kernel/emulation/x4/x4.h>
#include <kernel/emulation/x4/opcodes.h>


#include <kernel/boot/avr/interrupt.h>



extern struct Bus mem_bus;

void x4_init(void) {
    mem_bus.read_waitstate  = 2;
    mem_bus.write_waitstate = 1;
}

uint8_t x4_emulate(struct X4Thread* thread, char** args, uint8_t arg_count, uint32_t steps) {
    
#ifdef X4_DEBUG_PRINT_STACK_POINTER
    debug_print_opcode_hex32("SP ", thread->cache.sp);
    debug_print_opcode_hex32("BP ", thread->cache.bp);
#endif
    
    uint32_t counter=0;
    while (counter < steps) {
        counter++;
        
#ifdef X4_DEBUG_STEP_ENABLE
        if (kb_check_input_state() == 0) 
            continue;
        kb_clear_input_state();
        if (kb_getc() == 0x00) 
            continue;
#endif
        
#ifdef X4_DEBUG_STEP_DELAY
        _delay_ms(800);
#endif
        
        uint8_t instruction[6];
        for (uint8_t i=0; i < 6; i++) 
            x4_readb(thread->cache.cs + thread->cache.ep + i, &instruction[i]);
        
        uint8_t opcode = instruction[0];
        uint8_t argA   = instruction[1];
        uint8_t argB   = instruction[2];
        uint8_t argC   = instruction[3];
        uint8_t argD   = instruction[4];
        uint8_t argE   = instruction[5];
        
        uint8_t* op_args = &instruction[1];
        
        debug_print_line_begin(thread->cache.ep);
        
        thread->cache.ep++;
        
#ifdef X4_DEBUG_PRINT_CALL_STACK
        
 #ifndef X4_DEBUG_PRINT_INSTRUCTIONS
        print("\n");
 #endif
#endif
        
        switch (opcode) {
        
        case MOVMR: opcode_MOVMR(thread, op_args); thread->cache.ep += 5; continue;
        case MOVMW: opcode_MOVMW(thread, op_args); thread->cache.ep += 5; continue;
        
        case MOVR: opcode_MOVR(thread, op_args); thread->cache.ep += 2; continue;
        case MOVB: opcode_MOVB(thread, op_args); thread->cache.ep += 2; continue;
        
        case MOVA: opcode_MOVA(thread, op_args); thread->cache.ep += 5; continue;
        
        case INC: opcode_INC(thread, op_args); thread->cache.ep += 1; continue;
        case DEC: opcode_DEC(thread, op_args); thread->cache.ep += 1; continue;
        
        case ADD: opcode_ADD(thread, op_args); thread->cache.ep += 2; continue;
        case SUB: opcode_SUB(thread, op_args); thread->cache.ep += 2; continue;
        case MUL: opcode_MUL(thread, op_args); thread->cache.ep += 3; continue;
        case DIV: opcode_DIV(thread, op_args); thread->cache.ep += 3; continue;
        
        case POP:  opcode_POP(thread, op_args);  thread->cache.ep += 1; continue;
        case PUSH: opcode_PUSH(thread, op_args); thread->cache.ep += 1; continue;
        
        case AND: opcode_AND(thread, op_args); thread->cache.ep += 2; continue;
        case OR:  opcode_OR(thread, op_args);  thread->cache.ep += 2; continue;
        case XOR: opcode_XOR(thread, op_args); thread->cache.ep += 2; continue;
        case NOT: opcode_NOT(thread, op_args); thread->cache.ep += 1; continue;
        
        case CMP:  opcode_CMP(thread, op_args);  thread->cache.ep += 2; continue;
        case CMPR: opcode_CMPR(thread, op_args); thread->cache.ep += 2; continue;
        
        case JMP: opcode_JMP(thread, op_args); continue;
        case JE:  opcode_JE(thread, op_args); continue;
        case JNE: opcode_JNE(thread, op_args); continue;
        case JG:  opcode_JG(thread, op_args); continue;
        case JL:  opcode_JL(thread, op_args); continue;
        case JZ:  opcode_JZ(thread, op_args); continue;
        case JNZ: opcode_JNZ(thread, op_args); continue;
        
        case CALL: opcode_CALL(thread, op_args); continue;
        case RET:  opcode_RET(thread, op_args); continue;
        
        case CLI: opcode_CLI(thread, op_args); continue;
        case STI: opcode_STI(thread, op_args); continue;
        
        case 0xCC:
        case INT: {
            uint8_t result = opcode_INT(thread, op_args, args, arg_count);
            if (result != 0) {
                if (thread->cache.state & X4EMM_FLAG_CONSOLE_DIRTY) {
                    thread->cache.state &= ~X4EMM_FLAG_CONSOLE_DIRTY;
                    print("\n");
                }
                
                if (result == 0xD6) {
                    return result;
                }
                
#ifdef X4_DEBUG_PRINT_STACK_POINTER
                print("SP "); print_hex32(thread->cache.sp); print("\n");
                print("BP "); print_hex32(thread->cache.bp); print("\n");
#endif
                return 1;
            }
            thread->cache.ep += 1;
            continue;
            }
            
        case NOP: continue;
        
        default: debug_print_unknown_opcode(thread->cache.ep, opcode, op_args[0], op_args[1], op_args[2], op_args[3]); return 1;
        
        }
        
    }
    
    if (thread->cache.state & X4EMM_FLAG_CONSOLE_DIRTY) {
        thread->cache.state &= ~X4EMM_FLAG_CONSOLE_DIRTY;
        print("\n");
    }
    
#ifdef X4_DEBUG_PRINT_STACK_POINTER
    debug_print_opcode_hex32("SP ", thread->cache.sp);
    debug_print_opcode_hex32("BP ", thread->cache.bp);
#endif
    return 0;
}
