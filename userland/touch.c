#include "libc.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        const char* usage = "usage: touch <file...>\n";
        write(1, usage, strlen(usage));
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            const char* err1 = "touch: cannot touch '";
            const char* err2 = "'\n";
            write(1, err1, strlen(err1));
            write(1, argv[i], strlen(argv[i]));
            write(1, err2, strlen(err2));
        } else {
            close(fd);
        }
    }
    return 0;
}
