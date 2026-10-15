#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char* argv[]) {
    long first = 1;
    long step = 1;
    long last = 1;
    const char* sep = "\n";

    int arg_idx = 1;
    while (arg_idx < argc && argv[arg_idx][0] == '-' && argv[arg_idx][1] != '\0' && (argv[arg_idx][1] < '0' || argv[arg_idx][1] > '9')) {
        if (strcmp(argv[arg_idx], "-s") == 0 && arg_idx + 1 < argc) {
            sep = argv[++arg_idx];
        } else {
            fprintf(stderr, "Usage: seq [-s separator] [FIRST [INCREMENT]] LAST\n");
            return 1;
        }
        arg_idx++;
    }

    int num_args = argc - arg_idx;
    if (num_args == 1) {
        last = atol(argv[arg_idx]);
    } else if (num_args == 2) {
        first = atol(argv[arg_idx]);
        last = atol(argv[arg_idx + 1]);
    } else if (num_args == 3) {
        first = atol(argv[arg_idx]);
        step = atol(argv[arg_idx + 1]);
        last = atol(argv[arg_idx + 2]);
    } else {
        fprintf(stderr, "Usage: seq [-s separator] [FIRST [INCREMENT]] LAST\n");
        return 1;
    }

    if (step == 0) {
        fprintf(stderr, "seq: zero increment\n");
        return 1;
    }

    int first_print = 1;
    if (step > 0) {
        for (long i = first; i <= last; i += step) {
            if (!first_print) printf("%s", sep);
            printf("%ld", i);
            first_print = 0;
        }
    } else {
        for (long i = first; i >= last; i += step) {
            if (!first_print) printf("%s", sep);
            printf("%ld", i);
            first_print = 0;
        }
    }
    if (!first_print) printf("\n");

    return 0;
}
