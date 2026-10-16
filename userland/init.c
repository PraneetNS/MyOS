/* init.c - System Init process (PID 1)
   Reads /etc/init.conf, manages service lifetimes (once, wait, respawn),
   enforces respawn rate limiting, reaps orphaned zombies, and inspects /proc/cmdline.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#define MAX_SERVICES 16
#define MAX_ARGS 16
#define RATE_LIMIT_COUNT 5
#define RATE_LIMIT_WINDOW 10 /* seconds */
#define RATE_LIMIT_BACKOFF 5 /* seconds */

typedef enum {
    SVC_ONCE,
    SVC_WAIT,
    SVC_RESPAWN
} svc_type_t;

typedef struct {
    svc_type_t type;
    char tty[64];
    char cmd[128];
    char* argv[MAX_ARGS];
    int argc;
    pid_t current_pid;
    time_t spawn_times[RATE_LIMIT_COUNT];
    int spawn_count;
} service_t;

static service_t services[MAX_SERVICES];
static int num_services = 0;
static volatile sig_atomic_t sigchld_received = 0;
static volatile sig_atomic_t g_terminating = 0;

static void handle_sigchld(int sig) {
    (void) sig;
    sigchld_received = 1;
}

static void handle_sigterm(int sig) {
    (void) sig;
    g_terminating = 1;
}

static void parse_line(char* line) {
    /* Strip newline and trailing whitespace */
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    /* Skip leading whitespace */
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '#') return;

    if (num_services >= MAX_SERVICES) return;

    service_t* svc = &services[num_services];
    memset(svc, 0, sizeof(*svc));
    svc->current_pid = -1;

    char* token = strtok(p, " \t");
    if (!token) return;

    if (strcmp(token, "once") == 0) {
        svc->type = SVC_ONCE;
        token = strtok(NULL, " \t");
        if (!token) return;
        strncpy(svc->cmd, token, sizeof(svc->cmd) - 1);
        svc->argv[svc->argc++] = strdup(token);
    } else if (strcmp(token, "wait") == 0) {
        svc->type = SVC_WAIT;
        token = strtok(NULL, " \t");
        if (!token) return;
        strncpy(svc->cmd, token, sizeof(svc->cmd) - 1);
        svc->argv[svc->argc++] = strdup(token);
    } else if (strcmp(token, "respawn") == 0) {
        svc->type = SVC_RESPAWN;
        char* tty_tok = strtok(NULL, " \t");
        if (!tty_tok) return;
        strncpy(svc->tty, tty_tok, sizeof(svc->tty) - 1);
        token = strtok(NULL, " \t");
        if (!token) return;
        strncpy(svc->cmd, token, sizeof(svc->cmd) - 1);
        svc->argv[svc->argc++] = strdup(token);
    } else {
        return; /* unknown directive */
    }

    while ((token = strtok(NULL, " \t")) != NULL && svc->argc < MAX_ARGS - 1) {
        svc->argv[svc->argc++] = strdup(token);
    }
    svc->argv[svc->argc] = NULL;
    num_services++;
}

static void load_config(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        /* Default fallback configuration */
        char default_entry[] = "respawn /dev/console /bin/sh.elf";
        parse_line(default_entry);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        parse_line(line);
    }
    fclose(fp);

    if (num_services == 0) {
        char default_entry[] = "respawn /dev/console /bin/sh.elf";
        parse_line(default_entry);
    }
}

static void check_cmdline_extra(void) {
    FILE* fp = fopen("/proc/cmdline", "r");
    if (!fp) return;

    char buf[256];
    if (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "serial_shell=1")) {
            /* Check if serial0 already configured */
            int found = 0;
            for (int i = 0; i < num_services; i++) {
                if (strstr(services[i].tty, "serial0")) {
                    found = 1;
                    break;
                }
            }
            if (!found && num_services < MAX_SERVICES) {
                char serial_entry[] = "respawn /dev/serial0 /bin/sh.elf";
                parse_line(serial_entry);
            }
        }
    }
    fclose(fp);
}

static void spawn_service(service_t* svc) {
    /* Rate limiting check for respawn services */
    time_t now = time(NULL);
    if (svc->type == SVC_RESPAWN) {
        /* Shift out events older than RATE_LIMIT_WINDOW */
        int recent = 0;
        for (int i = 0; i < svc->spawn_count; i++) {
            if (now - svc->spawn_times[i] <= RATE_LIMIT_WINDOW) {
                svc->spawn_times[recent++] = svc->spawn_times[i];
            }
        }
        svc->spawn_count = recent;

        if (svc->spawn_count >= RATE_LIMIT_COUNT) {
            printf("[init] respawn rate limit reached on %s, backing off %ds\n", svc->cmd, RATE_LIMIT_BACKOFF);
            fflush(stdout);
            sleep(RATE_LIMIT_BACKOFF);
            now = time(NULL);
            svc->spawn_count = 0;
        }
        svc->spawn_times[svc->spawn_count++] = now;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("[init] fork failed for %s\n", svc->cmd);
        return;
    }

    if (pid == 0) {
        /* Child */
        if (svc->tty[0]) {
            setsid();
            int fd = open(svc->tty, O_RDWR);
            if (fd >= 0) {
                ioctl(fd, TIOCSCTTY, 0);
                dup2(fd, 0);
                dup2(fd, 1);
                dup2(fd, 2);
                if (fd > 2) close(fd);
            }
        }

        execvp(svc->cmd, svc->argv);

        /* Try /bin/ prefix */
        char bin_path[160];
        snprintf(bin_path, sizeof(bin_path), "/bin/%s", svc->cmd);
        execvp(bin_path, svc->argv);

        /* Try with .elf extension */
        char elf_path[160];
        snprintf(elf_path, sizeof(elf_path), "%s.elf", svc->cmd);
        execvp(elf_path, svc->argv);
        snprintf(elf_path, sizeof(elf_path), "/bin/%s.elf", svc->cmd);
        execvp(elf_path, svc->argv);

        exit(127);
    }

    svc->current_pid = pid;

    if (svc->type == SVC_WAIT) {
        int st = 0;
        waitpid(pid, &st, 0);
        svc->current_pid = -1;
    }
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;

    /* Verify running as PID 1 */
    if (getpid() != 1) {
        printf("init: must be run as PID 1\n");
        return 1;
    }

    /* Set signal handlers for init */
    signal(SIGINT, handle_sigterm);
    signal(SIGTERM, handle_sigterm);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCHLD, handle_sigchld);

    /* Load /etc/init.conf */
    load_config("/etc/init.conf");
    check_cmdline_extra();

    /* Start initial services */
    for (int i = 0; i < num_services; i++) {
        spawn_service(&services[i]);
    }

    /* Main init supervision & orphan reaping loop */
    while (1) {
        int status = 0;
        pid_t reaped;

        while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
            int handled = 0;
            for (int i = 0; i < num_services; i++) {
                if (services[i].type == SVC_RESPAWN && services[i].current_pid == reaped) {
                    services[i].current_pid = -1;
                    if (!g_terminating) {
                        if (strstr(services[i].cmd, "sh")) {
                            printf("[init] shell exited, respawning sh.elf...\n");
                            fflush(stdout);
                        }
                        spawn_service(&services[i]);
                        if (strstr(services[i].cmd, "sh") && services[i].current_pid > 0) {
                            printf("[init] respawned sh.elf (PID %d)\n", services[i].current_pid);
                            fflush(stdout);
                        }
                    }
                    handled = 1;
                    break;
                }
            }
            if (!handled) {
                /* Orphaned child reaped */
            }
        }

        if (g_terminating) {
            /* If system is shutting down, stop respawning and exit cleanly */
            break;
        }

        /* Sleep briefly waiting for signals */
        sleep(1);
    }

    return 0;
}
