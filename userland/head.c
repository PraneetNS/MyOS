/* head.c -- output the first part of files */

#include "libc.h"

/* strcmp from string.h */

int main(int argc, char** argv) {
    int max_lines = 10;
    const char* filename = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 < argc) {
                const char* s = argv[++i];
                max_lines = 0;
                for (int j = 0; s[j] >= '0' && s[j] <= '9'; j++) {
                    max_lines = max_lines * 10 + (s[j] - '0');
                }
            }
        } else if (argv[i][0] == '-' && argv[i][1] == 'n') {
            const char* s = &argv[i][2];
            max_lines = 0;
            for (int j = 0; s[j] >= '0' && s[j] <= '9'; j++) {
                max_lines = max_lines * 10 + (s[j] - '0');
            }
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }

    int fd = 0;
    if (filename) {
        fd = open(filename, O_RDONLY, 0);
        if (fd < 0) {
            write(2, "head: cannot open file\n", 23);
            exit(1);
        }
    }

    int lines_printed = 0;
    char c;
    while (lines_printed < max_lines && read(fd, &c, 1) == 1) {
        write(1, &c, 1);
        if (c == '\n') {
            lines_printed++;
        }
    }

    if (filename) {
        close(fd);
    }

    exit(0);
    return 0;
}
