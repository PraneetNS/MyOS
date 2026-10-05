/* sh.c -- Userland Shell running in Ring 3.
   Prompts, reads a line via SYS_READ on fd 0 (blocking), tokenizes arguments,
   handles builtins (cd, exit, help, cat), and otherwise fork + exec + wait. */

#include "libc.h"

static int strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return *a - *b;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int tokenize(char* line, char* argv[], int max_args) {
    int argc = 0;
    char* p = line;

    while (*p && argc < max_args - 1) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;

        argv[argc++] = p;
        while (*p && !is_space(*p)) p++;

        if (*p) {
            *p = '\0';
            p++;
        }
    }
    argv[argc] = 0;
    return argc;
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    const char* welcome = "\nMyOS Userland Shell (sh)\nType 'help' for builtins, or enter binary name to execute.\n";
    sys_write(1, welcome, strlen(welcome));

    char line[128];
    char* cmd_args[16];

    for (;;) {
        const char* prompt = "sh$ ";
        sys_write(1, prompt, strlen(prompt));

        int n = sys_read(0, line, sizeof(line) - 1);
        if (n <= 0) continue;
        line[n] = '\0';

        /* Strip trailing newlines */
        for (int i = 0; i < n; i++) {
            if (line[i] == '\r' || line[i] == '\n') {
                line[i] = '\0';
                break;
            }
        }

        int cmd_argc = tokenize(line, cmd_args, 16);
        if (cmd_argc == 0) continue;

        if (strcmp(cmd_args[0], "help") == 0) {
            const char* help_msg =
                "Builtins:\n"
                "  help          show this message\n"
                "  ls            list files on disk\n"
                "  cd <dir>      change directory (stub)\n"
                "  cat <file>    display file contents\n"
                "  exit          exit the shell\n"
                "External binaries (loaded via fork + exec + wait):\n"
                "  hello.elf, argtest.elf, reader.elf, forktest.elf, etc.\n";
            sys_write(1, help_msg, strlen(help_msg));
        } else if (strcmp(cmd_args[0], "ls") == 0) {
            const char* ls_msg =
                "hello.txt  hello.elf  badwrite.elf  reader.elf\n"
                "parent.elf  forktest.elf  producer.elf  consumer.elf\n"
                "forkexec.elf  heaptest.elf  argtest.elf  sh.elf\n";
            sys_write(1, ls_msg, strlen(ls_msg));
        } else if (strcmp(cmd_args[0], "cd") == 0) {
            const char* cd_msg = "cd: not supported on read-only filesystem\n";
            sys_write(1, cd_msg, strlen(cd_msg));
        } else if (strcmp(cmd_args[0], "exit") == 0) {
            const char* exit_msg = "Exiting shell.\n";
            sys_write(1, exit_msg, strlen(exit_msg));
            sys_exit();
        } else if (strcmp(cmd_args[0], "cat") == 0) {
            if (cmd_argc < 2) {
                const char* cat_usage = "usage: cat <file>\n";
                sys_write(1, cat_usage, strlen(cat_usage));
            } else {
                int fd = sys_open(cmd_args[1], O_RDONLY, 0);
                if (fd < 0) {
                    const char* cat_err = "cat: cannot open file\n";
                    sys_write(1, cat_err, strlen(cat_err));
                } else {
                    char buf[64];
                    int r;
                    while ((r = sys_read(fd, buf, sizeof(buf))) > 0) {
                        sys_write(1, buf, (unsigned int) r);
                    }
                    sys_close(fd);
                }
            }
        } else {
            /* External binary: fork + exec + wait */
            int pid = sys_fork();
            if (pid < 0) {
                const char* err = "sh: fork failed\n";
                sys_write(1, err, strlen(err));
            } else if (pid == 0) {
                /* Child: try exec directly */
                int ret = sys_exec(cmd_args[0], (const char* const*)cmd_args);
                if (ret < 0) {
                    /* If not found, try appending .elf */
                    char elf_name[64];
                    int l = 0;
                    while (cmd_args[0][l] && l < 58) {
                        elf_name[l] = cmd_args[0][l];
                        l++;
                    }
                    elf_name[l] = '.'; elf_name[l+1] = 'e';
                    elf_name[l+2] = 'l'; elf_name[l+3] = 'f';
                    elf_name[l+4] = '\0';
                    sys_exec(elf_name, (const char* const*)cmd_args);

                    const char* not_found = "sh: command not found: ";
                    sys_write(1, not_found, strlen(not_found));
                    sys_write(1, cmd_args[0], strlen(cmd_args[0]));
                    sys_write(1, "\n", 1);
                    sys_exit();
                }
            } else {
                /* Parent: wait for child */
                sys_wait(pid);
            }
        }
    }
    return 0;
}
