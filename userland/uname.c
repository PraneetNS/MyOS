#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char* argv[]) {
    struct utsname u;
    if (uname(&u) < 0) {
        perror("uname");
        return 1;
    }

    int opt_s = 0, opt_n = 0, opt_r = 0, opt_v = 0, opt_m = 0;

    if (argc == 1) {
        opt_s = 1;
    } else {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                for (int j = 1; argv[i][j]; j++) {
                    switch (argv[i][j]) {
                        case 'a':
                            opt_s = opt_n = opt_r = opt_v = opt_m = 1;
                            break;
                        case 's': opt_s = 1; break;
                        case 'n': opt_n = 1; break;
                        case 'r': opt_r = 1; break;
                        case 'v': opt_v = 1; break;
                        case 'm': opt_m = 1; break;
                        case 'h':
                            printf("Usage: uname [-asnrvm]\n");
                            return 0;
                        default:
                            fprintf(stderr, "uname: invalid option -- '%c'\n", argv[i][j]);
                            return 1;
                    }
                }
            } else {
                fprintf(stderr, "uname: unexpected argument -- '%s'\n", argv[i]);
                return 1;
            }
        }
    }

    if (!opt_s && !opt_n && !opt_r && !opt_v && !opt_m) {
        opt_s = 1;
    }

    int first = 1;
    if (opt_s) { if (!first) putchar(' '); printf("%s", u.sysname); first = 0; }
    if (opt_n) { if (!first) putchar(' '); printf("%s", u.nodename); first = 0; }
    if (opt_r) { if (!first) putchar(' '); printf("%s", u.release); first = 0; }
    if (opt_v) { if (!first) putchar(' '); printf("%s", u.version); first = 0; }
    if (opt_m) { if (!first) putchar(' '); printf("%s", u.machine); first = 0; }
    putchar('\n');

    return 0;
}
