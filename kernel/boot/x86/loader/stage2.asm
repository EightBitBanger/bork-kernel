; =============================================================================
; stage2.asm - second stage loader (Silent on success)
; =============================================================================

[bits 16]
[org 0x8000]

%ifndef KERNEL_LBA
  %error "KERNEL_LBA must be defined"
%endif
%ifndef KERNEL_SECTORS
  %error "KERNEL_SECTORS must be defined"
%endif

;
; Low memory layout. Everything the kernel reads (the info structure, the
; memory map, the VBE blocks, the loader name) sits below 1 MB, which the
; kernel identity maps and its PMM never hands out.
;
MBI             equ 0x1000      ; struct MultibootInfo (116 bytes)
MMAP            equ 0x1100      ; Multiboot memory map, 24-byte entries
MMAP_MAX        equ 128         ; -> ends at 0x1D00
VBE_CTRL        equ 0x2000      ; VbeInfoBlock (512 bytes)
VBE_MODE        equ 0x2200      ; ModeInfoBlock (256 bytes)
;               0x7C00          ; Stack top (grows down)
;               0x8000          ; This file
BOUNCE_SEG      equ 0x2000      ; Disk bounce buffer: 0x20000 - 0x27FFF
BOUNCE_LINEAR   equ 0x20000
CHUNK_SECTORS   equ 64          ; 32 KB per read, never crosses a 64 KB boundary

STAGING         equ 0x01000000  ; Boot area (LBA 0 .. end of kernel) is staged here
KERNEL_IMAGE    equ STAGING + KERNEL_LBA * 512  ; The kernel file inside it
BOOT_AREA_BYTES equ (KERNEL_LBA + KERNEL_SECTORS) * 512
MODS            equ 0x1D00      ; One multiboot_module entry (16 bytes)
HEADER_SEARCH   equ 8192        ; Multiboot header must lie in the first 8 KB

; Multiboot info offsets (Multiboot 1 spec, section 3.3)
MBI_FLAGS       equ 0
MBI_MEM_LOWER   equ 4
MBI_MEM_UPPER   equ 8
MBI_BOOT_DEVICE equ 12
MBI_MMAP_LENGTH equ 44
MBI_MMAP_ADDR   equ 48
MBI_LOADER_NAME equ 64
MBI_VBE_CTRL    equ 72
MBI_VBE_MODE_IN equ 76
MBI_VBE_MODE    equ 80
MBI_FB_ADDR     equ 88
MBI_FB_PITCH    equ 96
MBI_FB_WIDTH    equ 100
MBI_FB_HEIGHT   equ 104
MBI_FB_BPP      equ 108
MBI_FB_TYPE     equ 109
MBI_FB_COLOR    equ 110         ; red pos, red size, green pos, green size, blue pos, blue size
MBI_SIZE        equ 116

MBI_MODS_COUNT  equ 20
MBI_MODS_ADDR   equ 24

MBI_INFO_FLAGS  equ (1 << 0) | (1 << 1) | (1 << 3) | (1 << 6) | (1 << 9) | (1 << 11) | (1 << 12)

; VBE ModeInfoBlock offsets
VMI_ATTRIBUTES  equ 0x00
VMI_PITCH       equ 0x10
VMI_WIDTH       equ 0x12
VMI_HEIGHT      equ 0x14
VMI_BPP         equ 0x19
VMI_MODEL       equ 0x1B
VMI_RED_SIZE    equ 0x1F        ; then red pos, green size, green pos, blue size, blue pos
VMI_PHYS_BASE   equ 0x28
VMI_LIN_PITCH   equ 0x32        ; VBE 3.0+
VMI_LIN_RED     equ 0x36        ; VBE 3.0+: same six bytes, for linear modes

; Mode attributes we need: supported (0), graphics (4), linear framebuffer (7)
VMI_ATTR_NEEDED equ (1 << 0) | (1 << 4) | (1 << 7)

stage2_entry:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti
    cld

    mov     [boot_drive], dl

    ; Clear the info structure and the memory map
    mov     di, MBI
    mov     cx, (MODS + 16 - MBI) / 2
    xor     ax, ax
    rep     stosw

    call    enable_a20
    call    detect_memory
    call    check_staging_fits
    call    load_kernel_file

    ; Place the kernel image (32-bit code, run through protected mode)
    mov     dword [pm_target], place_kernel32
    call    pm_call
    test    eax, eax
    jz      .placed
    shl     ax, 1
    mov     bx, ax
    mov     si, [place_errors + bx - 2]
    jmp     fatal
.placed:

    call    set_video_mode          ; No text output is visible after this

    ; Remaining info fields
    mov     dword [MBI + MBI_FLAGS], MBI_INFO_FLAGS
    movzx   eax, byte [boot_drive]
    shl     eax, 24
    or      eax, 0x00FFFFFF         ; Whole disk, no partition
    mov     [MBI + MBI_BOOT_DEVICE], eax
    mov     dword [MBI + MBI_LOADER_NAME], loader_name

    ; Module 0: the raw boot area (MBR, stage 2, kernel file) as read from
    ; this disk, so a running kernel can install itself onto another one
    mov     dword [MODS + 0], STAGING
    mov     dword [MODS + 4], STAGING + BOOT_AREA_BYTES
    mov     dword [MODS + 8], module_name
    mov     dword [MODS + 12], 0
    mov     dword [MBI + MBI_MODS_COUNT], 1
    mov     dword [MBI + MBI_MODS_ADDR], MODS

    jmp     enter_kernel

; -----------------------------------------------------------------------------
; Text output (BIOS teletype). SI = zero-terminated string.
; -----------------------------------------------------------------------------
print:
    pusha
.next:
    lodsb
    test    al, al
    jz      .done
    mov     ah, 0x0E
    mov     bx, 0x0007
    int     0x10
    jmp     .next
.done:
    popa
    ret

; AX -> four hex digits
print_hex16:
    pusha
    mov     cx, 4
.digit:
    rol     ax, 4
    push    ax
    and     al, 0x0F
    add     al, '0'
    cmp     al, '9'
    jbe     .out
    add     al, 'A' - '9' - 1
.out:
    mov     ah, 0x0E
    mov     bx, 0x0007
    int     0x10
    pop     ax
    loop    .digit
    popa
    ret

; SI = message. Never returns.
fatal:
    push    si
    mov     si, msg_error
    call    print
    pop     si
    call    print
    mov     si, msg_newline
    call    print
.halt:
    cli
    hlt
    jmp     .halt

; -----------------------------------------------------------------------------
; A20
; -----------------------------------------------------------------------------

check_a20:
    pusha
    push    ds
    push    es
    xor     ax, ax
    mov     ds, ax
    not     ax
    mov     es, ax
    mov     si, 0x0500
    mov     di, 0x0510
    mov     al, [ds:si]
    mov     ah, [es:di]
    push    ax
    mov     byte [ds:si], 0x00
    mov     byte [es:di], 0xFF
    cmp     byte [ds:si], 0xFF      ; Wrapped around -> A20 off
    pop     ax
    mov     [es:di], ah
    mov     [ds:si], al
    pop     es
    pop     ds
    popa
    je      .off
    clc
    ret
.off:
    stc
    ret

enable_a20:
    call    check_a20
    jnc     .done

    ; 1. BIOS
    mov     ax, 0x2401
    int     0x15
    call    check_a20
    jnc     .done

    ; 2. Fast A20 (system control port A). Never set bit 0: that resets.
    in      al, 0x92
    test    al, 2
    jnz     .kbc
    or      al, 2
    and     al, 0xFE
    out     0x92, al
    call    a20_settle
    jnc     .done

.kbc:
    ; 3. Keyboard controller output port
    call    kbc_wait_write
    mov     al, 0xAD                ; Disable keyboard
    out     0x64, al
    call    kbc_wait_write
    mov     al, 0xD0                ; Read output port
    out     0x64, al
    call    kbc_wait_read
    in      al, 0x60
    push    ax
    call    kbc_wait_write
    mov     al, 0xD1                ; Write output port
    out     0x64, al
    call    kbc_wait_write
    pop     ax
    or      al, 2                   ; A20 gate
    out     0x60, al
    call    kbc_wait_write
    mov     al, 0xAE                ; Enable keyboard
    out     0x64, al
    call    kbc_wait_write
    call    a20_settle
    jnc     .done

    mov     si, msg_a20
    jmp     .fail
.fail:
    jmp     fatal
.done:
    ret

a20_settle:
    mov     cx, 0xFFFF
.poll:
    call    check_a20
    jnc     .ok
    loop    .poll
    stc
.ok:
    ret

kbc_wait_write:
    push    cx
    mov     cx, 0xFFFF
.w:
    in      al, 0x64
    test    al, 2
    jz      .ok
    loop    .w
.ok:
    pop     cx
    ret

kbc_wait_read:
    push    cx
    mov     cx, 0xFFFF
.r:
    in      al, 0x64
    test    al, 1
    jnz     .ok
    loop    .r
.ok:
    pop     cx
    ret

; -----------------------------------------------------------------------------
; Memory detection -> mem_lower, mem_upper, mmap
; -----------------------------------------------------------------------------
detect_memory:
    int     0x12
    movzx   eax, ax
    mov     [MBI + MBI_MEM_LOWER], eax

    mov     word [mmap_count], 0
    xor     ebx, ebx
    mov     di, MMAP
.e820_next:
    push    di
    add     di, 4
    mov     eax, 0xE820
    mov     edx, 0x534D4150         ; 'SMAP'
    mov     ecx, 20
    int     0x15
    pop     di
    jc      .e820_done
    cmp     eax, 0x534D4150
    jne     .e820_done
    cmp     ecx, 20
    jb      .e820_skip
    mov     eax, [di + 12]
    or      eax, [di + 16]
    jz      .e820_skip

    mov     dword [di], 20
    add     di, 24
    inc     word [mmap_count]
    cmp     word [mmap_count], MMAP_MAX
    jae     .e820_done
.e820_skip:
    test    ebx, ebx
    jnz     .e820_next
.e820_done:
    cmp     word [mmap_count], 0
    je      .no_e820

    mov     cx, [mmap_count]
    mov     si, MMAP
.upper_scan:
    cmp     dword [si + 20], 1
    jne     .upper_next
    cmp     dword [si + 8], 0
    jne     .upper_next
    mov     eax, [si + 4]
    cmp     eax, 0x100000
    ja      .upper_next
    mov     edx, eax
    add     edx, [si + 12]
    jc      .upper_cap
    cmp     dword [si + 16], 0
    je      .upper_have
.upper_cap:
    mov     edx, 0xFFFFFFFF
.upper_have:
    cmp     edx, 0x100000
    jbe     .upper_next
    sub     edx, 0x100000
    shr     edx, 10
    mov     [MBI + MBI_MEM_UPPER], edx
    jmp     .mmap_publish
.upper_next:
    add     si, 24
    loop    .upper_scan

    call    upper_from_bios
    jmp     .mmap_publish

.no_e820:
    call    upper_from_bios
    mov     di, MMAP
    mov     dword [di + 0], 20
    mov     dword [di + 4], 0
    mov     dword [di + 8], 0
    mov     eax, [MBI + MBI_MEM_LOWER]
    shl     eax, 10
    mov     [di + 12], eax
    mov     dword [di + 16], 0
    mov     dword [di + 20], 1
    add     di, 24
    mov     dword [di + 0], 20
    mov     dword [di + 4], 0x100000
    mov     dword [di + 8], 0
    mov     eax, [MBI + MBI_MEM_UPPER]
    shl     eax, 10
    mov     [di + 12], eax
    mov     dword [di + 16], 0
    mov     dword [di + 20], 1
    mov     word [mmap_count], 2

.mmap_publish:
    movzx   eax, word [mmap_count]
    imul    eax, eax, 24
    mov     [MBI + MBI_MMAP_LENGTH], eax
    mov     dword [MBI + MBI_MMAP_ADDR], MMAP
    ret

upper_from_bios:
    xor     cx, cx
    xor     dx, dx
    mov     ax, 0xE801
    int     0x15
    jc      .e88
    test    cx, cx
    jnz     .have
    test    dx, dx
    jnz     .have
    mov     cx, ax
    mov     dx, bx
.have:
    movzx   eax, cx
    movzx   edx, dx
    shl     edx, 6
    add     eax, edx
    mov     [MBI + MBI_MEM_UPPER], eax
    ret
.e88:
    mov     ah, 0x88
    int     0x15
    jc      .none
    movzx   eax, ax
    mov     [MBI + MBI_MEM_UPPER], eax
    ret
.none:
    mov     si, msg_memory
    jmp     fatal

check_staging_fits:
    mov     eax, [MBI + MBI_MEM_UPPER]
    shl     eax, 10
    add     eax, 0x100000
    cmp     eax, STAGING + BOOT_AREA_BYTES
    jae     .ok
    mov     si, msg_low_memory
    jmp     fatal
.ok:
    ret

; -----------------------------------------------------------------------------
; Read the kernel file into the staging area
; -----------------------------------------------------------------------------
load_kernel_file:
    mov     dword [read_lba], 0
    mov     dword [read_left], KERNEL_LBA + KERNEL_SECTORS
    mov     dword [read_dest], STAGING

.chunk:
    mov     eax, [read_left]
    test    eax, eax
    jz      .done
    cmp     eax, CHUNK_SECTORS
    jbe     .size_ok
    mov     eax, CHUNK_SECTORS
.size_ok:
    mov     [read_count], ax

    mov     byte [read_retries], 3
.retry:
    mov     ax, [read_count]
    mov     [dap_count], ax
    mov     word [dap_offset], 0
    mov     word [dap_segment], BOUNCE_SEG
    mov     eax, [read_lba]
    mov     [chunk_lba], eax
    mov     [dap_lba], eax
    mov     dword [dap_lba + 4], 0
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jnc     .read_ok

    push    ax
    xor     ah, ah
    mov     dl, [boot_drive]
    int     0x13
    pop     ax
    dec     byte [read_retries]
    jnz     .retry

    mov     si, msg_disk
    call    print
    mov     al, ah
    xor     ah, ah
    call    print_hex16
    mov     si, msg_newline
    call    print
    mov     si, msg_disk_fatal
    jmp     fatal

.read_ok:
    cmp     dword [chunk_lba], KERNEL_LBA
    jne     .copy
    call    find_multiboot_header

.copy:
    mov     dword [pm_target], copy32
    mov     esi, BOUNCE_LINEAR
    mov     edi, [read_dest]
    movzx   ecx, word [read_count]
    shl     ecx, 9
    call    pm_call
    add     [read_dest], ecx

    movzx   eax, word [read_count]
    add     [read_lba], eax
    sub     [read_left], eax

    jmp     .chunk
.done:
    ret

find_multiboot_header:
    push    es
    mov     ax, BOUNCE_SEG
    mov     es, ax
    xor     di, di
.scan:
    cmp     dword [es:di], 0x1BADB002
    jne     .next
    mov     eax, [es:di]
    add     eax, [es:di + 4]
    add     eax, [es:di + 8]
    jz      .found
.next:
    add     di, 4
    cmp     di, HEADER_SEARCH
    jb      .scan
    pop     es
    mov     si, msg_no_header
    jmp     fatal

.found:
    movzx   eax, di
    mov     [mb_header_offset], eax
    mov     eax, [es:di + 4]

    test    eax, (1 << 16)
    jz      .no_aout
    mov     byte [use_aout], 1
.no_aout:
    test    eax, (1 << 2)
    jz      .done
    cmp     dword [es:di + 32], 0
    jne     .done
    mov     eax, [es:di + 36]
    test    eax, eax
    jz      .no_w
    mov     [req_width], ax
.no_w:
    mov     eax, [es:di + 40]
    test    eax, eax
    jz      .no_h
    mov     [req_height], ax
.no_h:
    mov     eax, [es:di + 44]
    test    eax, eax
    jz      .done
    mov     [req_bpp], al
.done:
    pop     es
    ret

; -----------------------------------------------------------------------------
; VBE: choose and set a linear framebuffer mode, fill the framebuffer fields
; -----------------------------------------------------------------------------
set_video_mode:
    mov     di, VBE_CTRL
    mov     dword [di], 'VBE2'
    mov     ax, 0x4F00
    int     0x10
    cmp     ax, 0x004F
    jne     .no_vbe
    cmp     dword [VBE_CTRL], 'VESA'
    jne     .no_vbe

    mov     ax, [VBE_CTRL + 0x04]
    mov     [vbe_version], ax
    mov     ax, [VBE_CTRL + 0x0E]
    mov     [mode_list_off], ax
    mov     ax, [VBE_CTRL + 0x10]
    mov     [mode_list_seg], ax

    mov     word [best_mode], 0xFFFF
    mov     dword [best_score], 0

.mode_next:
    push    fs
    mov     fs, [mode_list_seg]
    mov     si, [mode_list_off]
    mov     cx, [fs:si]
    pop     fs
    cmp     cx, 0xFFFF
    je      .mode_done
    add     word [mode_list_off], 2
    mov     [cur_mode], cx

    mov     ax, 0x4F01
    mov     di, VBE_MODE
    int     0x10
    cmp     ax, 0x004F
    jne     .mode_next

    mov     ax, [VBE_MODE + VMI_ATTRIBUTES]
    and     ax, VMI_ATTR_NEEDED
    cmp     ax, VMI_ATTR_NEEDED
    jne     .mode_next
    cmp     byte [VBE_MODE + VMI_MODEL], 6
    jne     .mode_next
    mov     al, [VBE_MODE + VMI_BPP]
    cmp     al, [req_bpp]
    jne     .mode_next
    cmp     dword [VBE_MODE + VMI_PHYS_BASE], 0
    je      .mode_next

    mov     ax, [VBE_MODE + VMI_WIDTH]
    mov     bx, [VBE_MODE + VMI_HEIGHT]
    cmp     ax, [req_width]
    jne     .not_exact
    cmp     bx, [req_height]
    jne     .not_exact
    mov     edx, 0xFFFFFFFF
    jmp     .score
.not_exact:
    cmp     ax, [req_width]
    ja      .too_big
    cmp     bx, [req_height]
    ja      .too_big
    movzx   eax, ax
    movzx   ebx, bx
    imul    eax, ebx
    mov     edx, eax
    add     edx, 1
    jmp     .score
.too_big:
    mov     edx, 1
.score:
    cmp     edx, [best_score]
    jbe     .mode_next
    mov     [best_score], edx
    mov     ax, [cur_mode]
    mov     [best_mode], ax
    jmp     .mode_next

.mode_done:
    cmp     word [best_mode], 0xFFFF
    jne     .have_mode
    mov     si, msg_no_mode
    jmp     fatal

.have_mode:
    mov     ax, 0x4F01
    mov     cx, [best_mode]
    mov     di, VBE_MODE
    int     0x10
    cmp     ax, 0x004F
    jne     .set_failed

    mov     ax, 0x4F02
    mov     bx, [best_mode]
    or      bx, 0x4000
    int     0x10
    cmp     ax, 0x004F
    jne     .set_failed

    mov     eax, [VBE_MODE + VMI_PHYS_BASE]
    mov     [MBI + MBI_FB_ADDR], eax
    mov     dword [MBI + MBI_FB_ADDR + 4], 0

    movzx   eax, word [VBE_MODE + VMI_PITCH]
    mov     si, VBE_MODE + VMI_RED_SIZE
    cmp     word [vbe_version], 0x0300
    jb      .old_vbe
    movzx   edx, word [VBE_MODE + VMI_LIN_PITCH]
    test    edx, edx
    jz      .old_vbe
    mov     eax, edx
    mov     si, VBE_MODE + VMI_LIN_RED
.old_vbe:
    mov     [MBI + MBI_FB_PITCH], eax

    movzx   eax, word [VBE_MODE + VMI_WIDTH]
    mov     [MBI + MBI_FB_WIDTH], eax
    movzx   eax, word [VBE_MODE + VMI_HEIGHT]
    mov     [MBI + MBI_FB_HEIGHT], eax
    mov     al, [VBE_MODE + VMI_BPP]
    mov     [MBI + MBI_FB_BPP], al
    mov     byte [MBI + MBI_FB_TYPE], 1

    mov     al, [si + 0]
    mov     [MBI + MBI_FB_COLOR + 1], al
    mov     al, [si + 1]
    mov     [MBI + MBI_FB_COLOR + 0], al
    mov     al, [si + 2]
    mov     [MBI + MBI_FB_COLOR + 3], al
    mov     al, [si + 3]
    mov     [MBI + MBI_FB_COLOR + 2], al
    mov     al, [si + 4]
    mov     [MBI + MBI_FB_COLOR + 5], al
    mov     al, [si + 5]
    mov     [MBI + MBI_FB_COLOR + 4], al

    mov     dword [MBI + MBI_VBE_CTRL], VBE_CTRL
    mov     dword [MBI + MBI_VBE_MODE_IN], VBE_MODE
    mov     ax, [best_mode]
    mov     [MBI + MBI_VBE_MODE], ax
    ret

.no_vbe:
    mov     si, msg_no_vbe
    jmp     fatal
.set_failed:
    mov     si, msg_set_mode
    jmp     fatal

; -----------------------------------------------------------------------------
; Protected mode
; -----------------------------------------------------------------------------
pm_call:
    pushad
    cli
    movzx   esp, sp
    lgdt    [gdt_descriptor]
    mov     eax, cr0
    or      al, 1
    mov     cr0, eax
    jmp     GDT_CODE32:.pm32

[bits 32]
.pm32:
    mov     ax, GDT_DATA32
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    cld
    call    [pm_target]
    mov     [pm_result], eax
    jmp     GDT_CODE16:.pm16

[bits 16]
.pm16:
    mov     ax, GDT_DATA16
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     eax, cr0
    and     al, 0xFE
    mov     cr0, eax
    jmp     0x0000:.real

.real:
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    popad
    sti
    mov     eax, [pm_result]
    ret

enter_kernel:
    cli
    lgdt    [gdt_descriptor]
    mov     eax, cr0
    or      al, 1
    mov     cr0, eax
    jmp     GDT_CODE32:.kernel32

[bits 32]
.kernel32:
    mov     ax, GDT_DATA32
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, 0x7C00
    cld

    mov     eax, 0x2BADB002
    mov     ebx, MBI
    jmp     [kernel_entry]

copy32:
    shr     ecx, 2
    rep     movsd
    ret

place_kernel32:
    cmp     byte [use_aout], 0
    jne     .aout

    mov     esi, KERNEL_IMAGE
    cmp     dword [esi], 0x464C457F
    jne     .not_elf
    cmp     word [esi + 4], 0x0101
    jne     .not_elf
    cmp     word [esi + 18], 3
    jne     .not_elf

    mov     ebx, [esi + 28]
    cmp     ebx, KERNEL_SECTORS * 512
    jae     .bad
    add     ebx, esi
    movzx   ecx, word [esi + 44]
    movzx   edx, word [esi + 42]
    test    ecx, ecx
    jz      .nothing
    xor     ebp, ebp

.segment:
    cmp     dword [ebx], 1
    jne     .segment_next
    mov     eax, [ebx + 20]
    test    eax, eax
    jz      .segment_next
    cmp     [ebx + 16], eax
    ja      .bad

    mov     edi, [ebx + 12]
    cmp     edi, 0x100000
    jb      .range
    add     eax, edi
    jc      .range
    cmp     eax, STAGING
    ja      .range

    mov     eax, [ebx + 4]
    add     eax, [ebx + 16]
    jc      .bad
    cmp     eax, KERNEL_SECTORS * 512
    ja      .bad

    push    ecx
    mov     esi, KERNEL_IMAGE
    add     esi, [ebx + 4]
    mov     ecx, [ebx + 16]
    rep     movsb
    mov     ecx, [ebx + 20]
    sub     ecx, [ebx + 16]
    xor     eax, eax
    rep     stosb
    pop     ecx
    inc     ebp

.segment_next:
    add     ebx, edx
    dec     ecx
    jnz     .segment

    test    ebp, ebp
    jz      .nothing
    mov     eax, [KERNEL_IMAGE + 24]
    mov     [kernel_entry], eax
    xor     eax, eax
    ret

.aout:
    mov     ebx, KERNEL_IMAGE
    add     ebx, [mb_header_offset]
    mov     eax, [ebx + 12]
    sub     eax, [ebx + 16]
    jb      .bad
    mov     esi, [mb_header_offset]
    sub     esi, eax
    jb      .bad

    mov     edi, [ebx + 16]
    cmp     edi, 0x100000
    jb      .range

    mov     ecx, [ebx + 20]
    test    ecx, ecx
    jz      .aout_to_eof
    sub     ecx, edi
    jb      .bad
    jmp     .aout_size
.aout_to_eof:
    mov     ecx, KERNEL_SECTORS * 512
    sub     ecx, esi
.aout_size:
    mov     eax, esi
    add     eax, ecx
    cmp     eax, KERNEL_SECTORS * 512
    ja      .bad

    mov     eax, edi
    add     eax, ecx
    cmp     dword [ebx + 24], 0
    je      .aout_end
    cmp     [ebx + 24], eax
    jb      .aout_end
    mov     eax, [ebx + 24]
.aout_end:
    cmp     eax, STAGING
    ja      .range

    add     esi, KERNEL_IMAGE
    rep     movsb
    mov     ecx, eax
    sub     ecx, edi
    xor     eax, eax
    rep     stosb

    mov     eax, [ebx + 28]
    mov     [kernel_entry], eax
    xor     eax, eax
    ret

.not_elf:
    mov     eax, 1
    ret
.bad:
    mov     eax, 2
    ret
.range:
    mov     eax, 3
    ret
.nothing:
    mov     eax, 4
    ret

[bits 16]

; -----------------------------------------------------------------------------
; Data
; -----------------------------------------------------------------------------

GDT_CODE32      equ 0x08
GDT_DATA32      equ 0x10
GDT_CODE16      equ 0x18
GDT_DATA16      equ 0x20

align 8
gdt:
    dq      0
    dq      0x00CF9A000000FFFF      ; 0x08: 32-bit code, base 0, 4 GB
    dq      0x00CF92000000FFFF      ; 0x10: 32-bit data, base 0, 4 GB
    dq      0x00009A000000FFFF      ; 0x18: 16-bit code, base 0, 64 KB
    dq      0x000092000000FFFF      ; 0x20: 16-bit data, base 0, 64 KB
gdt_end:

gdt_descriptor:
    dw      gdt_end - gdt - 1
    dd      gdt

align 4
dap:
    db      0x10, 0
dap_count:
    dw      0
dap_offset:
    dw      0
dap_segment:
    dw      0
dap_lba:
    dq      0

align 4
pm_target           dd 0
pm_result           dd 0
kernel_entry        dd 0
mb_header_offset    dd 0
best_score          dd 0
read_lba            dd 0
read_left           dd 0
chunk_lba           dd 0
read_dest           dd 0
read_count          dw 0
mmap_count          dw 0
vbe_version         dw 0
mode_list_off       dw 0
mode_list_seg       dw 0
cur_mode            dw 0
best_mode           dw 0
req_width           dw 1024         ; Defaults when the header names no mode
req_height          dw 768
req_bpp             db 32
use_aout            db 0
boot_drive          db 0
read_retries        db 0

place_errors:
    dw      msg_not_elf
    dw      msg_bad_image
    dw      msg_range
    dw      msg_no_segments

loader_name     db "stage2 boot loader", 0
module_name     db "bootarea", 0

msg_error       db 13, 10, "stage2 error: ", 0
msg_a20         db "could not enable the A20 line", 0
msg_memory      db "BIOS reports no memory map", 0
msg_low_memory  db "not enough memory to stage the kernel at 16 MB", 0
msg_disk        db 13, 10, "disk read failed, BIOS status 0x", 0
msg_disk_fatal  db "cannot read the kernel", 0
msg_no_header   db "no Multiboot header in the first 8 KB of the kernel", 0
msg_not_elf     db "kernel is not an i386 ELF32 executable", 0
msg_bad_image   db "kernel file is truncated or malformed", 0
msg_range       db "kernel segment outside 1 MB .. 16 MB", 0
msg_no_segments db "kernel has no loadable segments", 0
msg_no_vbe      db "no VESA BIOS extensions (VBE 2.0+)", 0
msg_no_mode     db "no linear framebuffer mode with the requested depth", 0
msg_set_mode    db "VBE refused to set the chosen mode", 0
msg_newline     db 13, 10, 0

stage2_end:

%if KERNEL_LBA % CHUNK_SECTORS != 0
  %error "KERNEL_LBA must be a multiple of CHUNK_SECTORS"
%endif

%if stage2_end - $$ > 62 * 512
  %error "stage 2 is larger than 62 sectors"
%endif