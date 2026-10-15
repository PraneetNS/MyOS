#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void process_stream(FILE* fp, const char* sep, int width, int number_empty, long* line_no) {
    char buf[2048];
    while (fgets(buf, sizeof(buf), fp)) {
        int is_empty = (buf[0] == '\n' || (buf[0] == '\r' && buf[1] == '\n') || buf[0] == '\0');
        if (!is_empty || number_empty) {
            printf("%*ld%s%s", width, (*line_no)++, sep, buf);
        } else {
            printf("%*s%s%s", width, "", sep, buf);
        }
    }
}

int main(int argc, char* argv[]) {
    const char* sep = "\t";
    int width = 6;
    int number_empty = 0;
    int arg_idx = 1;

    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "-s") == 0 && arg_idx + 1 < argc) {
            sep = argv[++arg_idx];
        } else if (strcmp(argv[arg_idx], "-w") == 0 && arg_idx + 1 < argc) {
            width = atoi(argv[++arg_idx]);
            if (width < 1) width = 1;
        } else if (strcmp(argv[arg_idx], "-ba") == 0) {
            number_empty = 1;
        } else if (strcmp(argv[arg_idx], "-bt") == 0) {
            number_empty = 0;
        } else if (strcmp(argv[arg_idx], "--") == 0) {
            arg_idx++;
            break;
        } else {
            fprintf(stderr, "Usage: nl [-s SEP] [-w WIDTH] [-ba|-bt] [FILE ...]\n");
            return 1;
        }
        arg_idx++;
    }

    long line_no = 1;
    if (arg_idx >= argc) {
        process_stream(stdin, sep, width, number_empty, &line_no);
    } else {
        for (int i = arg_idx; i < argc; i++) {
            FILE* fp = fopen(argv[i], "r");
            if (!fp) {
                perror(argv[i]);
                continue;
            }
            process_stream(fp, sep, width, number_empty, &line_no);
            fclose(fp);
        }
    }

    return 0;
}
