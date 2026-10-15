#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char** environ;

int main(int argc, char* argv[]) {
    int arg_idx = 1;
    int clear_env = 0;

    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "-i") == 0 || strcmp(argv[arg_idx], "-") == 0) {
            clear_env = 1;
        } else if (strcmp(argv[arg_idx], "-u") == 0 && arg_idx + 1 < argc) {
            unsetenv(argv[++arg_idx]);
        } else {
            fprintf(stderr, "Usage: env [-i] [-u NAME] [NAME=VALUE ...] [COMMAND [ARG ...]]\n");
            return 1;
        }
        arg_idx++;
    }

    if (clear_env) {
        if (environ) {
            environ[0] = NULL;
        }
    }

    /* Process NAME=VALUE definitions */
    while (arg_idx < argc && strchr(argv[arg_idx], '=')) {
        char* eq = strchr(argv[arg_idx], '=');
        *eq = '\0';
        setenv(argv[arg_idx], eq + 1, 1);
        *eq = '=';
        arg_idx++;
    }

    /* If no command specified, print environment */
    if (arg_idx >= argc) {
        if (environ) {
            for (char** ep = environ; *ep != NULL; ep++) {
                printf("%s\n", *ep);
            }
        }
        return 0;
    }

    /* Execute command */
    execvp(argv[arg_idx], &argv[arg_idx]);
    perror(argv[arg_idx]);
    return 127;
}
