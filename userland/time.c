#include "libc.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        const char* usage = "usage: time <command> [args...]\n";
        write(2, usage, strlen(usage));
        return 1;
    }

    unsigned int start_ticks = sys_ticks();

    int pid = fork();
    if (pid < 0) {
        const char* err = "time: fork failed\n";
        write(2, err, strlen(err));
        return 1;
    }

    if (pid == 0) {
        char bin_path[64];
        bin_path[0] = '/';
        bin_path[1] = 'b';
        bin_path[2] = 'i';
        bin_path[3] = 'n';
        bin_path[4] = '/';
        int p = 0;
        while (argv[1][p] && p < 50) {
            bin_path[5 + p] = argv[1][p];
            p++;
        }
        bin_path[5 + p] = '\0';

        execv(argv[1], &argv[1]);
        execv(bin_path, &argv[1]);

        /* Try with .elf suffix */
        char elf_path[64];
        p = 0;
        while (bin_path[p]) { elf_path[p] = bin_path[p]; p++; }
        elf_path[p++] = '.'; elf_path[p++] = 'e'; elf_path[p++] = 'l'; elf_path[p++] = 'f';
        elf_path[p] = '\0';
        execv(elf_path, &argv[1]);

        const char* err = "time: command not found\n";
        write(2, err, strlen(err));
        exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    unsigned int end_ticks = sys_ticks();
    unsigned int diff = (end_ticks >= start_ticks) ? (end_ticks - start_ticks) : 0;

    write(2, "\nreal\t", 6);
    print_uint(diff / 100);
    write(2, ".", 1);
    unsigned int frac = diff % 100;
    if (frac < 10) write(2, "0", 1);
    print_uint(frac);
    write(2, "s (", 3);
    print_uint(diff);
    write(2, " ticks)\n", 8);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}
