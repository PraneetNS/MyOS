/*
 * userland/nc.c - Arbitrary TCP and UDP connections and listens.
 * Security: Human/non-agent only. Direct raw sockets are denied to agents.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static void print_usage(const char* prog) {
    printf("Usage:\n");
    printf("  %s [-u] <host> <port>       (Connect mode)\n", prog);
    printf("  %s -l [-u] <port>           (Listen mode)\n", prog);
}

int main(int argc, char** argv) {
    bool listen_mode = false;
    bool udp_mode = false;
    const char* host = NULL;
    int port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            listen_mode = true;
        } else if (strcmp(argv[i], "-u") == 0) {
            udp_mode = true;
        } else if (!host && !listen_mode) {
            host = argv[i];
        } else if (port == 0) {
            port = atoi(argv[i]);
        }
    }

    if (listen_mode && port == 0 && host) {
        port = atoi(host);
        host = NULL;
    }

    if (port <= 0 || port > 65535 || (!listen_mode && !host)) {
        print_usage(argv[0]);
        return 1;
    }

    int type = udp_mode ? SOCK_DGRAM : SOCK_STREAM;
    int sock = socket(AF_INET, type, 0);
    if (sock < 0) {
        if (errno == EACCES) {
            fprintf(stderr, "nc: direct raw socket denied for agents (use netd/net.http)\n");
        } else {
            fprintf(stderr, "nc: socket failed: %s\n", strerror(errno));
        }
        return 1;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);

    if (listen_mode) {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(sock, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
            fprintf(stderr, "nc: bind failed: %s\n", strerror(errno));
            close(sock);
            return 1;
        }

        if (!udp_mode) {
            if (listen(sock, 1) < 0) {
                fprintf(stderr, "nc: listen failed: %s\n", strerror(errno));
                close(sock);
                return 1;
            }
            printf("Listening on 0.0.0.0:%d...\n", port);
            struct sockaddr_in client_addr;
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(sock, (struct sockaddr*)&client_addr, &clen);
            if (client_fd < 0) {
                fprintf(stderr, "nc: accept failed: %s\n", strerror(errno));
                close(sock);
                return 1;
            }
            close(sock);
            sock = client_fd;
        }
    } else {
        /* Connect mode */
        struct hostent* he = gethostbyname(host);
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            sin.sin_addr.s_addr = *(uint32_t*)he->h_addr_list[0];
        } else {
            if (inet_pton(AF_INET, host, &sin.sin_addr.s_addr) != 1) {
                fprintf(stderr, "nc: cannot resolve %s\n", host);
                close(sock);
                return 1;
            }
        }

        if (!udp_mode) {
            if (connect(sock, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
                fprintf(stderr, "nc: connect to %s:%d failed: %s\n", host, port, strerror(errno));
                close(sock);
                return 1;
            }
        }
    }

    /* Bi-directional transfer loop */
    char buf[1024];
    while (1) {
        ssize_t n = read(0, buf, sizeof(buf));
        if (n <= 0) break;
        if (udp_mode && !listen_mode) {
            sendto(sock, buf, n, 0, (struct sockaddr*)&sin, sizeof(sin));
        } else {
            (void)write(sock, buf, n);
        }

        /* Check for reply if available */
        ssize_t r = read(sock, buf, sizeof(buf));
        if (r > 0) {
            (void)write(1, buf, r);
        }
    }

    close(sock);
    return 0;
}
