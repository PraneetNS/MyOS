#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: dirname PATH\n");
        return 1;
    }

    char buf[1024];
    strncpy(buf, argv[1], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int len = strlen(buf);
    if (len == 0) {
        printf(".\n");
        return 0;
    }

    /* Strip trailing slashes */
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    /* If string is just "/", print "/" */
    if (len == 1 && buf[0] == '/') {
        printf("/\n");
        return 0;
    }

    /* Find last slash */
    char* last_slash = strrchr(buf, '/');
    if (!last_slash) {
        printf(".\n");
        return 0;
    }

    if (last_slash == buf) {
        /* Leading slash only, e.g. /usr -> / */
        printf("/\n");
        return 0;
    }

    /* Strip further trailing slashes before last component */
    while (last_slash > buf && *(last_slash - 1) == '/') {
        last_slash--;
    }
    *last_slash = '\0';

    printf("%s\n", buf);
    return 0;
}
