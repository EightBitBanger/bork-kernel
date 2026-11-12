#include <kernel/console/virtual_key.h>
#include <kernel/util/string.h>

volatile uint8_t* VirtualKeyMap = (void*)0;

static uint16_t keymap_size = 0;

void kb_map_init(uint8_t* map_ptr, uint16_t size) {
    memset(map_ptr, 0x00, size);
    VirtualKeyMap = map_ptr;
    keymap_size = size;
}

char kb_vkey_check(uint8_t key) {
    if (key < keymap_size) 
        return VirtualKeyMap[key];
    return 0;
}

char kb_vkey_set(uint8_t key, uint8_t state) {
    if (key < keymap_size) 
        VirtualKeyMap[key] = state;
    return state;
}
