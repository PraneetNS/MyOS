/*
 * userland/netstat.c - Print network connections, routing tables, and interface statistics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void dump_file(const char* title, const char* path) {
    printf("=== %s ===\n", title);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buf[1024];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        close(fd);
    } else {
        printf("(none or not available)\n");
    }
    printf("\n");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    dump_file("Active Internet connections (TCP)", "/proc/net/tcp");
    dump_file("Active Internet connections (UDP)", "/proc/net/udp");
    dump_file("Kernel IP routing table", "/proc/net/route");
    dump_file("Network Device Statistics", "/proc/net/dev");

    return 0;
}
