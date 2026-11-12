/*
 * Linker script for loadable drivers (kernel/driver/kdrv.h)
 *
 * One image at address 0: code, read-only data, data, then bss last so it
 * need not be stored in the file. Link with --emit-relocs so mkkdrv can see
 * which words hold addresses; the loader adds the load address to them.
 */
ENTRY(kdrv_entry)

SECTIONS
{
    . = 0;

    .text   : { *(.text.kdrv_entry) *(.text .text.*) }
    .rodata : ALIGN(4) { *(.rodata .rodata.*) }
    .data   : ALIGN(4) { *(.data .data.*) }
    .bss    : ALIGN(4) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.interp) *(.dynamic) }
}
