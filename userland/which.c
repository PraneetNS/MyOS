#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int is_executable(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISREG(st.st_mode) || (st.st_mode & 0111)) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: which [-a] filename ...\n");
        return 1;
    }

    int opt_all = 0;
    int arg_start = 1;
    if (strcmp(argv[1], "-a") == 0) {
        opt_all = 1;
        arg_start = 2;
    }

    const char* path_env = getenv("PATH");
    if (!path_env || !*path_env) {
        path_env = "/bin:/";
    }

    int overall_found = 1;

    for (int i = arg_start; i < argc; i++) {
        const char* name = argv[i];
        int found = 0;

        /* If name contains '/', check directly */
        if (strchr(name, '/')) {
            if (is_executable(name)) {
                printf("%s\n", name);
                found = 1;
            }
        } else {
            char path_buf[1024];
            const char* cur = path_env;
            while (*cur) {
                const char* colon = strchr(cur, ':');
                int len = colon ? (int)(colon - cur) : (int)strlen(cur);
                if (len > 0 && len < 900) {
                    char dir[950];
                    memcpy(dir, cur, len);
                    dir[len] = '\0';
                    snprintf(path_buf, sizeof(path_buf), "%s/%s", (strcmp(dir, "/") == 0) ? "" : dir, name);
                    if (is_executable(path_buf)) {
                        printf("%s\n", path_buf);
                        found = 1;
                        if (!opt_all) break;
                    }
                }
                if (!colon) break;
                cur = colon + 1;
            }
        }

        if (!found) {
            overall_found = 0;
        }
    }

    return overall_found ? 0 : 1;
}
