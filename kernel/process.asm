global switch_to_task

section .text

; void switch_to_task(Process* current, Process* next)
; RDI = current process pointer
; RSI = next process pointer
switch_to_task:
    ; Save current context
    pushfq              ; Save RFLAGS
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    
    ; Save RSP to current->sp (offset 8, since pid is uint64_t at offset 0)
    ; struct Process { uint64_t pid; uint64_t sp; ... }
    mov [rdi + 8], rsp
    
    ; Load RSP from next->sp
    mov rsp, [rsi + 8]
    
    ; Restore next context
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq               ; Restore RFLAGS
    
    ret
