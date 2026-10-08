/* wc.c -- word, line, and byte count */

#include "libc.h"

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* strcmp from string.h */

int main(int argc, char** argv) {
    int count_lines = 0;
    int count_words = 0;
    int count_bytes = 0;
    int has_flags = 0;
    const char* filename = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            count_lines = 1;
            has_flags = 1;
        } else if (strcmp(argv[i], "-w") == 0) {
            count_words = 1;
            has_flags = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            count_bytes = 1;
            has_flags = 1;
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }

    if (!has_flags) {
        count_lines = 1;
        count_words = 1;
        count_bytes = 1;
    }

    int fd = 0;
    if (filename) {
        fd = open(filename, O_RDONLY, 0);
        if (fd < 0) {
            write(2, "wc: cannot open file\n", 21);
            exit(1);
        }
    }

    unsigned int total_lines = 0;
    unsigned int total_words = 0;
    unsigned int total_bytes = 0;
    int in_word = 0;

    char buf[512];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        total_bytes += n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') total_lines++;
            if (is_space(c)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                total_words++;
            }
        }
    }

    if (filename) {
        close(fd);
    }

    int printed = 0;
    if (count_lines) {
        print_uint(total_lines);
        printed++;
    }
    if (count_words) {
        if (printed) write(1, " ", 1);
        print_uint(total_words);
        printed++;
    }
    if (count_bytes) {
        if (printed) write(1, " ", 1);
        print_uint(total_bytes);
        printed++;
    }
    if (filename) {
        write(1, " ", 1);
        write(1, filename, strlen(filename));
    }
    write(1, "\n", 1);

    exit(0);
    return 0;
}
