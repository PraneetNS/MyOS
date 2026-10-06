#include "libc.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        write(2, "usage: diff file1 file2\n", 24);
        exit(2);
        return 2;
    }

    int fd1 = open(argv[1], O_RDONLY, 0);
    if (fd1 < 0) {
        write(2, "diff: cannot open ", 18);
        write(2, argv[1], strlen(argv[1]));
        write(2, "\n", 1);
        exit(2);
        return 2;
    }

    int fd2 = open(argv[2], O_RDONLY, 0);
    if (fd2 < 0) {
        write(2, "diff: cannot open ", 18);
        write(2, argv[2], strlen(argv[2]));
        write(2, "\n", 1);
        close(fd1);
        exit(2);
        return 2;
    }

    char buf1[512];
    char buf2[512];
    int differ = 0;

    for (;;) {
        int n1 = read(fd1, buf1, sizeof(buf1));
        int n2 = read(fd2, buf2, sizeof(buf2));

        if (n1 < 0 || n2 < 0) {
            write(2, "diff: read error\n", 17);
            differ = 2;
            break;
        }

        if (n1 != n2) {
            differ = 1;
            break;
        }

        if (n1 == 0) {
            /* Both EOF and matched so far */
            break;
        }

        for (int i = 0; i < n1; i++) {
            if (buf1[i] != buf2[i]) {
                differ = 1;
                break;
            }
        }
        if (differ) break;
    }

    close(fd1);
    close(fd2);

    if (differ == 0) {
        write(1, "Files ", 6);
        write(1, argv[1], strlen(argv[1]));
        write(1, " and ", 5);
        write(1, argv[2], strlen(argv[2]));
        write(1, " match\n", 7);
        exit(0);
        return 0;
    } else if (differ == 1) {
        write(1, "Files ", 6);
        write(1, argv[1], strlen(argv[1]));
        write(1, " and ", 5);
        write(1, argv[2], strlen(argv[2]));
        write(1, " differ\n", 8);
        exit(1);
        return 1;
    }

    exit(2);
    return 2;
}
