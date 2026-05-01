BITS 64
DEFAULT REL

global boot_jump_to_kernel

section .text
boot_jump_to_kernel:
    ; Windows x64 ABI:
    ;   rcx = kernel entrypoint
    ;   rdx = BootInfo* (higher-half virtual address)
    ;   r8  = stack top (higher-half virtual address)
    ;   r9  = new CR3 physical address
    cli
    cld
    mov rax, rcx
    mov rdi, rdx
    mov rsp, r8
    mov rcx, r9
    mov cr3, rcx
    and rsp, -16
    sub rsp, 8
    mov qword [rsp], 0
    xor rbp, rbp
    jmp rax
