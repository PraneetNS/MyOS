#include "libc.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        const char* usage = "usage: mv <source> <destination>\n";
        write(1, usage, strlen(usage));
        return 1;
    }

    int ret = rename(argv[1], argv[2]);
    if (ret != 0) {
        const char* err = "mv: cannot rename file\n";
        write(1, err, strlen(err));
        return 1;
    }
    return 0;
}
