/* sh.c -- Userland Shell running in Ring 3.
   Features:
   - Dynamic prompt reflecting cwd (sh:<cwd>$ )
   - Tokenizer with single quotes ('...'), double quotes ("..."), and backslash escapes (\)
   - Semicolon command sequencing (cmd1; cmd2)
   - $? exit status tracking and expansion (e.g. echo $?)
   - Builtins: cd, exit, help, status
   - Redirections: < file, > file, >> file, 2> file, 2>&1
   - Pipelines: a | b | c (N stages)
   - Background jobs: cmd & (non-blocking, reaped via WNOHANG on next prompt)
   - PATH lookup (/bin/)
*/

#include "libc.h"

#define MAX_STAGES 8
#define MAX_ARGS 16

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

static void int_to_str(int n, char* buf) {
    if (n == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[12];
    int idx = 0;
    int is_neg = 0;
    if (n < 0) {
        is_neg = 1;
        n = -n;
    }
    while (n > 0) {
        tmp[idx++] = '0' + (n % 10);
        n /= 10;
    }
    int out = 0;
    if (is_neg) buf[out++] = '-';
    while (idx > 0) {
        buf[out++] = tmp[--idx];
    }
    buf[out] = '\0';
}

static int tokenize_cmd(const char* str, char* tokens[], int max_tokens, char* buf, int buf_size, int last_status) {
    int ntokens = 0;
    int bidx = 0;
    int i = 0;

    char status_str[12];
    int_to_str(last_status, status_str);

    while (str[i] && ntokens < max_tokens - 1) {
        while (str[i] && is_space(str[i])) i++;
        if (!str[i]) break;

        /* Multi-character operators outside quotes */
        if (str[i] == '2' && str[i+1] == '>' && str[i+2] == '&' && str[i+3] == '1') {
            tokens[ntokens++] = "2>&1";
            i += 4;
            continue;
        }
        if (str[i] == '2' && str[i+1] == '>') {
            tokens[ntokens++] = "2>";
            i += 2;
            continue;
        }
        if (str[i] == '>' && str[i+1] == '>') {
            tokens[ntokens++] = ">>";
            i += 2;
            continue;
        }
        if (str[i] == '>' || str[i] == '<' || str[i] == '|' || str[i] == '&') {
            if (bidx + 2 < buf_size) {
                tokens[ntokens++] = &buf[bidx];
                buf[bidx++] = str[i++];
                buf[bidx++] = '\0';
            }
            continue;
        }

        /* Regular argument / word */
        tokens[ntokens++] = &buf[bidx];
        int in_sq = 0;
        int in_dq = 0;

        while (str[i]) {
            if (!in_sq && !in_dq) {
                if (is_space(str[i])) break;
                if (str[i] == '>' || str[i] == '<' || str[i] == '|' || str[i] == '&') break;
                if (str[i] == '2' && str[i+1] == '>') break;
            }

            if (str[i] == '\\' && !in_sq) {
                i++;
                if (str[i] && bidx + 1 < buf_size) {
                    buf[bidx++] = str[i++];
                }
                continue;
            }

            if (str[i] == '\'' && !in_dq) {
                in_sq = !in_sq;
                i++;
                continue;
            }

            if (str[i] == '"' && !in_sq) {
                in_dq = !in_dq;
                i++;
                continue;
            }

            /* $? expansion */
            if (str[i] == '$' && str[i+1] == '?' && !in_sq) {
                i += 2;
                int k = 0;
                while (status_str[k] && bidx + 1 < buf_size) {
                    buf[bidx++] = status_str[k++];
                }
                continue;
            }

            if (bidx + 1 < buf_size) {
                buf[bidx++] = str[i++];
            } else {
                i++;
            }
        }

        if (bidx < buf_size) {
            buf[bidx++] = '\0';
        }
    }

    tokens[ntokens] = 0;
    return ntokens;
}

typedef struct {
    char* argv[MAX_ARGS];
    int argc;
    char* stdin_file;
    char* stdout_file;
    int stdout_append;
    char* stderr_file;
    int stderr_append;
    int stderr_to_stdout;
} command_t;

static int parse_command(char* tokens[], int ntokens, command_t* cmd) {
    cmd->argc = 0;
    cmd->stdin_file = 0;
    cmd->stdout_file = 0;
    cmd->stdout_append = 0;
    cmd->stderr_file = 0;
    cmd->stderr_append = 0;
    cmd->stderr_to_stdout = 0;

    for (int i = 0; i < ntokens; i++) {
        if (strcmp(tokens[i], "<") == 0) {
            if (i + 1 < ntokens) {
                cmd->stdin_file = tokens[++i];
            } else {
                write(2, "sh: syntax error near unexpected token '<'\n", 43);
                return -1;
            }
        } else if (strcmp(tokens[i], ">") == 0) {
            if (i + 1 < ntokens) {
                cmd->stdout_file = tokens[++i];
                cmd->stdout_append = 0;
            } else {
                write(2, "sh: syntax error near unexpected token '>'\n", 43);
                return -1;
            }
        } else if (strcmp(tokens[i], ">>") == 0) {
            if (i + 1 < ntokens) {
                cmd->stdout_file = tokens[++i];
                cmd->stdout_append = 1;
            } else {
                write(2, "sh: syntax error near unexpected token '>>'\n", 44);
                return -1;
            }
        } else if (strcmp(tokens[i], "2>") == 0) {
            if (i + 1 < ntokens) {
                cmd->stderr_file = tokens[++i];
                cmd->stderr_append = 0;
            } else {
                write(2, "sh: syntax error near unexpected token '2>'\n", 44);
                return -1;
            }
        } else if (strcmp(tokens[i], "2>&1") == 0) {
            cmd->stderr_to_stdout = 1;
        } else {
            if (cmd->argc < MAX_ARGS - 1) {
                cmd->argv[cmd->argc++] = tokens[i];
            }
        }
    }
    cmd->argv[cmd->argc] = 0;
    return 0;
}

static void apply_redirections(const command_t* cmd) {
    if (cmd->stdin_file) {
        int fd = open(cmd->stdin_file, O_RDONLY, 0);
        if (fd < 0) {
            write(2, "sh: cannot open ", 16);
            write(2, cmd->stdin_file, strlen(cmd->stdin_file));
            write(2, "\n", 1);
            exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (cmd->stdout_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->stdout_append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->stdout_file, flags, 0);
        if (fd < 0) {
            write(2, "sh: cannot open ", 16);
            write(2, cmd->stdout_file, strlen(cmd->stdout_file));
            write(2, "\n", 1);
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    if (cmd->stderr_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->stderr_append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->stderr_file, flags, 0);
        if (fd < 0) {
            write(2, "sh: cannot open ", 16);
            write(2, cmd->stderr_file, strlen(cmd->stderr_file));
            write(2, "\n", 1);
            exit(1);
        }
        dup2(fd, 2);
        close(fd);
    }
    if (cmd->stderr_to_stdout) {
        dup2(1, 2);
    }
}

static void exec_command(const command_t* cmd) {
    apply_redirections(cmd);

    /* 1. Try directly as provided */
    exec(cmd->argv[0], (const char* const*) cmd->argv);

    /* 2. Try with .elf appended */
    char elf_name[64];
    int l = 0;
    while (cmd->argv[0][l] && l < 58) {
        elf_name[l] = cmd->argv[0][l];
        l++;
    }
    elf_name[l] = '.'; elf_name[l+1] = 'e';
    elf_name[l+2] = 'l'; elf_name[l+3] = 'f';
    elf_name[l+4] = '\0';
    exec(elf_name, (const char* const*) cmd->argv);

    /* 3. Try under /bin/ */
    if (cmd->argv[0][0] != '/') {
        char bin_path[64];
        bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
        l = 0;
        while (cmd->argv[0][l] && l < 50) {
            bin_path[5 + l] = cmd->argv[0][l];
            l++;
        }
        bin_path[5 + l] = '\0';
        exec(bin_path, (const char* const*) cmd->argv);

        bin_path[5 + l] = '.'; bin_path[5 + l + 1] = 'e';
        bin_path[5 + l + 2] = 'l'; bin_path[5 + l + 3] = 'f';
        bin_path[5 + l + 4] = '\0';
        exec(bin_path, (const char* const*) cmd->argv);
    }

    write(2, "sh: command not found: ", 23);
    write(2, cmd->argv[0], strlen(cmd->argv[0]));
    write(2, "\n", 1);
    exit(127);
}

#define MAX_JOBS 16

typedef enum {
    JOB_EMPTY = 0,
    JOB_RUNNING,
    JOB_STOPPED
} job_status_t;

typedef struct {
    int jid;
    int pgid;
    job_status_t status;
    char cmd[64];
} job_t;

static int is_interactive = 0;
static job_t job_table[MAX_JOBS];

static int job_add(int pgid, job_status_t status, const char* cmd) {
    int free_idx = -1;
    int max_jid = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].status != JOB_EMPTY) {
            if (job_table[i].jid > max_jid) max_jid = job_table[i].jid;
        } else if (free_idx == -1) {
            free_idx = i;
        }
    }
    if (free_idx == -1) return -1;
    job_table[free_idx].jid = max_jid + 1;
    job_table[free_idx].pgid = pgid;
    job_table[free_idx].status = status;
    int c = 0;
    while (cmd[c] && c < 63) {
        job_table[free_idx].cmd[c] = cmd[c];
        c++;
    }
    job_table[free_idx].cmd[c] = '\0';
    return job_table[free_idx].jid;
}

static job_t* job_find_by_jid(int jid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].status != JOB_EMPTY && job_table[i].jid == jid) {
            return &job_table[i];
        }
    }
    return 0;
}

static job_t* job_find_by_pgid(int pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].status != JOB_EMPTY && job_table[i].pgid == pgid) {
            return &job_table[i];
        }
    }
    return 0;
}

static void job_remove(int pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].status != JOB_EMPTY && job_table[i].pgid == pgid) {
            job_table[i].status = JOB_EMPTY;
            break;
        }
    }
}

static int atoi_custom(const char* s) {
    int res = 0;
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res;
}

static void reap_background_jobs(void) {
    int bg_status = 0;
    int wp;
    while ((wp = waitpid(-1, &bg_status, WNOHANG | WUNTRACED)) > 0) {
        job_t* j = job_find_by_pgid(wp);
        if (!j) {
            for (int ji = 0; ji < MAX_JOBS; ji++) {
                if (job_table[ji].status != JOB_EMPTY && job_table[ji].pgid == wp) {
                    j = &job_table[ji];
                    break;
                }
            }
        }
        if (j) {
            if (WIFSIGNALED(bg_status)) {
                write(1, "[", 1);
                print_uint((unsigned int) j->jid);
                write(1, "]+ Terminated  ", 15);
                write(1, j->cmd, strlen(j->cmd));
                write(1, "\n", 1);
                job_remove(j->pgid);
            } else if (WIFEXITED(bg_status)) {
                write(1, "[", 1);
                print_uint((unsigned int) j->jid);
                write(1, "]+ Done        ", 15);
                write(1, j->cmd, strlen(j->cmd));
                write(1, "\n", 1);
                job_remove(j->pgid);
            } else if (WIFSTOPPED(bg_status)) {
                j->status = JOB_STOPPED;
                write(1, "[", 1);
                print_uint((unsigned int) j->jid);
                write(1, "]+ Stopped     ", 15);
                write(1, j->cmd, strlen(j->cmd));
                write(1, "\n", 1);
            }
        }
    }
}

static void builtin_jobs(void) {
    reap_background_jobs();
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_table[i].status != JOB_EMPTY) {
            write(1, "[", 1);
            print_uint((unsigned int) job_table[i].jid);
            write(1, "] ", 2);
            if (job_table[i].status == JOB_STOPPED) {
                write(1, "+ Stopped  ", 11);
            } else {
                write(1, "  Running  ", 11);
            }
            write(1, job_table[i].cmd, strlen(job_table[i].cmd));
            write(1, "\n", 1);
        }
    }
}

static int builtin_fg(const command_t* cmd, int* last_status) {
    int target_jid = 1;
    if (cmd->argc >= 2) {
        const char* arg = cmd->argv[1];
        if (arg[0] == '%') arg++;
        target_jid = atoi_custom(arg);
    } else {
        int max_jid = 0;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (job_table[i].status != JOB_EMPTY && job_table[i].jid > max_jid) {
                max_jid = job_table[i].jid;
            }
        }
        target_jid = max_jid;
    }

    job_t* job = job_find_by_jid(target_jid);
    if (!job) {
        write(2, "sh: no such job\n", 16);
        *last_status = 1;
        return 1;
    }

    write(1, job->cmd, strlen(job->cmd));
    write(1, "\n", 1);

    int pgid = job->pgid;
    tcsetpgrp(0, pgid);
    kill(-pgid, SIGCONT);
    job->status = JOB_RUNNING;

    int raw_status = 0;
    int wp;
    int stopped = 0;
    while ((wp = waitpid(-pgid, &raw_status, WUNTRACED)) > 0) {
        if (WIFSTOPPED(raw_status)) {
            stopped = 1;
            break;
        }
        if (WIFEXITED(raw_status) || WIFSIGNALED(raw_status)) {
            int any_alive = 0;
            for (int p = 1; p < 64; p++) {
                if (getpgid(p) == pgid) { any_alive = 1; break; }
            }
            if (!any_alive) break;
        }
    }

    if (stopped) {
        job->status = JOB_STOPPED;
        write(1, "\n[", 2);
        print_uint((unsigned int) job->jid);
        write(1, "]+ Stopped  ", 12);
        write(1, job->cmd, strlen(job->cmd));
        write(1, "\n", 1);
        *last_status = 128 + SIGTSTP;
    } else {
        job_remove(pgid);
        *last_status = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : (128 + WTERMSIG(raw_status));
    }

    if (is_interactive) {
        tcsetpgrp(0, getpgrp());
    }
    return 1;
}

static int builtin_bg(const command_t* cmd, int* last_status) {
    int target_jid = 1;
    if (cmd->argc >= 2) {
        const char* arg = cmd->argv[1];
        if (arg[0] == '%') arg++;
        target_jid = atoi_custom(arg);
    } else {
        int max_jid = 0;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (job_table[i].status != JOB_EMPTY && job_table[i].jid > max_jid) {
                max_jid = job_table[i].jid;
            }
        }
        target_jid = max_jid;
    }

    job_t* job = job_find_by_jid(target_jid);
    if (!job) {
        write(2, "sh: no such job\n", 16);
        *last_status = 1;
        return 1;
    }

    kill(-job->pgid, SIGCONT);
    job->status = JOB_RUNNING;
    write(1, "[", 1);
    print_uint((unsigned int) job->jid);
    write(1, "]+ ", 3);
    write(1, job->cmd, strlen(job->cmd));
    write(1, " &\n", 3);
    *last_status = 0;
    return 1;
}

static int builtin_kill(const command_t* cmd, int* last_status) {
    if (cmd->argc < 2) {
        write(2, "kill: usage: kill [-sig] <pid | %jid>...\n", 41);
        *last_status = 1;
        return 1;
    }

    int sig = SIGTERM;
    int arg_idx = 1;

    if (cmd->argv[1][0] == '-' && cmd->argv[1][1] != '\0') {
        const char* sname = &cmd->argv[1][1];
        if (sname[0] >= '0' && sname[0] <= '9') {
            sig = atoi_custom(sname);
        } else if (strcmp(sname, "HUP") == 0) sig = SIGHUP;
        else if (strcmp(sname, "INT") == 0) sig = SIGINT;
        else if (strcmp(sname, "QUIT") == 0) sig = SIGQUIT;
        else if (strcmp(sname, "KILL") == 0) sig = SIGKILL;
        else if (strcmp(sname, "TERM") == 0) sig = SIGTERM;
        else if (strcmp(sname, "STOP") == 0) sig = SIGSTOP;
        else if (strcmp(sname, "TSTP") == 0) sig = SIGTSTP;
        else if (strcmp(sname, "CONT") == 0) sig = SIGCONT;
        arg_idx = 2;
    }

    if (arg_idx >= cmd->argc) {
        write(2, "kill: usage: kill [-sig] <pid | %jid>...\n", 41);
        *last_status = 1;
        return 1;
    }

    int err = 0;
    for (int i = arg_idx; i < cmd->argc; i++) {
        const char* target = cmd->argv[i];
        if (target[0] == '%') {
            int jid = atoi_custom(target + 1);
            job_t* job = job_find_by_jid(jid);
            if (!job) {
                write(2, "kill: no such job: ", 19);
                write(2, target, strlen(target));
                write(2, "\n", 1);
                err = 1;
                continue;
            }
            if (kill(-job->pgid, sig) != 0) {
                kill(job->pgid, sig);
            }
        } else {
            int pid = atoi_custom(target);
            if (kill(pid, sig) != 0) {
                write(2, "kill: failed to signal pid\n", 27);
                err = 1;
            }
        }
    }

    *last_status = err ? 1 : 0;
    return 1;
}

static int builtin_shinfo(int* last_status) {
    write(1, "sh: pid=", 8);
    print_uint((unsigned int) getpid());
    write(1, " ppid=", 6);
    print_uint((unsigned int) getppid());
    write(1, " pgid=", 6);
    print_uint((unsigned int) getpgrp());
    write(1, " free_frames=", 13);
    print_uint(sys_free_frames());
    write(1, "\n", 1);
    *last_status = 0;
    return 1;
}

static int run_builtin(const command_t* cmd, int* last_status) {
    if (cmd->argc == 0) return 1;

    if (strcmp(cmd->argv[0], "help") == 0) {
        const char* help_msg =
            "Builtins:\n"
            "  help          show this message\n"
            "  cd <dir>      change directory\n"
            "  exit [code]   exit the shell\n"
            "  status        print exit code of last command\n"
            "  jobs          list active background/stopped jobs\n"
            "  fg [%n]       bring job to foreground\n"
            "  bg [%n]       resume stopped job in background\n"
            "  kill [%n|pid] send signal to job or process\n"
            "  shinfo        print shell info (pid, ppid, pgid, frames)\n"
            "External binaries (loaded via fork + exec + waitpid from cwd or /bin):\n"
            "  ls, cat, echo, mkdir, rmdir, rm, cp, mv, touch, pwd, etc.\n";
        write(1, help_msg, strlen(help_msg));
        *last_status = 0;
        return 1;
    }

    if (strcmp(cmd->argv[0], "jobs") == 0) {
        builtin_jobs();
        *last_status = 0;
        return 1;
    }

    if (strcmp(cmd->argv[0], "fg") == 0) {
        return builtin_fg(cmd, last_status);
    }

    if (strcmp(cmd->argv[0], "bg") == 0) {
        return builtin_bg(cmd, last_status);
    }

    if (strcmp(cmd->argv[0], "kill") == 0) {
        return builtin_kill(cmd, last_status);
    }

    if (strcmp(cmd->argv[0], "shinfo") == 0) {
        return builtin_shinfo(last_status);
    }

    if (strcmp(cmd->argv[0], "frames") == 0) {
        write(1, "free_frames=", 12);
        print_uint(sys_free_frames());
        write(1, "\n", 1);
        *last_status = 0;
        return 1;
    }

    if (strcmp(cmd->argv[0], "cd") == 0) {
        const char* target = (cmd->argc >= 2) ? cmd->argv[1] : "/";
        if (chdir(target) != 0) {
            write(2, "cd: ", 4);
            write(2, target, strlen(target));
            write(2, ": No such file or directory\n", 28);
            *last_status = 1;
        } else {
            *last_status = 0;
        }
        return 1;
    }

    if (strcmp(cmd->argv[0], "status") == 0) {
        char sbuf[12];
        int_to_str(*last_status, sbuf);
        write(1, sbuf, strlen(sbuf));
        write(1, "\n", 1);
        return 1;
    }

    if (strcmp(cmd->argv[0], "exit") == 0) {
        int code = *last_status;
        if (cmd->argc >= 2) {
            code = 0;
            const char* a = cmd->argv[1];
            for (int i = 0; a[i] >= '0' && a[i] <= '9'; i++) {
                code = code * 10 + (a[i] - '0');
            }
        }
        const char* exit_msg = "Exiting shell.\n";
        write(1, exit_msg, strlen(exit_msg));
        exit(code);
    }

    return 0; /* not a builtin */
}

static void execute_pipeline(char* tokens[], int ntok, int* last_status) {
    if (ntok == 0) return;

    int is_background = 0;
    if (ntok > 0 && strcmp(tokens[ntok - 1], "&") == 0) {
        is_background = 1;
        ntok--;
        tokens[ntok] = 0;
    }
    if (ntok == 0) return;

    /* Split stages on '|' */
    int stage_starts[MAX_STAGES];
    int stage_lens[MAX_STAGES];
    int num_stages = 0;
    stage_starts[0] = 0;

    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            stage_lens[num_stages] = i - stage_starts[num_stages];
            num_stages++;
            if (num_stages < MAX_STAGES) {
                stage_starts[num_stages] = i + 1;
            } else {
                write(2, "sh: too many pipeline stages\n", 29);
                *last_status = 1;
                return;
            }
        }
    }
    stage_lens[num_stages] = ntok - stage_starts[num_stages];
    num_stages++;

    command_t cmds[MAX_STAGES];
    for (int s = 0; s < num_stages; s++) {
        if (parse_command(&tokens[stage_starts[s]], stage_lens[s], &cmds[s]) != 0) {
            *last_status = 1;
            return;
        }
        if (cmds[s].argc == 0) {
            write(2, "sh: invalid empty command in pipeline\n", 38);
            *last_status = 1;
            return;
        }
    }

    /* Single builtin command in foreground runs in parent */
    if (num_stages == 1 && !is_background) {
        if (run_builtin(&cmds[0], last_status)) {
            return;
        }
    }

    /* Create pipes for N stages */
    int pipes[MAX_STAGES - 1][2];
    for (int i = 0; i < num_stages - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            write(2, "sh: pipe failed\n", 16);
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            *last_status = 1;
            return;
        }
    }

    char cmd_str[64];
    cmd_str[0] = '\0';
    int clen = 0;
    for (int i = 0; i < ntok && clen < 60; i++) {
        if (i > 0 && clen < 60) cmd_str[clen++] = ' ';
        int sl = strlen(tokens[i]);
        if (clen + sl >= 60) sl = 60 - clen;
        for (int k = 0; k < sl; k++) cmd_str[clen++] = tokens[i][k];
        cmd_str[clen] = '\0';
    }

    int pids[MAX_STAGES];
    for (int s = 0; s < num_stages; s++) {
        pids[s] = fork();
        if (pids[s] < 0) {
            write(2, "sh: fork failed\n", 16);
            for (int j = 0; j < num_stages - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            *last_status = 1;
            return;
        }

        if (pids[s] == 0) {
            /* Reset signal handlers in child to default */
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            if (is_interactive) {
                int pgid = (s == 0) ? getpid() : pids[0];
                setpgid(0, pgid);
                if (!is_background) {
                    tcsetpgrp(0, pgid);
                }
            }

            /* Child process: connect pipes */
            if (s > 0) {
                dup2(pipes[s - 1][0], 0);
            }
            if (s < num_stages - 1) {
                dup2(pipes[s][1], 1);
            }

            /* Close all pipe file descriptors in child */
            for (int j = 0; j < num_stages - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            /* Check if builtin inside pipeline */
            if (run_builtin(&cmds[s], last_status)) {
                exit(*last_status);
            }

            exec_command(&cmds[s]);
            exit(127);
        } else {
            if (is_interactive) {
                setpgid(pids[s], pids[0]);
            }
        }
    }

    /* Parent process: close all pipe descriptors */
    for (int j = 0; j < num_stages - 1; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }

    if (is_background) {
        int jid = job_add(pids[0], JOB_RUNNING, cmd_str);
        write(1, "[", 1);
        print_uint((unsigned int) (jid > 0 ? jid : 1));
        write(1, "] ", 2);
        print_uint((unsigned int) pids[0]);
        write(1, "\n", 1);
    } else {
        if (is_interactive) {
            tcsetpgrp(0, pids[0]);
        }
        int stopped = 0;
        int last_st = 0;
        for (int s = 0; s < num_stages; s++) {
            int raw_status = 0;
            while (waitpid(pids[s], &raw_status, WUNTRACED) > 0) {
                if (WIFSTOPPED(raw_status)) {
                    stopped = 1;
                    break;
                }
                if (WIFEXITED(raw_status) || WIFSIGNALED(raw_status)) {
                    if (s == num_stages - 1) {
                        last_st = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : (128 + WTERMSIG(raw_status));
                    }
                    break;
                }
            }
        }

        if (stopped) {
            int jid = job_add(pids[0], JOB_STOPPED, cmd_str);
            write(1, "\n[", 2);
            print_uint((unsigned int) (jid > 0 ? jid : 1));
            write(1, "]+ Stopped  ", 12);
            write(1, cmd_str, strlen(cmd_str));
            write(1, "\n", 1);
            *last_status = 128 + SIGTSTP;
        } else {
            *last_status = last_st;
        }
        if (is_interactive) {
            tcsetpgrp(0, getpgrp());
        }
    }
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    is_interactive = isatty(0);
    if (is_interactive) {
        setpgid(0, 0);
        tcsetpgrp(0, getpgrp());
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);

        const char* welcome = "\nMyOS Userland Shell (sh)\nType 'help' for builtins, or enter binary name to execute.\n";
        write(1, welcome, strlen(welcome));
    }

    char line[256];
    char token_buf[512];
    char* tokens[64];
    int last_status = 0;

    for (;;) {
        /* Non-blocking reap of background jobs before next prompt */
        reap_background_jobs();

        if (is_interactive) {
            char cwd[64];
            if (!getcwd(cwd, sizeof(cwd))) {
                cwd[0] = '/';
                cwd[1] = '\0';
            }

            write(1, "sh:", 3);
            write(1, cwd, strlen(cwd));
            write(1, "$ ", 2);
        }

        int n = read(0, line, sizeof(line) - 1);
        if (n <= 0) {
            if (n == 0) break;
            continue;
        }
        line[n] = '\0';

        /* Strip trailing newline / carriage return */
        for (int i = 0; i < n; i++) {
            if (line[i] == '\r' || line[i] == '\n') {
                line[i] = '\0';
                break;
            }
        }

        /* Split commands separated by ';' */
        char* cur = line;
        while (*cur) {
            while (*cur && is_space(*cur)) cur++;
            if (!*cur) break;

            char* end = cur;
            int in_sq = 0, in_dq = 0;
            while (*end) {
                if (*end == '\'' && !in_dq) in_sq = !in_sq;
                else if (*end == '"' && !in_sq) in_dq = !in_dq;
                else if (*end == ';' && !in_sq && !in_dq) break;
                end++;
            }

            char saved = *end;
            *end = '\0';

            int ntok = tokenize_cmd(cur, tokens, 64, token_buf, sizeof(token_buf), last_status);
            if (ntok > 0) {
                execute_pipeline(tokens, ntok, &last_status);
            }

            if (saved == ';') {
                cur = end + 1;
            } else {
                break;
            }
        }
    }
    return 0;
}
