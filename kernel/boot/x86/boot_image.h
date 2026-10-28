#ifndef _KERNEL_BOOT_IMAGE_H_
#define _KERNEL_BOOT_IMAGE_H_

#include <stdint.h>

// The boot area this kernel was loaded from: sector 0 (MBR + stage 1),
// stage 2 and the kernel file, exactly as they sit at the front of the boot
// disk. Stage 2 hands it over as Multiboot module 0; kmain() moves it into
// memory the kernel reserves for it. "boot install" writes it to a new disk.

void boot_image_set(uint32_t address, uint32_t size);

// NULL when there is none (booted by another loader, or no RAM to keep it)
const uint8_t* boot_image_get(uint32_t* size);

// The asset archive (ustar) "boot install" / "boot repair" extract the
// system files from. Stage 2 reads it off the boot disk right behind the
// kernel file, so it lies inside the boot area above and travels with it
// to every disk "boot install" writes. Under GRUB it is a module of its own
// ("module /boot/assets.tar assets"). kmain() keeps it the same way as the
// boot area.

void boot_assets_set(uint32_t address, uint32_t size);

// NULL when there is none (no archive on the boot disk, or no RAM to keep it)
const uint8_t* boot_assets_get(uint32_t* size);

#endif
