/* grep.c -- print lines matching a pattern */

#include "libc.h"

static int strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return *a - *b;
}

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static const char* strstr_match(const char* haystack, const char* needle, int ignore_case) {
    if (!needle || !*needle) return haystack;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j]) {
            char hc = haystack[i + j];
            char nc = needle[j];
            if (ignore_case) {
                hc = to_lower(hc);
                nc = to_lower(nc);
            }
            if (hc != nc) break;
            j++;
        }
        if (!needle[j]) return &haystack[i];
    }
    return 0;
}

int main(int argc, char** argv) {
    int invert_match = 0;
    int line_number = 0;
    int ignore_case = 0;
    const char* pattern = 0;
    const char* filename = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            invert_match = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            line_number = 1;
        } else if (strcmp(argv[i], "-i") == 0) {
            ignore_case = 1;
        } else if (strcmp(argv[i], "-vn") == 0 || strcmp(argv[i], "-nv") == 0) {
            invert_match = 1;
            line_number = 1;
        } else if (!pattern) {
            pattern = argv[i];
        } else if (!filename) {
            filename = argv[i];
        }
    }

    if (!pattern) {
        write(2, "usage: grep [-v] [-n] pattern [file]\n", 37);
        exit(2);
    }

    int fd = 0;
    if (filename) {
        fd = open(filename, O_RDONLY, 0);
        if (fd < 0) {
            write(2, "grep: cannot open file\n", 23);
            exit(2);
        }
    }

    char line[1024];
    int len = 0;
    int current_line = 1;
    int match_found = 0;
    char c;

    while (read(fd, &c, 1) == 1) {
        if (c == '\n') {
            line[len] = '\0';
            int has_match = (strstr_match(line, pattern, ignore_case) != 0);
            if (invert_match) has_match = !has_match;

            if (has_match) {
                match_found = 1;
                if (line_number) {
                    print_uint(current_line);
                    write(1, ":", 1);
                }
                write(1, line, len);
                write(1, "\n", 1);
            }
            len = 0;
            current_line++;
        } else {
            if (len < (int)sizeof(line) - 1) {
                line[len++] = c;
            }
        }
    }

    if (len > 0) {
        line[len] = '\0';
        int has_match = (strstr_match(line, pattern, ignore_case) != 0);
        if (invert_match) has_match = !has_match;

        if (has_match) {
            match_found = 1;
            if (line_number) {
                print_uint(current_line);
                write(1, ":", 1);
            }
            write(1, line, len);
            write(1, "\n", 1);
        }
    }

    if (filename) {
        close(fd);
    }

    exit(match_found ? 0 : 1);
    return 0;
}
