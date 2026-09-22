/* forkexec.c -- the classic Unix process-creation idiom, all three
   pieces working together for real: fork() to create a new process,
   exec() to turn that copy into a different program, wait() for the
   parent to block until it's done. This is exactly how a real shell
   implements running a command. */

#include "libc.h"

void _start(void) {
    sys_write("[forkexec] pid ");
    print_uint((unsigned int) sys_getpid());
    sys_write(": calling fork()...\n");

    int child = sys_fork();

    if (child == 0) {
        /* CHILD: still running a full copy of forkexec's own code at
           this point -- exec() is what turns it into hello.elf. */
        sys_write("[forkexec] child (pid ");
        print_uint((unsigned int) sys_getpid());
        sys_write("): about to exec(\"hello.elf\") -- my own code is\n");
        sys_write("[forkexec] child: about to be replaced entirely...\n");

        sys_exec("hello.elf"); /* does not return on success */

        /* Only reached if exec() failed. */
        sys_write("[forkexec] child: exec FAILED!\n");
        sys_exit();
    } else if (child > 0) {
        /* PARENT: unchanged, still forkexec's own code. */
        sys_write("[forkexec] parent (pid ");
        print_uint((unsigned int) sys_getpid());
        sys_write("): waiting for child pid ");
        print_uint((unsigned int) child);
        sys_write("...\n");

        sys_wait(child);

        sys_write("[forkexec] parent: child finished. That output above\n");
        sys_write("[forkexec] parent: came from hello.elf, not forkexec --\n");
        sys_write("[forkexec] parent: the child really did become a different program.\n");
    } else {
        sys_write("[forkexec] fork() failed!\n");
    }

    sys_write("[forkexec] pid ");
    print_uint((unsigned int) sys_getpid());
    sys_write(" exiting.\n");
    sys_exit();

    for (;;) { }
}
