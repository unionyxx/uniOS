; hello.asm - Simple user program for uniOS
; Uses int 0x80 syscall interface
; Syscall convention: RAX=syscall, RDI=arg1, RSI=arg2, RDX=arg3
; (This matches x86-64 Linux syscall convention)

bits 64
section .text
global _start

_start:
    ; syscall: write(fd=1, msg, len=16)
    mov rax, 1              ; SYS_WRITE
    mov rdi, 1              ; fd = stdout
    lea rsi, [rel msg]      ; buffer pointer
    mov rdx, 16             ; length
    int 0x80

    ; syscall: exit(0)
    mov rax, 60             ; SYS_EXIT
    mov rdi, 0              ; exit status = 0 (success)
    int 0x80
    
    ; Should never reach here
    jmp $

section .rodata
msg: db "Hello from ELF!", 0x0A

