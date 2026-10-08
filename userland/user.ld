ENTRY(_start)

SECTIONS
{
    /* Classic Linux x86 32-bit ELF user space entry: 0x08048000 */
    . = 0x08048000;

    .text ALIGN(4K) : { *(.text .text.*) }
    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }
    .data ALIGN(4K) : { *(.data .data.*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss .bss.*) }

    . = ALIGN(4K);
    _end = .;
    PROVIDE(end = .);
}
