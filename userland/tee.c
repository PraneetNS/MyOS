/* tee.c -- read from standard input and write to standard output and files */

#include "libc.h"

#define MAX_OUT_FILES 8

/* strcmp from string.h */

int main(int argc, char** argv) {
    int append_mode = 0;
    int out_fds[MAX_OUT_FILES];
    int num_files = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            append_mode = 1;
        } else if (argv[i][0] != '-') {
            if (num_files < MAX_OUT_FILES) {
                int flags = O_WRONLY | O_CREAT | (append_mode ? O_APPEND : O_TRUNC);
                int fd = open(argv[i], flags, 0);
                if (fd >= 0) {
                    out_fds[num_files++] = fd;
                } else {
                    write(2, "tee: cannot open file\n", 22);
                }
            }
        }
    }

    char buf[512];
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
        for (int i = 0; i < num_files; i++) {
            write(out_fds[i], buf, n);
        }
    }

    for (int i = 0; i < num_files; i++) {
        close(out_fds[i]);
    }

    exit(0);
    return 0;
}
