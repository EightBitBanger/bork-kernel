ENTRY(_start)

SECTIONS
{
    . = 1M; /* Start at 1MB */
    
    .text : AT(ADDR(.text))
    {
        /* FORCE the multiboot header to be at the absolute beginning of the text section */
        KEEP(*(.multiboot))
        *(.text)
    }
    
    .rodata : { *(.rodata*) }
    .data   : { *(.data) }
    .bss    : { *(COMMON) *(.bss) }
    
    _kernel_program_end = .;
}
