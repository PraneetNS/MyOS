#include "libc.h"

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    char buf[128];
    char* p = getcwd(buf, sizeof(buf));
    if (p) {
        sys_write(1, p, strlen(p));
        sys_write(1, "\n", 1);
    } else {
        const char* err = "pwd: error getting current directory\n";
        sys_write(1, err, strlen(err));
    }
    return 0;
}
