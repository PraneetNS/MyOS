ENTRY(_start)

SECTIONS
{
    /* Load the kernel at 1MB -- below this is reserved/BIOS territory,
       and it's the conventional spot GRUB expects for a Multiboot kernel. */
    . = 1M;

    .multiboot ALIGN(8) : {
        *(.multiboot)
    }

    .text ALIGN(4K) : {
        *(.text)
    }

    .rodata ALIGN(4K) : {
        *(.rodata)
    }

    .data ALIGN(4K) : {
        *(.data)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
    }
}
