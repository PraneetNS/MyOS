#include "libc.h"

static void cat_fd(int fd) {
    char buf[256];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (n == -4 /* -EINTR */) continue;
            break;
        }
        write(1, buf, (unsigned int) n);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        cat_fd(0);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            const char* err1 = "cat: ";
            const char* err2 = ": No such file or directory\n";
            write(1, err1, strlen(err1));
            write(1, argv[i], strlen(argv[i]));
            write(1, err2, strlen(err2));
            continue;
        }
        cat_fd(fd);
        close(fd);
    }
    return 0;
}
