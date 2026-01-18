global switch_to_task
global init_fpu_state

section .text

; void switch_to_task(Process* current, Process* next)
; RDI = current process pointer
; RSI = next process pointer
;
; Process struct offsets:
;   fpu_state:    0    (512 bytes, 16-byte aligned)
;   pid:          512
;   parent_pid:   520
;   name:         528  (32 bytes)
;   cpu_time:     560
;   sp:           568
;
switch_to_task:
    ; Save current context - general purpose registers
    pushfq              ; Save RFLAGS
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    
    ; Save RSP to current->sp (offset 568)
    mov [rdi + 568], rsp
    
    ; Save FPU/SSE state to current->fpu_state (offset 0)
    ; fxsave requires 16-byte aligned address
    fxsave [rdi]
    
    ; Load RSP from next->sp (offset 568)
    mov rsp, [rsi + 568]
    
    ; Restore FPU/SSE state from next->fpu_state (offset 0)
    fxrstor [rsi]
    
    ; Restore next context - general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq               ; Restore RFLAGS
    
    ret

; void init_fpu_state(uint8_t* fpu_buffer)
; RDI = pointer to 512-byte aligned buffer
; Initializes FPU state to default values
init_fpu_state:
    ; Reset FPU to default state
    fninit              ; Initialize x87 FPU
    
    ; Initialize SSE: set MXCSR to default (mask all exceptions)
    ; Use stack with proper 32-bit push
    sub rsp, 8          ; Allocate stack space (maintain 16-byte alignment)
    mov dword [rsp], 0x1F80  ; Default MXCSR value (mask all exceptions)
    ldmxcsr [rsp]
    add rsp, 8
    
    ; Save initialized state to the buffer
    fxsave [rdi]
    
    ret

section .data
    align 4
    default_mxcsr: dd 0x1F80  ; Default MXCSR: mask all exceptions
