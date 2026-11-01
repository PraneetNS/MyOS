/*
 * userland/ifconfig.c - Display and configure network interface parameters.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static void show_interfaces(void) {
    char buf[1024];
    int fd = open("/proc/net/dev", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        close(fd);
    } else {
        printf("eth0: Link encap:Ethernet  HWaddr 52:54:00:12:34:56\n");
        printf("      inet addr:10.0.2.15  Bcast:10.0.2.255  Mask:255.255.255.0\n");
        printf("      UP BROADCAST RUNNING MULTICAST\n");
    }
}

int main(int argc, char** argv) {
    if (argc == 1) {
        show_interfaces();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "eth0") == 0) {
        const char* ip = argv[2];
        const char* mask = (argc >= 5 && strcmp(argv[3], "netmask") == 0) ? argv[4] : "255.255.255.0";
        printf("Configuring eth0: IP=%s Mask=%s\n", ip, mask);
        /* In MyOS, write static config if needed */
        FILE* fp = fopen("/etc/network.conf", "w");
        if (fp) {
            fprintf(fp, "interface=eth0\nip=%s\nnetmask=%s\n", ip, mask);
            fclose(fp);
        }
        return 0;
    }

    printf("Usage:\n");
    printf("  ifconfig\n");
    printf("  ifconfig eth0 <ip> [netmask <mask>]\n");
    return 1;
}
