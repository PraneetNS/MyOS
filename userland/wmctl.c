/*
 * wmctl.c - MyOS Window Manager Control CLI
 *
 * Usage:
 *   wmctl list
 *   wmctl focus <id>
 *   wmctl close <id>
 *   wmctl move <id> <x> <y>
 *   wmctl minimize <id>
 *   wmctl maximize <id>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define WM_SOCK_PATH "/run/wm.sock"

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <command> [args...]\n", prog);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  list                 List all windows\n");
    fprintf(stderr, "  focus <id>           Set focus and raise window\n");
    fprintf(stderr, "  close <id>           Request window to close\n");
    fprintf(stderr, "  move <id> <x> <y>    Move window to (x, y)\n");
    fprintf(stderr, "  minimize <id>        Minimize window\n");
    fprintf(stderr, "  maximize <id>        Toggle/set maximize on window\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* cmd = argv[1];
    char req[256];
    memset(req, 0, sizeof(req));

    if (strcmp(cmd, "list") == 0) {
        snprintf(req, sizeof(req), "LIST\n");
    } else if (strcmp(cmd, "focus") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: focus requires <id>\n");
            return 1;
        }
        snprintf(req, sizeof(req), "FOCUS %s\n", argv[2]);
    } else if (strcmp(cmd, "close") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: close requires <id>\n");
            return 1;
        }
        snprintf(req, sizeof(req), "CLOSE %s\n", argv[2]);
    } else if (strcmp(cmd, "move") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Error: move requires <id> <x> <y>\n");
            return 1;
        }
        snprintf(req, sizeof(req), "MOVE %s %s %s\n", argv[2], argv[3], argv[4]);
    } else if (strcmp(cmd, "minimize") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: minimize requires <id>\n");
            return 1;
        }
        snprintf(req, sizeof(req), "MINIMIZE %s\n", argv[2]);
    } else if (strcmp(cmd, "maximize") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: maximize requires <id>\n");
            return 1;
        }
        snprintf(req, sizeof(req), "MAXIMIZE %s\n", argv[2]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage(argv[0]);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("wmctl: socket");
        return 1;
    }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, WM_SOCK_PATH, sizeof(sun.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        fprintf(stderr, "wmctl: cannot connect to %s (is wm running?)\n", WM_SOCK_PATH);
        close(fd);
        return 1;
    }

    size_t req_len = strlen(req);
    size_t written = 0;
    while (written < req_len) {
        int nw = write(fd, req + written, req_len - written);
        if (nw <= 0) {
            if (nw < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            perror("wmctl: write");
            close(fd);
            return 1;
        }
        written += nw;
    }

    char buf[1024];
    int has_error = 0;
    int total_read = 0;

    while (1) {
        int nr = read(fd, buf, sizeof(buf) - 1);
        if (nr <= 0) {
            if (nr < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }
        buf[nr] = '\0';
        total_read += nr;
        if (strncmp(buf, "ERROR", 5) == 0) {
            has_error = 1;
        }
        fputs(buf, stdout);
        fflush(stdout);
        if (strstr(buf, "OK\n") || strstr(buf, "ERROR")) {
            break;
        }
    }

    close(fd);

    if (total_read == 0 || has_error) {
        return 1;
    }
    return 0;
}
