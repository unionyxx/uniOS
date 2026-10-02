; crt0.asm - C Runtime startup for uniOS userspace
bits 64
section .text

global _start
extern main
extern exit
extern __init_array_start
extern __init_array_end

_start:
    ; The kernel left the user stack pointer in RSP
    ; Ensure stack is 16-byte aligned for the System V ABI
    and rsp, -16

    ; Clear RBP to mark the top of the stack frame
    xor rbp, rbp

    ; Call global constructors
    mov rbx, __init_array_start
.loop:
    cmp rbx, __init_array_end
    je .done
    call [rbx]
    add rbx, 8
    jmp .loop
.done:

    ; Call the C main function
    ; Later we can pass argc/argv via RDI and RSI here
    call main

    ; Pass main's return value (in RAX) to exit()
    mov rdi, rax
    call exit

    ; Fallback in case exit fails
.hang:
    jmp .hang

global __sigret
__sigret:
    mov rax, 15 ; SYS_SIGRETURN
    syscall
    ret

; pthread_create enters the thread's fn with this address as the return
; address, so a plain return lands here with fn's return value in RAX.
; Route it into pthread_exit(RAX), which unmaps the stack and ends only
; this thread.
global __thread_exit_shim
extern pthread_exit
__thread_exit_shim:
    mov rdi, rax
    jmp pthread_exit
