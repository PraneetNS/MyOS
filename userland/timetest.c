#include "libc.h"

static void get_field(const char* buf, const char* key, char* out, int out_max) {
    int key_len = strlen(key);
    for (int i = 0; buf[i]; i++) {
        int match = 1;
        for (int k = 0; k < key_len; k++) {
            if (buf[i + k] != key[k]) { match = 0; break; }
        }
        if (match) {
            int p = i + key_len;
            while (buf[p] == ':' || buf[p] == '\t' || buf[p] == ' ') p++;
            int o = 0;
            while (buf[p] && buf[p] != '\n' && o < out_max - 1) {
                out[o++] = buf[p++];
            }
            out[o] = '\0';
            return;
        }
    }
    out[0] = '\0';
}

static unsigned int parse_uint(const char* s) {
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

int main(void) {
    write(1, "=== RUNNING TIMETEST ===\n", 25);

    /* 1. Test sleep 2: check elapsed ticks and sleeper CPU ticks */
    unsigned int start_ticks = sys_ticks();
    int child = fork();
    if (child == 0) {
        sleep(2);

        /* Read own CPU ticks from /proc/self/status */
        int fd = open("/proc/self/status", O_RDONLY, 0);
        char buf[512];
        int n = (fd >= 0) ? read(fd, buf, sizeof(buf) - 1) : 0;
        if (fd >= 0) close(fd);
        unsigned int my_ticks = 0;
        if (n > 0) {
            buf[n] = '\0';
            char ticks_str[32];
            get_field(buf, "Ticks", ticks_str, sizeof(ticks_str));
            my_ticks = parse_uint(ticks_str);
        }

        write(1, "[timetest] sleeper cpu_ticks=", 29);
        print_uint(my_ticks);
        if (my_ticks < 5) {
            write(1, " (< 5) PASS\n", 12);
            exit(0);
        } else {
            write(1, " (>= 5) FAIL\n", 13);
            exit(1);
        }
    }

    int st = 0;
    waitpid(child, &st, 0);
    unsigned int end_ticks = sys_ticks();
    unsigned int elapsed = end_ticks - start_ticks;

    write(1, "[timetest] sleep 2 elapsed ticks=", 33);
    print_uint(elapsed);
    if (elapsed >= 190 && elapsed <= 230 && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        write(1, " (190-230) PASS\n", 16);
    } else {
        write(1, " FAIL\n", 6);
        return 1;
    }

    /* 2. Test /proc/meminfo free frames vs sys_free_frames() */
    int mfd = open("/proc/meminfo", O_RDONLY, 0);
    if (mfd < 0) {
        write(1, "FAIL: cannot open /proc/meminfo\n", 32);
        return 1;
    }
    char mbuf[512];
    int mn = read(mfd, mbuf, sizeof(mbuf) - 1);
    close(mfd);
    if (mn <= 0) return 1;
    mbuf[mn] = '\0';

    char frames_str[32];
    get_field(mbuf, "FramesFree", frames_str, sizeof(frames_str));
    unsigned int proc_free = parse_uint(frames_str);
    unsigned int sys_free  = sys_free_frames();

    write(1, "[timetest] meminfo FramesFree=", 30);
    print_uint(proc_free);
    write(1, " vs sys_free_frames=", 20);
    print_uint(sys_free);

    /* Allow at most small variance (<= 5 frames) due to buffer reading allocation */
    int diff = (proc_free > sys_free) ? (proc_free - sys_free) : (sys_free - proc_free);
    if (diff <= 5) {
        write(1, " MATCH PASS\n", 12);
    } else {
        write(1, " MISMATCH FAIL\n", 15);
        return 1;
    }

    write(1, "=== ALL TIMETESTS PASSED ===\n", 29);
    return 0;
}
