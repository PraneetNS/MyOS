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

    /* Ensure 16-byte stack alignment before calls:
       Current rsp has popped argc (8 bytes removed from initial 16-byte alignment).
       Pushing a dummy 8 bytes or aligning */
    andq $-16, %rsp
    subq $8, %rsp         /* align stack to 16 bytes before call */

    /* Save argc, argv, envp across initializers */
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
