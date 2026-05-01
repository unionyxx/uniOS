global switch_to_task
global init_fpu_state
global save_fpu_state

extern g_use_xsave
extern g_xsave_mask_lo
extern g_xsave_mask_hi

section .text

; void switch_to_task(Process* current, Process* next)
; RDI = current process pointer
; RSI = next process pointer
;
; Process struct offsets (verified via static_assert):
;   uid:          0
;   _pad0:        4
;   _pad1:        8
;   fpu_state:    64   (1024 bytes, 64-byte aligned — compiler inserts 48B padding)
PROC_FPU_STATE equ 64
PROC_PID       equ 4160
PROC_PARENT_PID equ 4168
PROC_NAME      equ 4176
PROC_CPU_TIME  equ 4208
PROC_SP        equ 4216
;

extern kernel_task_wrapper
global kernel_thread_entry
kernel_thread_entry:
    ; switch_to_task popped entry_point into RBX
    mov rdi, rbx
    ; We arrive here via `ret`, not `call`, so RSP is already 16-byte aligned.
    ; Subtracting 8 misaligns the stack for the first C++ call and can crash
    ; before the task reaches its own first log line.
    call kernel_task_wrapper
    ; Should never return
    cli
    hlt
    jmp $

switch_to_task:
    ; Save current context - general purpose registers
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Save RSP to current->sp
    mov [rdi + PROC_SP], rsp

    ; Save FPU/SSE state to current->fpu_state
    cmp byte [rel g_use_xsave], 0
    je .save_fxsave
    mov eax, dword [rel g_xsave_mask_lo]
    mov edx, dword [rel g_xsave_mask_hi]
    xsave [rdi + PROC_FPU_STATE]
    jmp .save_done
.save_fxsave:
    fxsave64 [rdi + PROC_FPU_STATE]
.save_done:

    ; Load RSP from next->sp
    mov rsp, [rsi + PROC_SP]

    ; Restore FPU/SSE state from next->fpu_state
    cmp byte [rel g_use_xsave], 0
    je .restore_fxrstor
    mov eax, dword [rel g_xsave_mask_lo]
    mov edx, dword [rel g_xsave_mask_hi]
    xrstor [rsi + PROC_FPU_STATE]
    jmp .restore_done
.restore_fxrstor:
    fxrstor64 [rsi + PROC_FPU_STATE]
.restore_done:

    ; Restore next context - general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret

; void init_fpu_state(uint8_t* fpu_buffer)
; RDI = pointer to 64-byte aligned buffer
; Initializes FPU state to default values
init_fpu_state:
    ; Reset FPU to default state
    fninit              ; Initialize x87 FPU

    ; Initialize SSE: set MXCSR to default (mask all exceptions)
    sub rsp, 8
    mov dword [rsp], 0x1F80
    ldmxcsr [rsp]
    add rsp, 8

    ; Save initialized state to the buffer
    cmp byte [rel g_use_xsave], 0
    je .init_fxsave
    mov eax, dword [rel g_xsave_mask_lo]
    mov edx, dword [rel g_xsave_mask_hi]
    xsave [rdi]
    ret
.init_fxsave:
    fxsave64 [rdi]
    ret

; void save_fpu_state(uint8_t* fpu_buffer)
; RDI = pointer to 64-byte aligned buffer
save_fpu_state:
    cmp byte [rel g_use_xsave], 0
    je .sfpu_fxsave
    mov eax, dword [rel g_xsave_mask_lo]
    mov edx, dword [rel g_xsave_mask_hi]
    xsave [rdi]
    ret
.sfpu_fxsave:
    fxsave64 [rdi]
    ret
