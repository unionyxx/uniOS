; filetest.asm - Test file syscalls
; Opens hello.txt, reads it, prints content

bits 64
section .text
global _start

_start:
    ; SYS_OPEN: open("hello.txt", 0, 0) -> fd in RAX
    mov rax, 2              ; SYS_OPEN
    lea rbx, [rel filename]
    int 0x80
    
    ; Check if open failed
    cmp rax, -1
    je .error
    
    ; Save fd
    mov r12, rax
    
    ; SYS_READ: read(fd, buffer, 64) -> bytes in RAX
    mov rax, 0              ; SYS_READ
    mov rbx, r12            ; fd
    lea rcx, [rel buffer]   ; buffer
    mov r8, 64              ; count
    int 0x80
    
    ; Save bytes read
    mov r13, rax
    
    ; SYS_WRITE: write(1, buffer, bytes_read)
    mov rax, 1              ; SYS_WRITE
    mov rbx, 1              ; stdout
    lea rcx, [rel buffer]   ; buffer
    mov r8, r13             ; bytes read
    int 0x80
    
    ; Print success message
    mov rax, 1
    mov rbx, 1
    lea rcx, [rel success]
    mov r8, 15
    int 0x80
    
    ; SYS_CLOSE: close(fd)
    mov rax, 3              ; SYS_CLOSE
    mov rbx, r12
    int 0x80
    
    ; SYS_EXIT
    mov rax, 60
    int 0x80

.error:
    ; Print error message
    mov rax, 1
    mov rbx, 1
    lea rcx, [rel errmsg]
    mov r8, 12
    int 0x80
    
    mov rax, 60
    int 0x80

section .data
filename: db "hello.txt", 0
success:  db "File read OK!", 0x0A, 0
errmsg:   db "Open failed", 0x0A

section .bss
buffer: resb 128
