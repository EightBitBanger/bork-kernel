section .multiboot
align 4
    ; FLAGS: 
    ; Bit 0 (0x01): Align all boot modules on i386 page boundaries
    ; Bit 1 (0x02): Must provide memory information 
    ; Bit 2 (0x04): Must request a video mode configuration
    %define MULTIBOOT_FLAGS 0x00000007 
    
    dd 0x1BADB002                               ; MAGIC
    dd MULTIBOOT_FLAGS                          ; FLAGS
    dd -(0x1BADB002 + MULTIBOOT_FLAGS)          ; CHECKSUM
    
    ; --- MULTIBOOT HEADER ADDRESS FIELDS ---
    ; Required layout placeholders when Bit 2 of FLAGS is set.
    dd 0                                        ; header_addr
    dd 0                                        ; load_addr
    dd 0                                        ; load_end_addr
    dd 0                                        ; bss_end_addr
    dd 0                                        ; entry_addr
    
    ; --- GRAPHIC FIELDS ---
    dd 00000000                                 ; Mode type: 0 = Linear Graphics Mode
    dd 1600                                     ; Requested Width (Highly compatible)
    dd 900                                      ; Requested Height (Highly compatible)
    dd 32                                       ; Requested Depth (Bits Per Pixel)
    
section .bss nobits
align 16
stack_bottom:
    resb 16384                                ; 16 KB
stack_top:
    
section .text
global _start
    
_start:
    cli

    ; GRUB leaves the Multiboot magic in EAX and the info pointer in EBX.
    ; CPUID below clobbers both, so park them in callee-saved registers.
    mov esi, eax                                ; Multiboot magic
    mov edi, ebx                                ; Multiboot info pointer

    ; Set up the stack
    mov esp, stack_top
    and esp, 0xFFFFFFF0

    ; -------------------------------------------------------------------------
    ; FLOATING POINT & SSE INITIALIZATION
    ; -------------------------------------------------------------------------
    mov eax, cr0
    and eax, ~(1 << 2)                          ; Clear EM (Bit 2)
    or eax, (1 << 1) | (1 << 5)                 ; Set MP (Bit 1) & NE (Bit 5)
    mov cr0, eax
    finit                                       ; Initialize FPU

    mov eax, 0x1
    cpuid

    ; OSFXSR may only be set when FXSAVE/FXRSTOR exist (CPUID.1:EDX bit 24).
    ; Every ISR stub and the scheduler use FXSAVE, so this is required.
    test edx, (1 << 24)
    jz .hang

    mov eax, cr4
    or eax, (1 << 9)                            ; OSFXSR
    test edx, (1 << 25)                         ; SSE present?
    jz .no_sse
    or eax, (1 << 10)                           ; OSXMMEXCPT
.no_sse:
    mov cr4, eax

    ; The i386 SysV ABI (which GCC assumes) requires ESP to be 16-byte
    ; aligned at the CALL instruction. Two 4-byte argument pushes would leave
    ; it 8 bytes off, so pad by 8 first: 8 + 4 + 4 = 16.
    sub esp, 8
    push edi                                    ; Argument 2: Multiboot info pointer
    push esi                                    ; Argument 1: Multiboot magic number

    extern kmain
    call kmain                                  ; Kernel entry point
    
.hang:
    hlt                
    jmp .hang
    
    
global gdt_flush
gdt_flush:
    mov eax, [esp + 4]  ; Get the pointer to the gdt_ptr passed from C
    lgdt [eax]          ; Load the new GDT structure
    
    ; Reload all data segment registers to point to the kernel data descriptor (0x10)
    mov ax, 0x10      
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Far jump to reload the Code Segment Register (CS) to the kernel code descriptor (0x08)
    jmp 0x08:.reload_cs
    
.reload_cs:
    ret
    
