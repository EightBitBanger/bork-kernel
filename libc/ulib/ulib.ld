/*
 * Link script for user libraries (os/ulib.h)
 *
 * One image at address 0: code, read-only data, data, then bss last so it
 * need not be stored in the file. Link with --emit-relocs so mkulib can see
 * which words hold addresses; ulib_open() adds the load address to them.
 * The same shape as kernel-main/drivers/sdk/kdrv.ld.
 */
ENTRY(ulib_entry)

SECTIONS
{
    . = 0;

    .text   : { *(.text.ulib_entry) *(.text .text.*) }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data   : ALIGN(16) { *(.data .data.*) }
    .bss    : ALIGN(16) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : {
        *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr)
        *(.interp .dynamic .dynsym .dynstr .hash .gnu.hash)
        *(.init_array .init_array.* .fini_array .fini_array.* .ctors .ctors.* .dtors .dtors.*)
        *(.os.kind)
    }
}
