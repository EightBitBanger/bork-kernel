#include <stddef.h>
#include <kernel/boot/x86/boot_image.h>

static uint32_t boot_image_address = 0;
static uint32_t boot_image_size    = 0;

static uint32_t boot_assets_address = 0;
static uint32_t boot_assets_size    = 0;

void boot_image_set(uint32_t address, uint32_t size) {
    boot_image_address = address;
    boot_image_size    = size;
}

const uint8_t* boot_image_get(uint32_t* size) {
    if (size != NULL)
        *size = boot_image_size;
    if (boot_image_address == 0 || boot_image_size == 0)
        return NULL;
    return (const uint8_t*)(uintptr_t)boot_image_address;
}

void boot_assets_set(uint32_t address, uint32_t size) {
    boot_assets_address = address;
    boot_assets_size    = size;
}

const uint8_t* boot_assets_get(uint32_t* size) {
    if (size != NULL)
        *size = boot_assets_size;
    if (boot_assets_address == 0 || boot_assets_size == 0)
        return NULL;
    return (const uint8_t*)(uintptr_t)boot_assets_address;
}
