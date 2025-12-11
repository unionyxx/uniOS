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
    ; Save CPU state
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

    mov rdi, rsp        ; Pass pointer to stack frame as argument
    call exception_handler

    ; Restore CPU state
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

    add rsp, 16         ; Clean up error code and interrupt number
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
    ; Save CPU state
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

    mov rdi, rsp        ; Pass pointer to stack frame as argument
    call irq_handler

    ; Restore CPU state
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

    add rsp, 16         ; Clean up error code and interrupt number
    iretq

; Define IRQs (IRQ0-15 -> vectors 32-47)
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

global isr_stub_table
isr_stub_table:
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

global irq_stub_table
irq_stub_table:
    dq irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    dq irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

global load_idt
load_idt:
    lidt [rdi]
    ret

; Syscall handler (int 0x80)
extern syscall_handler

global isr128
isr128:
    ; Save callee-saved registers
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    
    ; Syscall convention: RAX = syscall number, RDI/RSI/RDX = args
    mov rdi, rax    ; syscall_num
    mov rsi, rbx    ; arg1 (we use RBX for user-mode arg1)
    mov rdx, rcx    ; arg2
    mov rcx, r8     ; arg3
    
    call syscall_handler
    
    ; RAX already has return value
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    
    iretq
