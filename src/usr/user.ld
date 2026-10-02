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

    .bss : {
        *(.bss)
    }

    /* Static TLS template: the file image (.tdata) plus its zero tail
     * (.tbss). The kernel clones per-thread blocks from these VAs at
     * thread-create time, so they must stay pristine in the running image.
     * .tbss goes last because a NOBITS tail does not advance the file
     * layout - any later file-backed section would land on the same
     * addresses and corrupt the template (lld's orphan placement does
     * exactly that: .data ends up aliasing .tbss). PT_TLS is emitted from
     * these two output sections. */
    .tdata ALIGN(4) : {
        *(.tdata .tdata.*)
    }

    .tbss : {
        *(.tbss .tbss.*)
    }
}
