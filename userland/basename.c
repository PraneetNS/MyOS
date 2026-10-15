#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: basename NAME [SUFFIX]\n");
        return 1;
    }

    char buf[1024];
    strncpy(buf, argv[1], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip trailing slashes, but keep single slash */
    int len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    /* Find last slash */
    char* base = strrchr(buf, '/');
    if (base && len > 1) {
        base++;
    } else if (!base) {
        base = buf;
    }

    /* If suffix provided, strip it if matching */
    if (argc == 3) {
        const char* sfx = argv[2];
        int blen = strlen(base);
        int slen = strlen(sfx);
        if (blen > slen && strcmp(base + blen - slen, sfx) == 0) {
            base[blen - slen] = '\0';
        }
    }

    printf("%s\n", base);
    return 0;
}
