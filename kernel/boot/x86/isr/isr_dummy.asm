; Default handlers for vectors nobody claimed.
;
;   isr_dummy         - non-IRQ vectors: no EOI (sending one could ack a real IRQ)
;   isr_dummy_master  - unhandled master PIC IRQs (0x20-0x27): EOI master
;   isr_dummy_slave   - unhandled slave PIC IRQs  (0x28-0x2F): EOI slave + master

extern c_dummy_handler
extern c_dummy_master_handler
extern c_dummy_slave_handler

global isr_dummy
global isr_dummy_master
global isr_dummy_slave

%macro DUMMY_STUB 2
%1:
    pusha                  ; Save standard general-purpose registers
    
    ; 16-byte aligned FXSAVE area; ESP stays 16-byte aligned for the C call
    mov ebp, esp
    sub esp, 512
    and esp, 0xFFFFFFF0
    
    fxsave [esp]
    
    call %2
    
    fxrstor [esp]
    
    mov esp, ebp
    popa                   ; Restore general-purpose registers
    iret                   ; Return to the interrupted code
%endmacro

DUMMY_STUB isr_dummy,        c_dummy_handler
DUMMY_STUB isr_dummy_master, c_dummy_master_handler
DUMMY_STUB isr_dummy_slave,  c_dummy_slave_handler
