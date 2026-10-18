global isr_mouse
extern mouse_handler_c

isr_mouse:
    pusha                  ; Save all general-purpose registers
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
    
    ; Carve out a 512-byte FXSAVE area and align it to 16 bytes. Aligning ESP
    ; itself also satisfies the i386 SysV ABI, which requires ESP % 16 == 0 at
    ; every CALL into C (GCC may use movaps on stack slots otherwise).
    mov ebp, esp           ; EBP was saved by pusha; callee-saved across the call
    sub esp, 512
    and esp, 0xFFFFFFF0
    
    fxsave [esp]           ; Save the entire FPU/MMX/SSE/XMM state
    
    call mouse_handler_c
    
    fxrstor [esp]          ; Restore the entire FPU/MMX/SSE/XMM state
    
    mov esp, ebp           ; Drop the FXSAVE area and alignment padding
    pop gs
    pop fs
    pop es
    pop ds
    popa                   ; Restore registers
    iret

