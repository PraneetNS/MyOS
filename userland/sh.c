/* sh.c -- Userland Shell running in Ring 3.
   Prompts with cwd (sh:/dir$ ), reads commands, handles builtins (cd, exit, help),
   and executes external binaries via fork + exec + wait with /bin PATH lookup. */

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
        char cwd[64];
        if (!getcwd(cwd, sizeof(cwd))) {
            cwd[0] = '/';
            cwd[1] = '\0';
        }

        sys_write(1, "sh:", 3);
        sys_write(1, cwd, strlen(cwd));
        sys_write(1, "$ ", 2);

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
                "  cd <dir>      change directory\n"
                "  exit          exit the shell\n"
                "External binaries (loaded via fork + exec + wait from cwd or /bin):\n"
                "  ls, cat, echo, mkdir, rmdir, rm, cp, mv, touch, pwd, hello, argtest, etc.\n";
            sys_write(1, help_msg, strlen(help_msg));
        } else if (strcmp(cmd_args[0], "cd") == 0) {
            const char* target = (cmd_argc >= 2) ? cmd_args[1] : "/";
            if (chdir(target) != 0) {
                sys_write(1, "cd: ", 4);
                sys_write(1, target, strlen(target));
                sys_write(1, ": No such file or directory\n", 28);
            }
        } else if (strcmp(cmd_args[0], "exit") == 0) {
            const char* exit_msg = "Exiting shell.\n";
            sys_write(1, exit_msg, strlen(exit_msg));
            sys_exit();
        } else {
            /* External binary: fork + exec + wait */
            int pid = sys_fork();
            if (pid < 0) {
                const char* err = "sh: fork failed\n";
                sys_write(1, err, strlen(err));
            } else if (pid == 0) {
                /* Child: try exec directly */
                sys_exec(cmd_args[0], (const char* const*)cmd_args);

                /* Try appending .elf */
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

                /* Try in /bin/ */
                if (cmd_args[0][0] != '/') {
                    char bin_path[64];
                    bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
                    l = 0;
                    while (cmd_args[0][l] && l < 50) {
                        bin_path[5 + l] = cmd_args[0][l];
                        l++;
                    }
                    bin_path[5 + l] = '\0';
                    sys_exec(bin_path, (const char* const*)cmd_args);

                    /* Try /bin/<cmd>.elf */
                    bin_path[5 + l] = '.'; bin_path[5 + l + 1] = 'e';
                    bin_path[5 + l + 2] = 'l'; bin_path[5 + l + 3] = 'f';
                    bin_path[5 + l + 4] = '\0';
                    sys_exec(bin_path, (const char* const*)cmd_args);
                }

                const char* not_found = "sh: command not found: ";
                sys_write(1, not_found, strlen(not_found));
                sys_write(1, cmd_args[0], strlen(cmd_args[0]));
                sys_write(1, "\n", 1);
                sys_exit();
            } else {
                /* Parent: wait for child */
                int status = 0;
                waitpid(pid, &status, 0);
            }
        }
    }
    return 0;
}
