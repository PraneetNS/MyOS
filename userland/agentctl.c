#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <agent.h>

static void print_usage(const char* prog) {
    printf("Usage: %s <command> [args...]\n\n", prog);
    printf("Commands:\n");
    printf("  create <name> [options]    Create a new kernel agent\n");
    printf("    --fs-read <prefix>       Allow read access to path prefix\n");
    printf("    --fs-write <prefix>      Allow write access to path prefix\n");
    printf("    --exec <prefix>          Allow exec access to path prefix\n");
    printf("    --connect <prefix>       Allow socket connect to path prefix\n");
    printf("    --window                 Allow window management capability\n");
    printf("    --spawn                  Allow spawning/delegating child agents\n");
    printf("    --net-proxy              Allow network proxy capability\n");
    printf("    --bus-call <prefix>      Allow calling intent bus skills with prefix\n");
    printf("    --bus-provide <prefix>   Allow providing intent bus skills with prefix\n");
    printf("    --window-observe <app>   Allow observing GUI a11y trees for app\n");
    printf("    --window-act <app>       Allow acting on GUI a11y trees for app\n");
    printf("    --window-secret          Allow interacting with secret/password fields\n");
    printf("    --window-raw             Allow raw input injection and screenshots\n");
    printf("    --model-backend <list>   Allow model backends (local, host, local,host)\n");
    printf("    --cpu <ticks>            Set maximum CPU ticks budget\n");
    printf("    --mem <pages>            Set maximum memory pages budget\n");
    printf("    --procs <count>          Set maximum active processes budget\n");
    printf("    --tokens <count>         Set maximum tokens budget\n\n");
    printf("  list                       List active kernel agents\n");
    printf("  log [-f]                   Show kernel agent audit log\n");
    printf("  kill <id>                  Kill an agent and all its processes\n");
    printf("  suspend <id>               Suspend an agent\n");
    printf("  resume <id>                Resume an agent\n");
    printf("  run <id> <cmd...>          Run a command inside an agent\n");
}

static int cmd_create(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Error: agent name required\n");
        return 1;
    }

    const char* name = argv[2];
    agent_caps_t caps;
    agent_budget_t budget;
    memset(&caps, 0, sizeof(caps));
    memset(&budget, 0, sizeof(budget));

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--fs-read") == 0 && i + 1 < argc) {
            caps.mask |= CAP_FS_READ;
            if (caps.num_fs_read < MAX_PATH_RULES) {
                strncpy(caps.fs_read[caps.num_fs_read].prefix, argv[++i], MAX_PATH_PREFIX_LEN - 1);
                caps.num_fs_read++;
            } else {
                i++;
            }
        } else if (strcmp(argv[i], "--fs-write") == 0 && i + 1 < argc) {
            caps.mask |= CAP_FS_WRITE;
            if (caps.num_fs_write < MAX_PATH_RULES) {
                strncpy(caps.fs_write[caps.num_fs_write].prefix, argv[++i], MAX_PATH_PREFIX_LEN - 1);
                caps.num_fs_write++;
            } else {
                i++;
            }
        } else if (strcmp(argv[i], "--exec") == 0 && i + 1 < argc) {
            caps.mask |= CAP_EXEC;
            if (caps.num_exec < MAX_PATH_RULES) {
                strncpy(caps.exec[caps.num_exec].prefix, argv[++i], MAX_PATH_PREFIX_LEN - 1);
                caps.num_exec++;
            } else {
                i++;
            }
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            caps.mask |= CAP_SOCKET_CONNECT;
            if (caps.num_connect < MAX_PATH_RULES) {
                strncpy(caps.connect[caps.num_connect].prefix, argv[++i], MAX_PATH_PREFIX_LEN - 1);
                caps.num_connect++;
            } else {
                i++;
            }
        } else if (strcmp(argv[i], "--window") == 0) {
            caps.mask |= CAP_WINDOW;
        } else if (strcmp(argv[i], "--spawn") == 0) {
            caps.mask |= CAP_SPAWN;
        } else if (strcmp(argv[i], "--net-proxy") == 0) {
            caps.mask |= CAP_NET_PROXY;
        } else if (strcmp(argv[i], "--bus-call") == 0 && i + 1 < argc) {
            caps.mask |= CAP_BUS_CALL;
            if (caps.num_bus_call < MAX_PATH_RULES) {
                strncpy(caps.bus_call[caps.num_bus_call].prefix, argv[++i], MAX_PATH_PREFIX_LEN - 1);
                caps.num_bus_call++;
            } else {
                i++;
            }
        } else if (strcmp(argv[i], "--bus-provide") == 0 && i + 1 < argc) {
            caps.mask |= CAP_BUS_PROVIDE;
            if (caps.num_bus_provide < MAX_PATH_RULES) {
                strncpy(caps.bus_provide[caps.num_bus_provide].prefix, argv[++i], MAX_PATH_PREFIX_LEN - 1);
                caps.num_bus_provide++;
            } else {
                i++;
            }
        } else if (strcmp(argv[i], "--window-observe") == 0 && i + 1 < argc) {
            caps.mask |= CAP_WINDOW_OBSERVE;
            if (caps.num_window_observe < MAX_PATH_RULES) {
                strncpy(caps.window_observe[caps.num_window_observe].prefix, argv[++i], MAX_PATH_PREFIX_LEN - 1);
                caps.num_window_observe++;
            } else {
                i++;
            }
        } else if (strcmp(argv[i], "--window-act") == 0 && i + 1 < argc) {
            caps.mask |= CAP_WINDOW_ACT;
            if (caps.num_window_act < MAX_PATH_RULES) {
                strncpy(caps.window_act[caps.num_window_act].prefix, argv[++i], MAX_PATH_PREFIX_LEN - 1);
                caps.num_window_act++;
            } else {
                i++;
            }
        } else if (strcmp(argv[i], "--window-secret") == 0) {
            caps.mask |= CAP_WINDOW_SECRET;
        } else if (strcmp(argv[i], "--window-raw") == 0) {
            caps.mask |= CAP_WINDOW_RAW;
        } else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            budget.max_cpu_ticks = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mem") == 0 && i + 1 < argc) {
            budget.max_mem_pages = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--procs") == 0 && i + 1 < argc) {
            budget.max_procs = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tokens") == 0 && i + 1 < argc) {
            budget.max_tokens = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--model-backend") == 0 && i + 1 < argc) {
            const char* val = argv[++i];
            caps.model_backends = 0;
            if (strstr(val, "local")) caps.model_backends |= MODEL_BACKEND_LOCAL;
            if (strstr(val, "host")) caps.model_backends |= MODEL_BACKEND_HOST;
            if (strcmp(val, "none") == 0) caps.model_backends = 0x80000000;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    int id = agent_create(name, &caps, &budget);
    if (id < 0) {
        perror("agent_create");
        return 1;
    }

    printf("Agent %d created: %s\n", id, name);
    return 0;
}

static int cmd_list(void) {
    printf("%-4s %-20s %-10s %-5s %-9s %-9s\n", "ID", "NAME", "STATE", "PROCS", "CPU_TICKS", "MEM_PAGES");
    int count = 0;
    for (int id = 1; id <= MAX_AGENTS; id++) {
        agent_info_t info;
        if (agent_info(id, &info) == 0 && info.state != AGENT_STATE_UNUSED) {
            const char* st = "UNKNOWN";
            if (info.state == AGENT_STATE_RUNNING) st = "RUNNING";
            else if (info.state == AGENT_STATE_SUSPENDED) st = "SUSPENDED";
            else if (info.state == AGENT_STATE_KILLED) st = "KILLED";

            printf("%-4d %-20s %-10s %-5u %-9u %-9u\n",
                   info.id, info.name, st,
                   (unsigned int)info.usage.procs_count, (unsigned int)info.usage.cpu_ticks, (unsigned int)info.usage.mem_pages);
            count++;
        }
    }
    if (count == 0) {
        printf("(no active agents)\n");
    }
    return 0;
}

static int cmd_log(int argc, char** argv) {
    int follow = 0;
    if (argc >= 3 && strcmp(argv[2], "-f") == 0) {
        follow = 1;
    }

    int fd = open("/proc/agents/log", O_RDONLY);
    if (fd < 0) {
        perror("open /proc/agents/log");
        return 1;
    }

    char buf[1024];
    for (;;) {
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("%s", buf);
            fflush(stdout);
        } else if (n == 0) {
            if (!follow) break;
            struct timespec req = { 0, 100000000 }; /* 100 ms */
            nanosleep(&req, NULL);
        } else {
            perror("read log");
            break;
        }
    }

    close(fd);
    return 0;
}

static int cmd_kill(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Error: agent id required\n");
        return 1;
    }
    int id = atoi(argv[2]);
    if (agent_kill(id) < 0) {
        perror("agent_kill");
        return 1;
    }
    printf("Agent %d killed.\n", id);
    return 0;
}

static int cmd_suspend(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Error: agent id required\n");
        return 1;
    }
    int id = atoi(argv[2]);
    if (agent_suspend(id) < 0) {
        perror("agent_suspend");
        return 1;
    }
    printf("Agent %d suspended.\n", id);
    return 0;
}

static int cmd_resume(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Error: agent id required\n");
        return 1;
    }
    int id = atoi(argv[2]);
    if (agent_resume(id) < 0) {
        perror("agent_resume");
        return 1;
    }
    printf("Agent %d resumed.\n", id);
    return 0;
}

static int cmd_run(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: agentctl run <id> <cmd...>\n");
        return 1;
    }

    int id = atoi(argv[2]);
    char* const* cmd_argv = &argv[3];

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Child */
        if (agent_attach(id) < 0) {
            perror("agent_attach");
            exit(1);
        }

        execvp(cmd_argv[0], cmd_argv);

        /* Fallback: try prepending /bin/ */
        char bin_path[128];
        snprintf(bin_path, sizeof(bin_path), "/bin/%s", cmd_argv[0]);
        execv(bin_path, cmd_argv);

        snprintf(bin_path, sizeof(bin_path), "/bin/%s.elf", cmd_argv[0]);
        execv(bin_path, cmd_argv);

        perror("execvp");
        exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "create") == 0) {
        return cmd_create(argc, argv);
    } else if (strcmp(argv[1], "list") == 0) {
        return cmd_list();
    } else if (strcmp(argv[1], "log") == 0) {
        return cmd_log(argc, argv);
    } else if (strcmp(argv[1], "kill") == 0) {
        return cmd_kill(argc, argv);
    } else if (strcmp(argv[1], "suspend") == 0) {
        return cmd_suspend(argc, argv);
    } else if (strcmp(argv[1], "resume") == 0) {
        return cmd_resume(argc, argv);
    } else if (strcmp(argv[1], "run") == 0) {
        return cmd_run(argc, argv);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }
}
