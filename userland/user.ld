ENTRY(_start)

SECTIONS
{
    /* Classic Linux x86 32-bit ELF user space entry: 0x08048000 */
    . = 0x08048000;

    .text ALIGN(4K) : { *(.text) }
    .rodata ALIGN(4K) : { *(.rodata) }
    .data ALIGN(4K) : { *(.data) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss) }
}
