/* parent.c -- proves process spawning via fork + exec + waitpid:
   this process spawns hello.elf as a CHILD and blocks until it exits. */

#include "libc.h"

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    int mypid = sys_getpid();
    const char* m1 = "[parent] pid ";
    const char* m2 = " starting.\n";
    sys_write(1, m1, strlen(m1));
    print_uint((unsigned int) mypid);
    sys_write(1, m2, strlen(m2));

    const char* m3 = "[parent] spawning hello.elf as a child and WAITING for it...\n";
    sys_write(1, m3, strlen(m3));

    int child_pid = fork();
    if (child_pid == 0) {
        const char* argv_child[] = { "hello.elf", 0 };
        exec("hello.elf", argv_child);
        exit(1);
    }

    if (child_pid < 0) {
        const char* err = "[parent] spawn failed!\n";
        sys_write(1, err, strlen(err));
    } else {
        int status = 0;
        waitpid(child_pid, &status, 0);
        const char* m4 = "[parent] child (pid ";
        const char* m5 = ") finished -- back in parent now.\n";
        sys_write(1, m4, strlen(m4));
        print_uint((unsigned int) child_pid);
        sys_write(1, m5, strlen(m5));
    }

    const char* m6 = "[parent] exiting.\n";
    sys_write(1, m6, strlen(m6));
    exit(0);

    return 0;
}
