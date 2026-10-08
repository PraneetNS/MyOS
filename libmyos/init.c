#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

void __libmyos_init(int argc, char **argv, char **envp) {
    (void) argc;
    (void) argv;
    (void) envp;

    /* Initialize timezone from /etc/timezone if TZ environment variable is unset */
    if (!getenv("TZ")) {
        int fd = open("/etc/timezone", O_RDONLY);
        if (fd >= 0) {
            char buf[32];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                int sign = 1;
                int idx = 0;
                while (buf[idx] == ' ' || buf[idx] == '\t' || buf[idx] == '\r' || buf[idx] == '\n') idx++;
                if (buf[idx] == '-') { sign = -1; idx++; }
                else if (buf[idx] == '+') { idx++; }
                int val = 0;
                while (buf[idx] >= '0' && buf[idx] <= '9') {
                    val = val * 10 + (buf[idx] - '0');
                    idx++;
                }
                int offset_min = sign * val;
                char tz_buf[32];
                int h = (offset_min < 0 ? -offset_min : offset_min) / 60;
                int m = (offset_min < 0 ? -offset_min : offset_min) % 60;
                const char *name = (offset_min == 330) ? "IST" : "LOC";
                if (offset_min > 0) {
                    /* East of Prime Meridian: negative sign in POSIX TZ */
                    snprintf(tz_buf, sizeof(tz_buf), "%s-%d:%02d", name, h, m);
                } else if (offset_min < 0) {
                    /* West of Prime Meridian: positive sign in POSIX TZ */
                    snprintf(tz_buf, sizeof(tz_buf), "%s+%d:%02d", name, h, m);
                } else {
                    snprintf(tz_buf, sizeof(tz_buf), "UTC0");
                }
                setenv("TZ", tz_buf, 0);
                tzset();
            }
        }
    }
}
