#include "libc.h"

static int strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return *a - *b;
}

int main(int argc, char** argv) {
    int long_mode = 0;
    const char* path = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            long_mode = 1;
        } else {
            path = argv[i];
        }
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        const char* err1 = "ls: cannot access '";
        const char* err2 = "': No such file or directory\n";
        write(2, err1, strlen(err1));
        write(2, path, strlen(path));
        write(2, err2, strlen(err2));
        return 1;
    }

    struct dirent de;
    int n;
    while ((n = getdents(fd, &de, sizeof(de))) > 0) {
        if (de.d_name[0] == '\0') continue;

        if (long_mode) {
            if (de.d_type == 2) {
                write(1, "d ", 2);
            } else {
                write(1, "- ", 2);
            }
            print_uint(de.d_size);
            write(1, "\t", 1);
            write(1, de.d_name, strlen(de.d_name));
            write(1, "\n", 1);
        } else {
            write(1, de.d_name, strlen(de.d_name));
            write(1, "\n", 1);
        }
    }

    close(fd);
    return 0;
}
