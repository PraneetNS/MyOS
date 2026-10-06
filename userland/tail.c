/* tail.c -- output the last part of files */

#include "libc.h"

#define MAX_TAIL_LINES 128
#define LINE_CAPACITY 256

static int strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return *a - *b;
}

static char lines[MAX_TAIL_LINES][LINE_CAPACITY];

int main(int argc, char** argv) {
    int n_lines = 10;
    const char* filename = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 < argc) {
                const char* s = argv[++i];
                n_lines = 0;
                for (int j = 0; s[j] >= '0' && s[j] <= '9'; j++) {
                    n_lines = n_lines * 10 + (s[j] - '0');
                }
            }
        } else if (argv[i][0] == '-' && argv[i][1] == 'n') {
            const char* s = &argv[i][2];
            n_lines = 0;
            for (int j = 0; s[j] >= '0' && s[j] <= '9'; j++) {
                n_lines = n_lines * 10 + (s[j] - '0');
            }
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }

    if (n_lines > MAX_TAIL_LINES) n_lines = MAX_TAIL_LINES;

    int fd = 0;
    if (filename) {
        fd = open(filename, O_RDONLY, 0);
        if (fd < 0) {
            write(2, "tail: cannot open file\n", 23);
            exit(1);
        }
    }

    int total_lines = 0;
    int cur_len = 0;
    char c;

    while (read(fd, &c, 1) == 1) {
        int slot = total_lines % MAX_TAIL_LINES;
        if (cur_len < LINE_CAPACITY - 1) {
            lines[slot][cur_len++] = c;
        }
        if (c == '\n') {
            lines[slot][cur_len] = '\0';
            total_lines++;
            cur_len = 0;
        }
    }

    if (cur_len > 0) {
        int slot = total_lines % MAX_TAIL_LINES;
        lines[slot][cur_len] = '\0';
        total_lines++;
    }

    if (filename) {
        close(fd);
    }

    int start = (total_lines > n_lines) ? (total_lines - n_lines) : 0;
    for (int i = start; i < total_lines; i++) {
        int slot = i % MAX_TAIL_LINES;
        write(1, lines[slot], strlen(lines[slot]));
    }

    exit(0);
    return 0;
}
