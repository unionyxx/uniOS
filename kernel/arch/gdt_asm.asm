bits 64

global load_gdt

load_gdt:
    lgdt [rdi]
    
    push 0x08       ; Kernel code segment
    lea rax, [rel .reload_CS]
    push rax
    retfq

.reload_CS:
    mov ax, 0x10    ; Kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

global load_tss
load_tss:
    mov ax, 0x28    ; TSS is now at index 5 (5 * 8 = 0x28)
    ltr ax
    ret

; Jump to user mode
global jump_to_user_mode
; void jump_to_user_mode(uint64_t code, uint64_t stack, uint64_t entry)
; RDI = user code selector (0x1B)
; RSI = user stack pointer
; RDX = entry point
jump_to_user_mode:
    mov ax, 0x23       ; User data segment (0x20 | 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Build iretq frame
    push 0x23          ; SS (user data)
    push rsi           ; RSP (user stack)
    pushfq             ; RFLAGS
    or qword [rsp], 0x200 ; Enable interrupts
    push 0x1B          ; CS (user code = 0x18 | 3)
    push rdx           ; RIP (entry point)
    
    iretq
