/* Linker script for user programs */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;  /* Load at 4MB */

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }

    .data : {
        *(.data)
    }

    /* Static TLS template: the file image (.tdata) plus its zero tail
     * (.tbss). PT_TLS is emitted from the pair, and the kernel clones
     * per-thread blocks from the template - but only the file-backed
     * .tdata prefix [p_vaddr, p_vaddr + p_filesz) is ever read back.
     * The .tbss tail VAs are never dereferenced, because ld/lld do NOT
     * advance the location counter past .tbss: the following .bss starts
     * at the same VA and overlaps it (the standard ELF TLS layout).
     *
     * .tbss must still sit before .bss. The NOBITS .bss tail is what
     * extends the RW PT_LOAD's p_memsz past the .tbss VAs; with .tbss
     * last (the old order) those VAs fell outside every PT_LOAD - a
     * .tdata ending exactly on a page boundary then left them unmapped
     * and thread creation failed with -EFAULT. Ending the image in
     * NOBITS sections also keeps file-backed orphans away: lld once
     * aliased .data over .tbss at the same VAs when allowed to place
     * the TLS sections itself. */
    .tdata ALIGN(4096) : {
        *(.tdata .tdata.*)
    }

    .tbss : {
        *(.tbss .tbss.*)
        __tbss_end = .;
    }

    .bss : {
        __bss_start = .;
        *(.bss)
    }

    /* Structural pin for the comment above: .bss does not follow .tbss
     * (it starts at the same VA), so the covered range ends at
     * __bss_start + SIZEOF(.bss) - the .tbss VAs must stay inside it. */
    ASSERT(__tbss_end <= __bss_start + SIZEOF(.bss),
           ".tbss VAs must stay inside the RW PT_LOAD's memsz")
}
