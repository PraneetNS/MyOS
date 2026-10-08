.global _start
.extern main
.extern exit
.extern environ
.extern __libc_init_array
.extern __libmyos_init

.section .text
_start:
    /* Clear base pointer for outermost frame */
    xorl %ebp, %ebp

    /* Pop argc */
    popl %esi             /* esi = argc */

    /* Current esp points to argv[0] */
    movl %esp, %edi       /* edi = argv */

    /* Compute envp: argv + argc + 1 */
    movl %esi, %eax
    incl %eax
    shll $2, %eax
    addl %esp, %eax       /* eax = &argv[argc + 1] = envp */

    /* Store envp into environ */
    movl %eax, environ

    /* Run libmyos initialization (e.g., timezone check) */
    pushl %eax            /* envp */
    pushl %edi            /* argv */
    pushl %esi            /* argc */
    call __libmyos_init
    addl $12, %esp

    /* Call C++ / Newlib initializers if present */
    call __libc_init_array

    /* Push args for main(argc, argv, envp) */
    pushl environ
    pushl %edi            /* argv */
    pushl %esi            /* argc */

    call main

    /* Pass main's return value to exit */
    pushl %eax
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

/* Signal return restorer stub for the kernel sigreturn */
.global __sigreturn_restorer
__sigreturn_restorer:
    popl %eax
    movl $119, %eax       /* SYS_SIGRETURN */
    int $0x80
    hlt
