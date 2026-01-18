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
    
    ; Set up user data segment selectors (0x20 | 3 = 0x23)
    mov ax, 0x23        ; User data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    
    ; Note: GS is handled specially via swapgs
    ; We don't load GS here - swapgs will do it
    
    ; Build iretq stack frame (in reverse order):
    ; [SS]     - Stack segment selector
    ; [RSP]    - User stack pointer
    ; [RFLAGS] - Flags (with IF=1 for interrupts)
    ; [CS]     - Code segment selector
    ; [RIP]    - User entry point
    
    push 0x23           ; SS (user data selector: GDT index 4 | RPL 3)
    push rsi            ; RSP (user stack pointer)
    
    ; Push RFLAGS with IF set (bit 9 = interrupts enabled)
    ; Also set bit 1 which is always 1 in x86 RFLAGS
    pushfq
    pop rax
    or rax, 0x202       ; Set IF and reserved bit 1
    push rax            ; RFLAGS
    
    push 0x1B           ; CS (user code selector: GDT index 3 | RPL 3)
    push rdi            ; RIP (entry point)
    
    ; TODO: swapgs requires proper GS base initialization
    ; For now, skip it - user code doesn't need GS
    ; swapgs
    
    ; Transition to Ring 3
    iretq


; =============================================================================
; return_to_kernel()
; =============================================================================
; Called when a user process exits (via exception or explicit return)
; This is a placeholder - actual return is via syscall or exception
; =============================================================================
global return_to_kernel
return_to_kernel:
    ; This shouldn't be called directly
    ; User programs should use SYS_EXIT syscall
    cli
    hlt
    jmp return_to_kernel
