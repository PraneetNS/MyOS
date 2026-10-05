.section .text
.global _start
.type _start, @function

_start:
    xor %ebp, %ebp          # Clear frame pointer for stack traces

    # System V ABI i386 initial stack layout:
    # 0(%esp) = argc
    # 4(%esp) = argv[0] (address of argv array is %esp + 4)
    mov (%esp), %eax        # eax = argc
    lea 4(%esp), %edx       # edx = argv

    # Align stack to 16 bytes before calling main:
    and $-16, %esp
    sub $8, %esp
    push %edx               # arg 2: argv
    push %eax               # arg 1: argc
    call main

    # Exit with main's return code
    mov %eax, %ebx
    mov $0, %eax
    int $0x80

1:  hlt
    jmp 1b

.size _start, . - _start

.section .note.GNU-stack,"",@progbits
