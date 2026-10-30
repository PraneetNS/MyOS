/* memctl - human admin tool for memd. Talks to mem.admin, which rejects agent callers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intent.h>
#include <json.h>

static void usage(void) {
    printf("Usage: memctl <cmd> <principal> [args] [--ns <ns>]\n"
           "  list <p>                 List memories\n"
           "  search <p> <query>       Search memories\n"
           "  get <p> <id>             Show one memory\n"
           "  put <p> <text>           Add a user-trusted memory\n"
           "  delete <p> <id>          Physically delete a memory\n"
           "  mark-trusted <p> <id>    Promote an untrusted memory to user-trusted\n"
           "  wipe <p>                 Delete every memory of a principal\n"
           "  stats <p>                Entry counts\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    const char* cmd = argv[1];
    const char* principal = argv[2];
    const char* ns = "priv";
    const char* extra = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--ns") == 0 && i + 1 < argc) ns = argv[++i];
        else if (!extra) extra = argv[i];
    }

    int fd = intent_connect();
    if (fd < 0) { fprintf(stderr, "memctl: cannot connect to intent bus\n"); return 1; }

    char args[1024];
    json_buf_t jb;
    json_buf_init(&jb, args, sizeof(args));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "op");
    json_buf_append_str(&jb, cmd);
    json_buf_obj_key(&jb, "principal");
    json_buf_append_str(&jb, principal);
    json_buf_obj_key(&jb, "ns");
    json_buf_append_str(&jb, ns);
    if (extra) {
        if (strcmp(cmd, "search") == 0) { json_buf_obj_key(&jb, "query"); json_buf_append_str(&jb, extra); }
        else if (strcmp(cmd, "put") == 0) { json_buf_obj_key(&jb, "text"); json_buf_append_str(&jb, extra); }
        else { json_buf_obj_key(&jb, "id"); json_buf_append_int(&jb, atoi(extra)); }
    }
    json_buf_obj_end(&jb);

    static char res[8192];
    int rc = intent_call(fd, "mem.admin", args, res, sizeof(res), 5000);
    intent_close(fd);
    if (rc != 0) { fprintf(stderr, "memctl: error: %s\n", res); return 1; }
    printf("%s\n", res);
    return 0;
}

