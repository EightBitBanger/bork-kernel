global scheduler_yield_context
extern thread_handler_c
extern current_thread

%define TB_FPU_STATE 16            ; offsetof(ThreadBlock, fpu_state), checked in scheduler.c

align 4

; int $0x80: voluntary reschedule (thread_yield). Same frame layout and FPU
; handling as scheduler_context, without the timer tick or EOI.
scheduler_yield_context:
    pushad
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld                        ; Ring 3 can leave DF set; C needs it clear (iret restores it)
    
    mov eax, [current_thread]
    test eax, eax
    jz .no_save
    mov eax, [eax + TB_FPU_STATE]
    fxsave [eax]
.no_save:
    
    mov ebx, esp               ; Frame pointer
    and esp, 0xFFFFFFF0        ; ESP % 16 == 0 at the CALL
    
    sub esp, 12                ; 12 + 4 (argument) = 16
    push ebx                   ; Argument: address of the saved frame
    call thread_handler_c
    
    mov esp, eax               ; Switch to the chosen thread's saved frame
    
    mov eax, [current_thread]
    test eax, eax
    jz .no_restore
    mov eax, [eax + TB_FPU_STATE]
    fxrstor [eax]
.no_restore:
    
    pop gs
    pop fs
    pop es
    pop ds
    popad
    iret

