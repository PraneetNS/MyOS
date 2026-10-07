ENTRY(_start)

SECTIONS
{
    . = 0xC0100000;

    .kernel ALIGN(4K) : AT(0x00100000) {
        *(.multiboot)
        *(.text)
        *(.rodata*)
        *(.data)
        *(COMMON)
        *(.bss)
        . = ALIGN(4K);
        kernel_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
