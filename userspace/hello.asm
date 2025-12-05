; hello.asm - Simple user program for uniOS
; Uses int 0x80 syscall interface

bits 64
section .text
global _start

_start:
    ; syscall: write(1, msg, 20)
    ; RAX = syscall number (1 = SYS_WRITE)
    ; RBX = string pointer
    ; RCX = length
    mov rax, 1              ; SYS_WRITE
    lea rbx, [rel msg]      ; pointer to message
    mov rcx, 20             ; length
    int 0x80                ; syscall
    
    ; syscall: exit(0)
    mov rax, 60             ; SYS_EXIT
    int 0x80
    
    ; Should never reach here
    jmp $

section .rodata
msg: db "Hello from ELF!", 0x0A, 0, 0, 0, 0
