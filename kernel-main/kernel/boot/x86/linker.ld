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
    
    /* Built-in drivers (DRIVER_BUILTIN_DECLARE, driver/driver.h) */
    .kdrivers : ALIGN(4)
    {
        __kdrivers_start = .;
        KEEP(*(.kdrivers))
        __kdrivers_end = .;
    }
    
    .data   : { *(.data) }
    .bss    : { *(COMMON) *(.bss) }
    
    _kernel_program_end = .;
}
