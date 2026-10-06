#include "libc.h"

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    write(1, "[stress] Checking baseline free frames...\n", 42);
    unsigned int base_frames = sys_free_frames();
    write(1, "[stress] Baseline free frames: ", 31);
    print_uint(base_frames);
    write(1, "\n", 1);

    for (int i = 0; i < 50; i++) {
        int p[2];
        if (pipe(p) < 0) {
            write(2, "[stress] pipe() failed\n", 23);
            exit(1);
        }

        int pid = fork();
        if (pid < 0) {
            write(2, "[stress] fork() failed\n", 23);
            exit(1);
        }

        if (pid == 0) {
            /* Child */
            close(p[0]);
            dup2(p[1], 1);
            close(p[1]);
            const char* const args[] = { "true", 0 };
            exec("true.elf", args);
            exec("/bin/true.elf", args);
            exit(127);
        }

        /* Parent */
        close(p[1]);
        char buf[32];
        while (read(p[0], buf, sizeof(buf)) > 0) { }
        close(p[0]);

        int status = 0;
        int reaped = waitpid(pid, &status, 0);
        if (reaped != pid) {
            write(2, "[stress] waitpid failed\n", 24);
            exit(1);
        }
    }

    unsigned int final_frames = sys_free_frames();
    write(1, "[stress] Final free frames: ", 28);
    print_uint(final_frames);
    write(1, "\n", 1);

    if (final_frames == base_frames) {
        write(1, "[stress] ALL 50 ITERATIONS PASSED - NO LEAKS!\n", 47);
        exit(0);
        return 0;
    } else {
        write(2, "[stress] FRAME LEAK DETECTED!\n", 30);
        exit(1);
        return 1;
    }
}
