/* sort.c -- sort lines of text files */

#include "libc.h"

#define MAX_SORT_LINES 512
#define LINE_CAPACITY 256

/* strcmp from string.h */

static char lines[MAX_SORT_LINES][LINE_CAPACITY];
static char* line_ptrs[MAX_SORT_LINES];

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
            write(2, "sort: cannot open file\n", 23);
            exit(1);
        }
    }

    int nlines = 0;
    int cur_len = 0;
    char c;

    while (nlines < MAX_SORT_LINES && read(fd, &c, 1) == 1) {
        if (c == '\n') {
            lines[nlines][cur_len] = '\0';
            line_ptrs[nlines] = lines[nlines];
            nlines++;
            cur_len = 0;
        } else {
            if (cur_len < LINE_CAPACITY - 1) {
                lines[nlines][cur_len++] = c;
            }
        }
    }

    if (cur_len > 0 && nlines < MAX_SORT_LINES) {
        lines[nlines][cur_len] = '\0';
        line_ptrs[nlines] = lines[nlines];
        nlines++;
    }

    if (filename) {
        close(fd);
    }

    /* Insertion sort */
    for (int i = 1; i < nlines; i++) {
        char* key = line_ptrs[i];
        int j = i - 1;
        while (j >= 0 && strcmp(line_ptrs[j], key) > 0) {
            line_ptrs[j + 1] = line_ptrs[j];
            j--;
        }
        line_ptrs[j + 1] = key;
    }

    for (int i = 0; i < nlines; i++) {
        write(1, line_ptrs[i], strlen(line_ptrs[i]));
        write(1, "\n", 1);
    }

    exit(0);
    return 0;
}
