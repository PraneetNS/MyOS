ENTRY(_start)

SECTIONS
{
    /* Canonical standard 64-bit ELF user space entry: 0x0000000000400000 (4MB) */
    . = 0x0000000000400000;

    .text ALIGN(4K) : { *(.text .text.*) }
    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }
    .data ALIGN(4K) : { *(.data .data.*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss .bss.*) }

    . = ALIGN(4K);
    _end = .;
    PROVIDE(end = .);
}
