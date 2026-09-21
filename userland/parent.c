/* parent.c -- proves SYS_SPAWN_WAIT works: this process spawns
   hello.elf as a CHILD and blocks until it exits, unlike the shell's
   `run`, which is fire-and-forget. Contrast this with launching
   hello.elf directly from the shell (concurrent) -- here, "parent"
   prints its own pid, spawns the child, waits, and only then continues. */

#include "libc.h"

void _start(void) {
    int mypid = sys_getpid();
    sys_write("[parent] pid ");
    print_uint((unsigned int) mypid);
    sys_write(" starting.\n");

    sys_write("[parent] spawning hello.elf as a child and WAITING for it...\n");
    int child_pid = sys_spawn_wait("hello.elf");

    if (child_pid < 0) {
        sys_write("[parent] spawn failed!\n");
    } else {
        sys_write("[parent] child (pid ");
        print_uint((unsigned int) child_pid);
        sys_write(") finished -- back in parent now.\n");
    }

    sys_write("[parent] exiting.\n");
    sys_exit();

    for (;;) { }
}
