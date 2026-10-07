#include "libc.h"

int main(void) {
    int ufd = open("/proc/uptime", O_RDONLY, 0);
    if (ufd >= 0) {
        char buf[64];
        int n = read(ufd, buf, sizeof(buf) - 1);
        close(ufd);
        if (n > 0) {
            buf[n] = '\0';
            write(1, "top - uptime: ", 14);
            write(1, buf, strlen(buf));
        }
    }

    int mfd = open("/proc/meminfo", O_RDONLY, 0);
    if (mfd >= 0) {
        char buf[512];
        int n = read(mfd, buf, sizeof(buf) - 1);
        close(mfd);
        if (n > 0) {
            buf[n] = '\0';
            write(1, buf, strlen(buf));
        }
    }

    write(1, "\n", 1);
    char* ps_argv[] = { "ps.elf", 0 };
    int pid = fork();
    if (pid == 0) {
        execv("/bin/ps.elf", ps_argv);
        execv("/ps.elf", ps_argv);
        exit(1);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }

    return 0;
}
