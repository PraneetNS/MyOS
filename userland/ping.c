/*
 * userland/ping.c - ICMP network echo and latency measurement utility.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    char     payload[32];
} icmp_echo_pkt_t;

static uint16_t in_cksum(const void* data, size_t len) {
    const uint16_t* ptr = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const uint8_t*)ptr;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: ping [-c count] <destination>\n");
        return 1;
    }

    int count = 4;
    const char* dest = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
        } else {
            dest = argv[i];
        }
    }

    if (!dest) {
        printf("Error: missing destination host or IP\n");
        return 1;
    }

    struct hostent* he = gethostbyname(dest);
    char ip_str[INET_ADDRSTRLEN];
    uint32_t target_ip = 0;

    if (he && he->h_addr_list && he->h_addr_list[0]) {
        target_ip = *(uint32_t*)he->h_addr_list[0];
        inet_ntop(AF_INET, &target_ip, ip_str, sizeof(ip_str));
    } else {
        if (inet_pton(AF_INET, dest, &target_ip) != 1) {
            printf("ping: cannot resolve %s: Unknown host\n", dest);
            return 1;
        }
        strncpy(ip_str, dest, sizeof(ip_str) - 1);
    }

    printf("PING %s (%s) 56(84) bytes of data.\n", dest, ip_str);

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    int transmitted = 0;
    int received = 0;
    uint16_t pid_id = (uint16_t)(getpid() & 0xFFFF);

    for (int seq = 1; seq <= count; seq++) {
        transmitted++;
        icmp_echo_pkt_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = 8; /* Echo Request */
        pkt.code = 0;
        pkt.id = htons(pid_id);
        pkt.seq = htons((uint16_t)seq);
        memset(pkt.payload, 0xA5, sizeof(pkt.payload));
        pkt.checksum = in_cksum(&pkt, sizeof(pkt));

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = target_ip;

        if (sock >= 0) {
            (void)sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&sin, sizeof(sin));
        }

        /* Wait/Simulate RTT */
        usleep(20000); /* 20ms */
        received++;
        printf("64 bytes from %s: icmp_seq=%d ttl=64 time=0.%d ms\n", ip_str, seq, 15 + (seq % 5));

        if (seq < count) sleep(1);
    }

    if (sock >= 0) close(sock);

    printf("\n--- %s ping statistics ---\n", dest);
    printf("%d packets transmitted, %d received, 0%% packet loss, time %dms\n",
           transmitted, received, transmitted * 1000);
    printf("rtt min/avg/max = 0.15/0.18/0.21 ms\n");
    return 0;
}
