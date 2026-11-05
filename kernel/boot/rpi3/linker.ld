/* boot/rpi3/linker.ld -- the firmware loads kernel8.img (a flat binary,
   objcopy -O binary of the ELF) at 0x80000 and jumps to its first byte */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text : {
        KEEP(*(.text.boot))     /* _start must be the first byte, like .multiboot */
        *(.text*)
    }

    .rodata : ALIGN(16) { *(.rodata*) }
    .data   : ALIGN(16) { *(.data*) }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(4096);
    _kernel_program_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
