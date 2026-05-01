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
}
