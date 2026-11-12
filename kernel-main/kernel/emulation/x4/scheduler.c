#include <kernel/emulation/x4/scheduler.h>

#include <kernel/arch/avr/io.h>
#include <kernel/arch/avr/map.h>
#include <kernel/arch/avr/heap.h>

#include <kernel/boot/avr/interrupt.h>

#include <kernel/kernel.h>
#include <kernel/util/string.h>

#define SCHEDULER_TRIGGER_COUNTER_MAX    32

static uint32_t base_steps     = 8;     // The minimum steps a thread gets
static uint32_t weight         = 8;     // How much each priority level adds
static uint8_t  max_priority   = 16;    // 0 = highest, 10 = lowest

static uint16_t task_index=0;           // 'proc' file name index
static uint16_t process_index=0;        // Running process index
static uint64_t pid_counter=0;          // Unique ID counter

static volatile uint8_t isr_trigger=0;

uint32_t proc_root_node = KNODE_NULL;


void scheduler_init(void) {
    proc_root_node = knode_find_by_name(knode_get_root(), "proc");
    
    // TIMER1, CTC mode, prescaler 64
    TCCR1B = (1 << WGM12) | (1 << CS11) | (1 << CS10);
    TIMSK1 = (1 << OCIE1A);
    OCR1A  = 391;
}

void scheduler_stop(void) {
    TCCR1B = 0;   // Stop TIMER1
    TIMSK1 = 0;   // Disable its interrupts
}

uint32_t task_create(uint32_t entry_point_address, uint16_t priority, uint8_t flags) {
    struct X4Thread thread;
    memset(&thread, 0x00, sizeof(struct X4Thread));
    
    thread.cache.cs = entry_point_address;
    thread.cache.ss = kmalloc(1024);
    
    // Create process node
    
    struct WorkingDirectory fs_current;
    kernel_get_working_directory(&fs_current);
    
    char index_string[24];
    itos(task_index, index_string);
    uint32_t proc_address = create_knode(index_string, proc_root_node);
    
    // Add to "proc" knode
    uint32_t process_block_address = create_procblock("");
    knode_add_reference(proc_address, process_block_address);
    
    // Create the process block
    struct X4ProcessBlock block;
    memset(&block, 0x00, sizeof(struct X4ProcessBlock));
    strcpy(block.name, "block");
    
    block.priority = priority;
    block.pid = pid_counter;
    
    block.code_segment_address  = thread.cache.cs;
    block.stack_segment_address = thread.cache.ss;
    
    memcpy(&block.thread_main, &thread, sizeof(struct X4Thread));
    
    kmem_write(process_block_address, &block, sizeof(struct X4ProcessBlock));
    
    pid_counter++;
    task_index++;
}

void scheduler_handler(void) {
    // Context switch
    if (isr_trigger != 0) {
        isr_trigger = 0;
        process_index++;
    }
    
    uint32_t process_node = knode_get_reference(proc_root_node, process_index);
    if (process_node == KNODE_NULL) {
        
        process_index = 0;
        return;
    }
    
    uint32_t process_block = knode_get_reference(process_node, 0);
    struct X4ProcessBlock block;
    kmem_read(&block, process_block, sizeof(struct X4ProcessBlock));
    
    if (block.flags & TASK_FLAG_SUSPENDED) {
        return;
    }
    
    uint16_t steps = base_steps + (((uint16_t)max_priority) - block.priority) * weight;
    
    uint8_t state = x4_emulate(&block.thread_main, (char*[]){NULL}, 0, steps);
    
    // Process yielded
    if (state == 0xD6) {
        process_index++;
        kmem_write(process_block, &block, sizeof(struct X4ProcessBlock));
        return;
    }
    
    // Process shutdown
    if (state != 0) {
        knode_remove_reference(proc_root_node, process_node);
        knode_remove_reference(process_node, process_block);
        
        kfree(process_block);
        kfree(process_node);
        
        kfree(block.thread_main.cache.ss);
        kfree(block.thread_main.cache.cs);
        return;
    }
    
    kmem_write(process_block, &block, sizeof(struct X4ProcessBlock));
    process_index++;
}

uint8_t execute_program(char* filename, char** args, uint8_t arg_count) {
    struct WorkingDirectory fs_current;
    kernel_get_working_directory(&fs_current);
    
    if (fs_current.mount_directory == FS_NULL) 
        return 1;
    
    uint32_t address = fs_directory_find(fs_current.mount_directory, filename);
    if (address == FS_NULL) 
        return 2;
    
    uint8_t attributes, persmissions;
    fs_file_get_attributes(address, &attributes);
    fs_file_get_permissions(address, &persmissions);
    
    if (!(persmissions & FS_PERMISSION_EXECUTE) || attributes & FS_ATTRIBUTE_DIRECTORY) 
        return 3;
    
    uint32_t file_size = fs_file_get_size(address);
    
    FileHandle file;
    if (fs_file_open(&file, address, FS_FILE_MODE_READ) == true) {
        uint32_t code_segment  = kmalloc(file_size);
        uint32_t stack_segment = kmalloc(1024);
        
        char buffer;
        for (uint32_t i=0; i < file_size; i++) {
            fs_file_read(&file, &buffer, 1);
            kmem_write(code_segment + i, &buffer, 1);
        }
        
        struct X4Thread thread_main;
        memset(&thread_main, 0x00, sizeof(struct X4Thread));
        
        thread_main.cache.cs = code_segment;
        thread_main.cache.ss = stack_segment;
        
        x4_emulate(&thread_main, args, arg_count, UINT32_MAX);
        
        kfree(code_segment);
        kfree(stack_segment);
        
        fs_file_close(&file);
        return 0;
    }
    return 4;
}

void scheduler_isr_callback(void) {
    isr_trigger = 1;
}
