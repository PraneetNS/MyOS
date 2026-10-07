#include "libc.h"

static int parse_int(const char* s) {
    int res = 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return sign * res;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        const char* usage = "usage: kill [-sig] <pid>\n";
        write(2, usage, strlen(usage));
        return 1;
    }

    int sig = 15; /* SIGTERM default */
    int pid_idx = 1;

    if (argv[1][0] == '-') {
        sig = parse_int(&argv[1][1]);
        pid_idx = 2;
    }

    if (pid_idx >= argc) {
        const char* err = "kill: missing pid\n";
        write(2, err, strlen(err));
        return 1;
    }

    int pid = parse_int(argv[pid_idx]);
    int ret = kill(pid, sig);
    if (ret < 0) {
        const char* err = "kill: failed to send signal\n";
        write(2, err, strlen(err));
        return 1;
    }

    return 0;
}
