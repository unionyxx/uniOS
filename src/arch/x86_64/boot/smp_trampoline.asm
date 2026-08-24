; smp_trampoline.asm — AP startup trampoline.
;
; The BSP copies this blob into one page of RAM below 1 MiB and starts APs
; with a SIPI whose vector is (trampoline_phys >> 12); the CPU enters here in
; 16-bit real mode with CS:IP = page_base:0.
;
; POSITION-INDEPENDENT BY CONSTRUCTION: the blob lives in the kernel image at
; a higher-half link address but executes from low RAM, so every memory
; reference is either an immediate layout constant or computed from RIP at
; runtime. Do not introduce [label] operands here.
;
; Fixed page layout — MUST match TrampParamOffsets in src/kernel/smp/smp.cpp:
;   0x000  16-bit entry code
;   0x080  64-bit continuation (long mode)
;   0x180  parameter block (u32 tpml4_phys, u32 pad, u64 stack, kcr3,
;          entry, percpu)
;   0x200  GDT (null, 64-bit code @0x08, data @0x10)
;   0x218  lgdt descriptor (limit + linear base, patched by BSP at copy time)

TR_OFF_TPML4    equ 0x180
TR_OFF_STACK    equ 0x188
TR_OFF_KCR3     equ 0x190
TR_OFF_ENTRY    equ 0x198
TR_OFF_PERCPU   equ 0x1A0
TR_OFF_GDT      equ 0x200
TR_OFF_GDT_DESC equ 0x218

global smp_trampoline_start
global smp_trampoline_end
global smp_trampoline_farjmp

section .rodata.smp_trampoline progbits alloc noexec nowrite align=4096

bits 16
smp_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ss, ax
    mov sp, 0x7000              ; scratch stack in conventional low RAM
    mov ax, cs
    mov ds, ax                  ; DS = CS (SIPI sets CS = page_base >> 4), so
    mov es, ax                  ; [si] resolves to page_base + layout offset

    mov si, TR_OFF_GDT_DESC
    o32 lgdt [ds:si]

    ; PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Trampoline page tables: identity low map + copied kernel upper half.
    mov si, TR_OFF_TPML4
    mov eax, [ds:si]
    mov cr3, eax

    ; EFER.LME | EFER.NXE (copied kernel PTEs may carry NX bits).
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)
    wrmsr

    ; PG + PE together: with PAE|LME set this lands directly in IA-32e mode.
    mov eax, cr0
    or eax, (1 << 31) | 1
    mov cr0, eax

    ; Far jump into 64-bit code. The immediate must be the LINEAR address of
    ; the continuation: long-mode segments have base 0, and once paging is on
    ; the fetch goes through the identity map. The BSP patches this dword to
    ; trampoline_phys + 0x80 at copy time.
    db 0x66                     ; jmp ptr16:32
    db 0xEA
smp_trampoline_farjmp:
    dd 0                        ; patched: trampoline_phys + LM_OFF
    dw 0x08

align 16
times (0x80 - ($ - $$)) db 0

bits 64
    ; Long-mode continuation, still on the trampoline CR3 (identity low map +
    ; copied kernel upper half): both this page and higher-half kernel text
    ; are addressable. Recover the runtime page base from RIP, load per-AP
    ; parameters, then jump INTO kernel .text before switching CR3 — the low
    ; page is unmapped under the kernel PML4, so execution must already sit
    ; at a higher-half address when CR3 changes.
    call .rip
.rip:
    pop rax                     ; runtime address of .rip (identity == phys here)
    sub rax, (.rip - smp_trampoline_start)

    mov rsp, [rax + TR_OFF_STACK]
    mov rcx, [rax + TR_OFF_KCR3]
    mov rdx, [rax + TR_OFF_ENTRY]
    mov rdi, [rax + TR_OFF_PERCPU]

    test rsp, rsp
    jz .hang
    test rdx, rdx
    jz .hang
    jmp rdx

.hang:
    cli
    hlt
    jmp .hang

times (0x180 - ($ - $$)) db 0

; Parameter block (written by the BSP via offsets — no storage needed here).
times (0x200 - ($ - $$)) db 0

align 16
; GDT: null, 64-bit code (selector 0x08), data (selector 0x10).
    dq 0
    dq 0x00AF9A000000FFFF
    dq 0x00CF92000000FFFF

; lgdt descriptor at page offset 0x218: limit word + 32-bit linear base that
; the BSP patches to trampoline_phys + TR_OFF_GDT at copy time.
dw 0x18 - 1                   ; limit: three entries
dd 0                          ; base — patched at copy time
dd 0

smp_trampoline_end:
