/* forktest.c -- proves SYS_FORK works with real Unix fork() semantics:
   ONE call to sys_fork() returns TWICE -- once in the parent (with the
   child's pid) and once in the child (with 0) -- because process_fork()
   duplicated this process's entire address space AND its exact CPU
   state at the moment of the call. Both copies resume at the exact
   same instruction, right after sys_fork() returns, with only the
   return value differing. */

#include "libc.h"

void _start(void) {
    sys_write("[forktest] pid ");
    print_uint((unsigned int) sys_getpid());
    sys_write(": about to fork()...\n");

    int result = sys_fork();

    if (result == 0) {
        /* This code runs in the CHILD -- a completely separate process,
           with its own copy of every page, that happens to have started
           life mid-way through this same function. */
        sys_write("[forktest] I am the CHILD, pid ");
        print_uint((unsigned int) sys_getpid());
        sys_write(". My parent saw my pid as its return value.\n");
    } else if (result > 0) {
        /* This code runs in the ORIGINAL (parent) process. */
        sys_write("[forktest] I am the PARENT, pid ");
        print_uint((unsigned int) sys_getpid());
        sys_write(". fork() gave me child pid ");
        print_uint((unsigned int) result);
        sys_write(".\n");
    } else {
        sys_write("[forktest] fork() failed!\n");
    }

    sys_write("[forktest] pid ");
    print_uint((unsigned int) sys_getpid());
    sys_write(" exiting.\n");
    sys_exit();

    for (;;) { }
}
