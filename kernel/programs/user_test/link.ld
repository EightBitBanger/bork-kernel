/*
 * Linker script for user programs loaded by CreateProcess() (scheduler/elf.c)
 *
 * The default i386 link address (0x08048000) is kernel space in this OS,
 * so programs are linked into the image area at 0x40000000. Each output
 * section starts on its own page, which keeps .text read-only to ring 3
 * (a page shared with .data would have to be writable).
 *
 * Build (static, non-PIE, no host libc):
 *
 *   i686-elf-gcc -ffreestanding -fno-pie -O2 -c crt0.S main.c
 *   i686-elf-gcc -nostdlib -static -no-pie -Wl,-z,max-page-size=4096 \
 *                -T link.ld -o hello crt0.o main.o -lgcc
 *
 * At _start, ESP is 16-byte aligned and nothing is on the stack (no argc,
 * argv or auxv yet). _start must never return: end with the exit syscall.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : ALIGN(4096) {
        *(.text.start)          /* put _start first if crt0 uses this section */
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4096) {
        *(.data .data.*)
    }

    .bss : ALIGN(4096) {
        *(COMMON)
        *(.bss .bss.*)
    }

    /* Nothing here is needed at run time; .tbss/.tdata would create a
       PT_TLS segment, which the loader refuses */
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.interp .dynamic .dynsym .dynstr .hash .gnu.hash)
    }
}
