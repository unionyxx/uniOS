; usermode.asm - Ring 3 (userspace) entry functions for uniOS
; Provides enter_user_mode for transitioning from kernel to user mode

bits 64
section .text

; =============================================================================
; enter_user_mode(uint64_t entry_point, uint64_t user_stack)
; =============================================================================
; Transitions from Ring 0 to Ring 3 by building an iretq stack frame
;
; Parameters:
;   RDI = user entry point (code address to jump to)
;   RSI = user stack pointer (top of user stack)
;
; This function never returns - control goes to user code
; =============================================================================
global enter_user_mode
enter_user_mode:
    ; Disable interrupts during transition
    cli

    ; Set up user data segment selectors (0x18 | 3 = 0x1B)
    ; New GDT order: 3=Data, 4=Code
    mov ax, 0x1B        ; User data selector
    mov ds, ax
    mov es, ax
    mov fs, ax

    ; Build iretq stack frame (in reverse order):
    push 0x1B           ; SS (user data selector: GDT index 3 | RPL 3)
    push rsi            ; RSP (user stack pointer)

    pushfq
    pop rax
    or rax, 0x202       ; Set IF and reserved bit 1
    push rax            ; RFLAGS

    push 0x23           ; CS (user code selector: GDT index 4 | RPL 3)
    push rdi            ; RIP (entry point)

    ; Move kernel GS base to KERNEL_GS_BASE MSR and clear active GS
    swapgs

    ; Clear volatile registers so we don't leak kernel state
    xor eax, eax
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d

    ; Transition to Ring 3
    iretq


; =============================================================================
; syscall_entry()
; =============================================================================
; Entry point for the 'syscall' instruction
;
; RCX = User RIP (saved by CPU)
; R11 = User RFLAGS (saved by CPU)
; RAX = Syscall number
; RDI, RSI, RDX, R10, R8, R9 = Arguments
; =============================================================================
extern syscall_handler
global syscall_entry

syscall_entry:
    ; 1. Swap to kernel GS base
    swapgs

    ; 2. Save user stack and switch to kernel stack
    ; [gs:0] is kernel_stack (TSS.RSP0 equivalent)
    ; [gs:8] is user_stack scratch area
    mov [gs:8], rsp
    mov rsp, [gs:0]

    ; 3. Build a frame on kernel stack matching SyscallFrame struct
    ; We push 14 values total (5 fixed + 3 extra args + 6 GP regs) = 112 bytes
    ; 112 is a multiple of 16, so alignment is maintained.

    push qword 0x1B     ; SS (User Data)
    push qword [gs:8]   ; User RSP
    push r11            ; RFLAGS (saved by CPU)
    push qword 0x23     ; CS (User Code)
    push rcx            ; User RIP (saved by CPU)

    push r10            ; Arg 4
    push r8             ; Arg 5
    push r9             ; Arg 6

    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; 4. Enable interrupts (they were masked by SFMASK)
    ; This allows timer/mouse IRQs to fire during long syscalls (like blit)
    sti

    ; 5. Call C++ handler with System V ABI arguments
    mov r8, rsp     ; 5th arg: SyscallFrame* (current RSP)
    mov rcx, rdx    ; 4th arg: a3 (from RDX in Syscall ABI)
    mov rdx, rsi    ; 3rd arg: a2 (from RSI)
    mov rsi, rdi    ; 2nd arg: a1 (from RDI)
    mov rdi, rax    ; 1st arg: syscall_num (from RAX)

    call syscall_handler

    ; RAX now contains the return value

    ; Signal check
    push rax            ; Save return value
    mov rdi, rsp
    add rdi, 8          ; RDI = SyscallFrame* (it's above the saved RAX)
    extern signal_check
    call signal_check
    pop rax             ; Restore return value

    ; 6. Disable interrupts before stack restore and sysret
    cli

    ; 7. Restore state
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    add rsp, 24 ; Skip r9, r8, r10 (arg6, arg5, arg4)

    pop rcx    ; Restore user RIP for sysret
    add rsp, 8 ; Skip CS
    pop r11    ; Restore user RFLAGS for sysret
    pop qword [gs:8] ; Pop user RSP directly into scratch storage, PRESERVING r12!
    add rsp, 8 ; Skip SS

    cli             ; Disable interrupts before switching to user RSP
    mov rsp, [gs:8]

    ; Clear volatile registers to prevent kernel state leakage
    ; RCX and R11 are already holding user RIP and RFLAGS for sysret
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d

    ; 7. Final swap back to user GS and return
    swapgs
    o64 sysret


; =============================================================================
; safe_copy_from_user(void *dest, const void *src, size_t n)
; =============================================================================
global safe_copy_from_user
global __user_copy_start
global __user_copy_end
global __user_copy_fixup

safe_copy_from_user:
    push rbp
    mov rbp, rsp

    test rdx, rdx
    jz safe_copy_from_user_success

    xor rcx, rcx
safe_copy_from_user_loop:
__user_copy_start:
    mov al, [rsi + rcx]
__user_copy_end:
    mov [rdi + rcx], al
    inc rcx
    cmp rcx, rdx
    jne safe_copy_from_user_loop

safe_copy_from_user_success:
    mov rax, 1
    pop rbp
    ret

__user_copy_fixup:
    mov rax, 0
    pop rbp
    ret


; =============================================================================
; safe_copy_to_user(void *dest, const void *src, size_t n)
; =============================================================================
global safe_copy_to_user
global __user_copy_to_start
global __user_copy_to_end
global __user_copy_to_fixup

safe_copy_to_user:
    push rbp
    mov rbp, rsp

    test rdx, rdx
    jz safe_copy_to_user_success

    xor rcx, rcx
safe_copy_to_user_loop:
    mov al, [rsi + rcx]
__user_copy_to_start:
    mov [rdi + rcx], al
__user_copy_to_end:
    inc rcx
    cmp rcx, rdx
    jne safe_copy_to_user_loop

safe_copy_to_user_success:
    mov rax, 1
    pop rbp
    ret

__user_copy_to_fixup:
    mov rax, 0
    pop rbp
    ret
