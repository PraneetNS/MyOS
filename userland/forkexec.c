/* forkexec.c -- the classic Unix process-creation idiom, all three
   pieces working together for real: fork() to create a new process,
   exec() to turn that copy into a different program, wait() for the
   parent to block until it's done. This is exactly how a real shell
   implements running a command. */

#include "libc.h"

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    const char* m1 = "[forkexec] pid ";
    const char* m2 = ": calling fork()...\n";
    sys_write(1, m1, strlen(m1));
    print_uint((unsigned int) sys_getpid());
    sys_write(1, m2, strlen(m2));

    int child = sys_fork();

    if (child == 0) {
        /* CHILD: still running a full copy of forkexec's own code at
           this point -- exec() is what turns it into hello.elf. */
        const char* mc1 = "[forkexec] child (pid ";
        const char* mc2 = "): about to exec(\"hello.elf\") -- my own code is\n";
        const char* mc3 = "[forkexec] child: about to be replaced entirely...\n";
        sys_write(1, mc1, strlen(mc1));
        print_uint((unsigned int) sys_getpid());
        sys_write(1, mc2, strlen(mc2));
        sys_write(1, mc3, strlen(mc3));

        sys_exec("hello.elf", 0); /* does not return on success */

        /* Only reached if exec() failed. */
        const char* err = "[forkexec] child: exec FAILED!\n";
        sys_write(1, err, strlen(err));
        sys_exit();
    } else if (child > 0) {
        /* PARENT: unchanged, still forkexec's own code. */
        const char* mp1 = "[forkexec] parent (pid ";
        const char* mp2 = "): waiting for child pid ";
        const char* mp3 = "...\n";
        sys_write(1, mp1, strlen(mp1));
        print_uint((unsigned int) sys_getpid());
        sys_write(1, mp2, strlen(mp2));
        print_uint((unsigned int) child);
        sys_write(1, mp3, strlen(mp3));

        waitpid(child, 0, 0);

        const char* mp4 = "[forkexec] parent: child finished. That output above\n";
        const char* mp5 = "[forkexec] parent: came from hello.elf, not forkexec --\n";
        const char* mp6 = "[forkexec] parent: the child really did become a different program.\n";
        sys_write(1, mp4, strlen(mp4));
        sys_write(1, mp5, strlen(mp5));
        sys_write(1, mp6, strlen(mp6));
    } else {
        const char* err = "[forkexec] fork() failed!\n";
        sys_write(1, err, strlen(err));
    }

    const char* me1 = "[forkexec] pid ";
    const char* me2 = " exiting.\n";
    sys_write(1, me1, strlen(me1));
    print_uint((unsigned int) sys_getpid());
    sys_write(1, me2, strlen(me2));
    sys_exit();

    for (;;) { }
}
