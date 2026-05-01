bits 64

extern exception_handler

%macro ISR_NOERRCODE 1
    global isr%1
    isr%1:
        push 0              ; Push dummy error code
        push %1             ; Push interrupt number
        jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
    global isr%1
    isr%1:
        push %1             ; Push interrupt number
        jmp isr_common_stub
%endmacro
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Robust swapgs logic:
    ; 1. If CS was RPL 3, we definitely need swapgs.
    ; 2. If CS was RPL 0, we might still need swapgs if we were in syscall transition.
    test qword [rsp + 144], 3
    jnz .do_swap

    ; Paranoid check: is GS base a user address?
    ; We can check IA32_GS_BASE MSR (0xC0000101)
    mov ecx, 0xC0000101
    rdmsr
    test edx, edx
    js .no_swap ; If high bit of EDX (bit 63 of GS_BASE) is 1, it's kernel

.do_swap:
    swapgs
    push qword 1 ; Flag that we swapped
    jmp .handler
.no_swap:
    push qword 0 ; Flag that we didn't swap

.handler:
    mov rdi, rsp
    add rdi, 8   ; Adjust RDI to point to the saved registers (skip the flag)
    call exception_handler

    ; Restore GS if we swapped it
    pop rax
    cmp rax, 1
    jne .no_swap_back
    swapgs
.no_swap_back:

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

; Define ISRs
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

; IRQ handlers (hardware interrupts)
%macro IRQ 2
    global irq%1
    irq%1:
        push 0              ; Dummy error code
        push %2             ; Interrupt number (32+)
        jmp irq_common_stub
%endmacro

extern irq_handler

irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Robust swapgs logic
    test qword [rsp + 144], 3
    jnz .do_swap

    mov ecx, 0xC0000101
    rdmsr
    test edx, edx
    js .no_swap

.do_swap:
    swapgs
    push qword 1
    jmp .handler
.no_swap:
    push qword 0

.handler:
    mov rdi, rsp
    add rdi, 8
    call irq_handler

    ; Restore GS if we swapped it
    pop rax
    cmp rax, 1
    jne .no_swap_back
    swapgs
.no_swap_back:

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

; Define hardware interrupt stubs for vectors 32-255.
%assign i 0
%rep 224
IRQ i, (32 + i)
%assign i i + 1
%endrep

section .rodata
global isr_stub_table
isr_stub_table:
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

global irq_stub_table
irq_stub_table:
%assign i 0
%rep 224
    dq irq%+i
%assign i i + 1
%endrep

section .text

global load_idt
load_idt:
    lidt [rdi]
    ret

; Syscall handler (int 0x80)
extern syscall_handler

global isr128
isr128:
    test qword [rsp + 8], 3 ; Check if CS requested Ring 3
    jz .no_swap
    swapgs                  ; Only swap if coming from userspace
.no_swap:
    push r10 ; arg4
    push r8  ; arg5
    push r9  ; arg6
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov r10, rdi
    mov r11, rsi
    mov rdi, rax
    mov rsi, r10
    mov rcx, rdx
    mov rdx, r11
    mov r8, rsp

    call syscall_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    add rsp, 24 ; Skip arg6, 5, 4

    test qword [rsp + 8], 3
    jz .no_swap_back
    swapgs

    ; Clear volatile registers to prevent kernel state leakage
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d

.no_swap_back:
    iretq

extern scheduler_unlock_after_switch

global fork_ret
fork_ret:
    sub rsp, 8 ; Align for call
    call scheduler_unlock_after_switch
    add rsp, 8

    mov rax, 0      ; Child returns 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    add rsp, 24 ; Skip arg6, 5, 4 (newly added to SyscallFrame)

    ; Clear volatile registers to prevent kernel state leakage
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d

    swapgs          ; Switch to user GS
    iretq

