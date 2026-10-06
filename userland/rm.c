#include "libc.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        const char* usage = "usage: rm <file...>\n";
        write(1, usage, strlen(usage));
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        int ret = unlink(argv[i]);
        if (ret != 0) {
            const char* err1 = "rm: cannot remove '";
            const char* err2 = "': No such file or directory\n";
            write(1, err1, strlen(err1));
            write(1, argv[i], strlen(argv[i]));
            write(1, err2, strlen(err2));
            status = 1;
        }
    }
    return status;
}
