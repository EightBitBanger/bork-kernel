#include <stdio.h>
#include <stdint.h>

#include <kernel/util/string.h>
#include <kernel/util/delay.h>
#include <kernel/kernel.h>

#include <kernel/emulation/x4/x4.h>
#include <kernel/fs/fs.h>

void x4_read_string_from_dx(struct X4Thread* thread, char* str);

struct X4Pointer x4_pointer_from_low16(uint8_t low, uint8_t high) {
    struct X4Pointer pointer;
    pointer.address = ((uint32_t)low) | ((uint32_t)high << 8);
    return pointer;
}

uint8_t isc_operating_system(struct X4Thread* thread, uint8_t* program, uint8_t size, char** args, uint8_t arg_count) {return 0;}

uint8_t isc_display_routine(struct X4Thread* thread, char** args, uint8_t arg_count) {
    if (thread->cache.ah == 0x09) {
        
        for (uint32_t i = 0;; i++) {
            uint8_t buffer;
            struct X4Pointer dataOffset = x4_pointer_from_low16(thread->cache.dl, thread->cache.dh);
            kmem_read(&buffer, thread->cache.cs + dataOffset.address + i, 1);
            
            if (buffer == '\0')
                break;
            
            if (buffer == 0x0D) {
                print("\n");
                continue;
            }
            
            char ch[] = " ";
            ch[0] = buffer;
            print(ch);
        }
        
        thread->cache.state |= X4EMM_FLAG_CONSOLE_DIRTY;
        return 0;
    }
    
    if (thread->cache.ah == 0x03) {
        display_clear();
        display_cursor_set_position(0);
        display_cursor_set_line(0);
        
        thread->cache.state &= ~X4EMM_FLAG_CONSOLE_DIRTY;
        return 0;
    }
    return 0;
}

uint8_t isc_filesystem_routine(struct X4Thread* thread, char** args, uint8_t arg_count) {
    uint32_t mount     = console_get_mounted_device();
    uint32_t directory = console_get_mounted_directory();
    if (mount == FS_NULL) 
        return 0;
    
    // File create
    if (thread->cache.ah == 0x3C) {
        struct X4Pointer file_size = x4_pointer_from_low16(thread->cache.cl, thread->cache.ch);
        
        char name[17];
        x4_read_string_from_dx(thread, name);
        
        struct FSPartitionBlock partition;
        fs_device_open(mount, &partition, FS_DEVICE_TYPE_EEPROM);
        
        uint32_t file_address = fs_file_create(name, FS_PERMISSION_READ | FS_PERMISSION_WRITE, file_size.address, directory);
        if (file_address == FS_NULL) {
            thread->cache.bl = 3;
            return 0;
        }
        
        fs_directory_add_reference(directory, file_address);
        
        fs_bitmap_flush();
        
        thread->cache.bl = 1;
        thread->cache.state &= ~X4EMM_FLAG_CONSOLE_DIRTY;
        
        return 0;
    }
    
    thread->cache.bl = 0;
    return 0;
}

uint8_t isc_network_routine(struct X4Thread* thread, char** args, uint8_t arg_count) {
    /*
    // AH = 0x0C Send a message
    if (reg[rAH] == 0x0C) {
        struct X4Pointer offset = x4_pointer_from_low16(reg[rDL], reg[rDH]);

        if (offset.address >= programSize || offset.address + reg[rCL] >= programSize) {
            reg[rBL] = 1;
            return 0;
        }

        switch (reg[rAL]) {
        case 0x00: ntSend(&programBuffer[offset.address], reg[rCL]); break;
        case 0x01: ntSend(param_string, param_length); break;
        }

        reg[rBL] = 0;
        return 0;
    }

    // AH = 0x0F Send a packet
    if (reg[rAH] == 0x0F) {
        struct X4Pointer offset = x4_pointer_from_low16(reg[rDL], reg[rDH]);

        if (offset.address >= programSize || offset.address + reg[rCL] >= programSize) {
            reg[rBL] = 1;
            return 0;
        }

        struct NetworkPacket packet;
        packet.start = NETWORK_PACKET_START_BYTE;
        packet.addr_d[0] = 0x00;
        packet.addr_d[1] = 0x00;
        packet.addr_s[0] = 0x00;
        packet.addr_s[1] = 0x00;
        memset(packet.data, 0x55, NETWORK_PACKET_DATA_SIZE);
        packet.index = 1;
        packet.total = 1;
        packet.stop = NETWORK_PACKET_STOP_BYTE;

        ntPacketSend(&packet);

        reg[rBL] = 0;
        return 0;
    }
    */
    thread->cache.bl = 0;
    return 0;
}

uint8_t last_character = 0x00;

uint8_t isc_keyboard_routine(struct X4Thread* thread, char** op_args, uint8_t arg_count) {
    if (thread->cache.ah == 0x00) {
        uint8_t current_character = kb_get_current_char();
        if (current_character != last_character) {
            last_character = current_character;
            thread->cache.al = current_character;
        } else {
            thread->cache.al = 0x00;
            return 0xD6; // Yield, still waiting
        }
        if (thread->cache.al != 0) {
            return 0;
        }
        if (kb_check_input_state() == 0) 
            
        return 0;
    }
    
    // Check if a key is currently in the buffer ready to be handled
    if (thread->cache.ah == 0x01) {
        uint8_t current_character = kb_get_current_char();
        if (current_character != last_character) {
            thread->cache.al = current_character;
        } else {
            thread->cache.al = 0x00;
        }
        return 0;
    }
    
    // Get the last character the keyboard sent in
    if (thread->cache.ah == 0x03) {
        uint8_t current_character = kb_get_current_char();
        if (current_character != last_character) {
            last_character = current_character;
            thread->cache.al = current_character;
        } else {
            thread->cache.al = 0x00;
        }
        return 0;
    }
    
    //Get shift state: AH=02h checks shift, ctrl, alt ect.. are pressed
    return 0;
}

