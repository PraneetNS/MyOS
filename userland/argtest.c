/* argtest.c -- prints its argc and argv array. */
#include "libc.h"

int main(int argc, char** argv) {
    const char* header = "argtest running\nargc = ";
    sys_write(1, header, strlen(header));
    print_uint((unsigned int) argc);
    sys_write(1, "\n", 1);

    for (int i = 0; i < argc; i++) {
        const char* prefix = "argv[";
        sys_write(1, prefix, strlen(prefix));
        print_uint((unsigned int) i);
        const char* mid = "] = '";
        sys_write(1, mid, strlen(mid));
        if (argv && argv[i]) {
            sys_write(1, argv[i], strlen(argv[i]));
        } else {
            const char* null_str = "(null)";
            sys_write(1, null_str, strlen(null_str));
        }
        const char* suffix = "'\n";
        sys_write(1, suffix, strlen(suffix));
    }

    const char* done = "argtest done\n";
    sys_write(1, done, strlen(done));
    return 0;
}
