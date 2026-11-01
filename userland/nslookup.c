/*
 * userland/nslookup.c - Query Internet name servers interactively.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: nslookup <host> [server]\n");
        return 1;
    }

    const char* host = argv[1];
    const char* server = (argc >= 3) ? argv[2] : "10.0.2.3";

    printf("Server:   %s\n", server);
    printf("Address:  %s#53\n\n", server);

    struct hostent* he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        printf("** server can't find %s: NXDOMAIN\n", host);
        return 1;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, he->h_addr_list[0], ip_str, sizeof(ip_str));

    printf("Name:     %s\n", he->h_name ? he->h_name : host);
    printf("Address:  %s\n", ip_str);

    int idx = 1;
    while (he->h_addr_list[idx]) {
        inet_ntop(AF_INET, he->h_addr_list[idx], ip_str, sizeof(ip_str));
        printf("Address:  %s\n", ip_str);
        idx++;
    }

    return 0;
}
