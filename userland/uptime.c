#include "libc.h"

int main(void) {
    int fd = open("/proc/uptime", O_RDONLY, 0);
    if (fd < 0) {
        const char* err = "uptime: cannot open /proc/uptime\n";
        write(2, err, strlen(err));
        return 1;
    }
    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 1;
    buf[n] = '\0';

    unsigned int up_sec = 0;
    int idx = 0;
    while (buf[idx] >= '0' && buf[idx] <= '9') {
        up_sec = up_sec * 10 + (buf[idx] - '0');
        idx++;
    }
    while (buf[idx] && buf[idx] != ' ') idx++;
    if (buf[idx] == ' ') idx++;
    unsigned int idle_sec = 0;
    while (buf[idx] >= '0' && buf[idx] <= '9') {
        idle_sec = idle_sec * 10 + (buf[idx] - '0');
        idx++;
    }

    write(1, "up ", 3);
    print_uint(up_sec);
    write(1, " seconds (idle ", 15);
    print_uint(idle_sec);
    write(1, " seconds)\n", 10);
    return 0;
}
