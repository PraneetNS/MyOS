ENTRY(_start)

SECTIONS
{
    /* 8MB -- comfortably clear of the kernel image + 2MB heap, which
       both sit well under 4MB, and still inside paging.c's 16MB
       identity-mapped, user-accessible region. */
    . = 0x800000;

    .text ALIGN(4K) : { *(.text) }
    .rodata ALIGN(4K) : { *(.rodata) }
    .data ALIGN(4K) : { *(.data) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss) }
}
