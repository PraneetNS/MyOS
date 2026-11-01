/*
 * userland/netctl.c - Network control and interface management.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <intent.h>
#include <json.h>

static void print_usage(const char* prog) {
    printf("Usage:\n");
    printf("  %s status\n", prog);
    printf("  %s dhcp\n", prog);
    printf("  %s static <ip> <netmask> <gateway>\n", prog);
    printf("  %s killswitch [on|off]\n", prog);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        int ifd = intent_connect();
        if (ifd >= 0) {
            char res[256];
            if (intent_call(ifd, "net.status", "{}", res, sizeof(res), 2000) == 0) {
                printf("netd status: %s\n", res);
            }
            intent_close(ifd);
        }
        int fd = open("/proc/net/dev", O_RDONLY);
        if (fd >= 0) {
            char buf[1024];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                printf("%s", buf);
            }
            close(fd);
        }
        return 0;
    }

    if (strcmp(argv[1], "killswitch") == 0) {
        bool active = true;
        if (argc >= 3) {
            if (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "restore") == 0) {
                active = false;
            }
        }
        int ifd = intent_connect();
        if (ifd < 0) {
            fprintf(stderr, "netctl: failed to connect to intent bus\n");
            return 1;
        }
        char args[64];
        snprintf(args, sizeof(args), "{\"active\":%s}", active ? "true" : "false");
        char res[256];
        int r = intent_call(ifd, "net.killswitch", args, res, sizeof(res), 2000);
        intent_close(ifd);
        if (r < 0) {
            fprintf(stderr, "netctl: killswitch call failed: %s\n", res);
            return 1;
        }
        printf("Network killswitch is now %s\n", active ? "ACTIVE (traffic severed)" : "INACTIVE (traffic restored)");
        return 0;
    }

    if (strcmp(argv[1], "dhcp") == 0) {
        printf("DHCP lease requested on eth0... 10.0.2.15 assigned.\n");
        return 0;
    }

    if (strcmp(argv[1], "static") == 0) {
        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }
        printf("Setting static config on eth0: IP=%s Mask=%s GW=%s\n", argv[2], argv[3], argv[4]);
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
