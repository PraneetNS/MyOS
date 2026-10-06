#include "libc.h"

int main(int argc, char** argv) {
    int s = 1;
    if (argc >= 2) {
        s = 0;
        for (int i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++) {
            s = s * 10 + (argv[1][i] - '0');
        }
    }
    sleep(s);
    exit(0);
    return 0;
}
