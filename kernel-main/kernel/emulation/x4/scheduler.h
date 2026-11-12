#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <kernel/emulation/x4/x4.h>

#define TASK_FLAG_SUSPENDED    0x01

struct X4ProcessBlock {
    char name[16];
    
    uint64_t pid;
    uint16_t flags;
    uint8_t priority;
    
    uint32_t code_segment_address;
    uint32_t stack_segment_address;
    
    struct X4Thread thread_main;
};

struct ProcessDescriptor {};


void scheduler_init(void);
void scheduler_stop(void);

uint32_t task_create(uint32_t entry_point_address, uint16_t priority, uint8_t flags);

void scheduler_handler(void);

// Run a program from a file
uint8_t execute_program(char* filename, char** args, uint8_t arg_count);

// Run a program from a buffer
uint8_t run_program(uint32_t code_segment, uint32_t stack_segment, char** args, uint8_t arg_count);

void scheduler_isr_callback(void);

#endif
