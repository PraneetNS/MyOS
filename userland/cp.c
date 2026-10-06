#include "libc.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        const char* usage = "usage: cp <source> <destination>\n";
        write(1, usage, strlen(usage));
        return 1;
    }

    int src = open(argv[1], O_RDONLY, 0);
    if (src < 0) {
        const char* err = "cp: cannot open source file\n";
        write(1, err, strlen(err));
        return 1;
    }

    int dst = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst < 0) {
        close(src);
        const char* err = "cp: cannot open/create destination file\n";
        write(1, err, strlen(err));
        return 1;
    }

    char buf[512];
    int n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        write(dst, buf, (unsigned int) n);
    }

    close(src);
    close(dst);
    return 0;
}
