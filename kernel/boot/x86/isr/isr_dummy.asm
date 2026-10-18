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
    push ds                ; User code may have left any selector here,
    push es                ; including null: reload the kernel's before C
    push fs
    push gs
    mov ax, 0x10           ; EAX was saved by pusha
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld                    ; Ring 3 can leave DF set; C needs it clear (iret restores it)
    
    ; 16-byte aligned FXSAVE area; ESP stays 16-byte aligned for the C call
    mov ebp, esp
    sub esp, 512
    and esp, 0xFFFFFFF0
    
    fxsave [esp]
    
    call %2
    
    fxrstor [esp]
    
    mov esp, ebp
    pop gs
    pop fs
    pop es
    pop ds
    popa                   ; Restore general-purpose registers
    iret                   ; Return to the interrupted code
%endmacro

DUMMY_STUB isr_dummy,        c_dummy_handler
DUMMY_STUB isr_dummy_master, c_dummy_master_handler
DUMMY_STUB isr_dummy_slave,  c_dummy_slave_handler

