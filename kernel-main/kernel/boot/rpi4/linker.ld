/* boot/rpi4/linker.ld -- firmware loads kernel8.img (flat binary) at 0x80000 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text : {
        KEEP(*(.text.boot))     /* _start must be the first byte, like .multiboot */
        *(.text*)
    }

    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    _kernel_program_end = .;
}

/* Build: aarch64-none-elf-objcopy -O binary kernel.elf kernel8.img */
