OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

KERNEL_LMA = 0x00100000;
KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    /* Physical load address for 32-bit Multiboot2 entry trampoline */
    . = KERNEL_LMA;

    .boot ALIGN(4K) : {
        *(.multiboot)
        *(.boot_trampoline)
        *(.boot_data)
        *(.boot_bss)
    }

    /* Transition to higher-half kernel (-2GB) */
    . = . + KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
        kernel_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
