/* uniq.c -- report or omit repeated lines */

#include "libc.h"

#define LINE_CAPACITY 512

static int strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return *a - *b;
}

int main(int argc, char** argv) {
    const char* filename = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            filename = argv[i];
            break;
        }
    }

    int fd = 0;
    if (filename) {
        fd = open(filename, O_RDONLY, 0);
        if (fd < 0) {
            write(2, "uniq: cannot open file\n", 23);
            exit(1);
        }
    }

    char cur_line[LINE_CAPACITY];
    char prev_line[LINE_CAPACITY];
    int cur_len = 0;
    int first_line = 1;
    char c;

    while (read(fd, &c, 1) == 1) {
        if (c == '\n') {
            cur_line[cur_len] = '\0';
            if (first_line || strcmp(cur_line, prev_line) != 0) {
                write(1, cur_line, cur_len);
                write(1, "\n", 1);
                for (int j = 0; j <= cur_len; j++) {
                    prev_line[j] = cur_line[j];
                }
                first_line = 0;
            }
            cur_len = 0;
        } else {
            if (cur_len < LINE_CAPACITY - 1) {
                cur_line[cur_len++] = c;
            }
        }
    }

    if (cur_len > 0) {
        cur_line[cur_len] = '\0';
        if (first_line || strcmp(cur_line, prev_line) != 0) {
            write(1, cur_line, cur_len);
            write(1, "\n", 1);
        }
    }

    if (filename) {
        close(fd);
    }

    exit(0);
    return 0;
}
