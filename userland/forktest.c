/* forktest.c -- proves SYS_FORK works with real Unix fork() semantics:
   ONE call to sys_fork() returns TWICE -- once in the parent (with the
   child's pid) and once in the child (with 0) -- because process_fork()
   duplicated this process's entire address space AND its exact CPU
   state at the moment of the call. Both copies resume at the exact
   same instruction, right after sys_fork() returns, with only the
   return value differing. */

#include "libc.h"

void _start(void) {
    const char* m1 = "[forktest] pid ";
    const char* m2 = ": about to fork()...\n";
    sys_write(1, m1, strlen(m1));
    print_uint((unsigned int) sys_getpid());
    sys_write(1, m2, strlen(m2));

    int result = sys_fork();

    if (result == 0) {
        /* This code runs in the CHILD -- a completely separate process,
           with its own copy of every page, that happens to have started
           life mid-way through this same function. */
        const char* mc1 = "[forktest] I am the CHILD, pid ";
        const char* mc2 = ". My parent saw my pid as its return value.\n";
        sys_write(1, mc1, strlen(mc1));
        print_uint((unsigned int) sys_getpid());
        sys_write(1, mc2, strlen(mc2));
    } else if (result > 0) {
        /* This code runs in the ORIGINAL (parent) process. */
        const char* mp1 = "[forktest] I am the PARENT, pid ";
        const char* mp2 = ". fork() gave me child pid ";
        const char* mp3 = ".\n";
        sys_write(1, mp1, strlen(mp1));
        print_uint((unsigned int) sys_getpid());
        sys_write(1, mp2, strlen(mp2));
        print_uint((unsigned int) result);
        sys_write(1, mp3, strlen(mp3));
    } else {
        const char* err = "[forktest] fork() failed!\n";
        sys_write(1, err, strlen(err));
    }

    const char* me1 = "[forktest] pid ";
    const char* me2 = " exiting.\n";
    sys_write(1, me1, strlen(me1));
    print_uint((unsigned int) sys_getpid());
    sys_write(1, me2, strlen(me2));
    sys_exit();

    for (;;) { }
}
