.global _start
.extern main
.extern exit
.extern environ
.extern __libc_init_array
.extern __libmyos_init

.section .text
_start:
    /* Clear base pointer for outermost frame */
    xorq %rbp, %rbp

    /* In standard 64-bit calling/ELF conventions:
       Stack on entry:
         [rsp]     = argc
         [rsp + 8] = argv[0]
         ...
         [rsp + 8*(argc+1)] = NULL
         [rsp + 8*(argc+2)] = envp[0]
    */
    popq %rdi             /* rdi = argc */
    movq %rsp, %rsi       /* rsi = argv */

    /* Compute envp: argv + 8*(argc + 1) */
    movq %rdi, %rax
    incq %rax
    shlq $3, %rax
    addq %rsp, %rax       /* rax = &argv[argc + 1] = envp */
    movq %rax, %rdx       /* rdx = envp */

    /* Store envp into environ */
    movq %rax, environ(%rip)

    /* Align stack to 16 bytes */
    andq $-16, %rsp

    /* Pad 8 bytes so that (8 + 3*8 = 32 bytes) preserves 16-byte alignment */
    subq $8, %rsp
    pushq %rdx
    pushq %rsi
    pushq %rdi

    /* Run libmyos initialization: __libmyos_init(argc, argv, envp) */
    call __libmyos_init

    /* Call C++ / Newlib initializers if present */
    call __libc_init_array

    /* Restore args for main(argc, argv, envp) */
    popq %rdi             /* argc */
    popq %rsi             /* argv */
    popq %rdx             /* envp */
    addq $8, %rsp         /* pop padding: %rsp is 0 mod 16 before call main */

    call main

    /* Pass main's return value (eax/edi) to exit */
    movl %eax, %edi
    call exit

    /* If exit returns, halt */
1:  hlt
    jmp 1b

/* Default _init and _fini functions */
.global _init
_init:
    ret

.global _fini
_fini:
    ret

/* Signal return restorer stub for the 64-bit kernel sigreturn */
.global __sigreturn_restorer
__sigreturn_restorer:
    movq $119, %rax       /* SYS_SIGRETURN */
    syscall
    int $0x80
    hlt
