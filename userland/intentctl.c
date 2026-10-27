#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <intent.h>
#include <json.h>

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <command> [args...]\n", prog);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  list                     List registered skills\n");
    fprintf(stderr, "  call <skill> [args_json] Call a skill with JSON arguments\n");
    fprintf(stderr, "  approvals                List pending skill approvals\n");
    fprintf(stderr, "  approve <id>             Approve pending call\n");
    fprintf(stderr, "  deny <id>                Deny pending call\n");
    fprintf(stderr, "  stats                    Show bus daemon stats\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int fd = intent_connect();
    if (fd < 0) {
        fprintf(stderr, "intentctl: failed to connect to intent bus daemon\n");
        return 1;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "list") == 0) {
        char buf[8192];
        if (intent_list_skills(fd, buf, sizeof(buf)) != 0) {
            fprintf(stderr, "intentctl: failed to list skills\n");
            intent_close(fd);
            return 1;
        }

        json_doc_t doc;
        if (json_parse(&doc, buf, strlen(buf)) != JSON_ERR_OK || !doc.root) {
            printf("%s\n", buf);
            intent_close(fd);
            return 0;
        }

        const json_val_t* arr = json_obj_get_arr(doc.root, "skills");
        size_t count = json_arr_len(arr);
        printf("%-20s %-10s %-10s %s\n", "NAME", "PROVIDER", "SENSITIVE", "DESCRIPTION");
        printf("----------------------------------------------------------------------\n");
        for (size_t i = 0; i < count; i++) {
            const json_val_t* item = json_arr_get(arr, i);
            if (!item) continue;
            const char* name = json_obj_get_str(item, "name");
            const char* desc = json_obj_get_str(item, "description");
            int prov_id = 0;
            json_obj_get_int(item, "provider_agent", &prov_id);
            bool sens = false;
            json_obj_get_bool(item, "sensitive", &sens);

            char prov_str[32];
            if (prov_id > 0) snprintf(prov_str, sizeof(prov_str), "agent %d", prov_id);
            else snprintf(prov_str, sizeof(prov_str), "root");

            printf("%-20s %-10s %-10s %s\n",
                   name ? name : "",
                   prov_str,
                   sens ? "yes" : "no",
                   desc ? desc : "");
        }
    } else if (strcmp(cmd, "call") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s call <skill> [args_json]\n", argv[0]);
            intent_close(fd);
            return 1;
        }
        const char* skill = argv[2];
        const char* args = (argc >= 4) ? argv[3] : "{}";

        char res[8192];
        int rc = intent_call(fd, skill, args, res, sizeof(res), 15000);
        if (rc == 0) {
            printf("%s\n", res);
        } else {
            fprintf(stderr, "Error: %s\n", res[0] ? res : "Call failed");
            intent_close(fd);
            return 1;
        }
    } else if (strcmp(cmd, "approvals") == 0) {
        char buf[8192];
        if (intent_list_approvals(fd, buf, sizeof(buf)) != 0) {
            fprintf(stderr, "intentctl: failed to list approvals\n");
            intent_close(fd);
            return 1;
        }

        json_doc_t doc;
        if (json_parse(&doc, buf, strlen(buf)) != JSON_ERR_OK || !doc.root) {
            printf("%s\n", buf);
            intent_close(fd);
            return 0;
        }

        const json_val_t* arr = json_obj_get_arr(doc.root, "approvals");
        size_t count = json_arr_len(arr);
        printf("%-6s %-20s %-12s %s\n", "ID", "SKILL", "CALLER", "PROVIDER");
        printf("----------------------------------------------------\n");
        for (size_t i = 0; i < count; i++) {
            const json_val_t* item = json_arr_get(arr, i);
            if (!item) continue;
            int id = 0, caller = 0, prov = 0;
            json_obj_get_int(item, "id", &id);
            json_obj_get_int(item, "caller_agent", &caller);
            json_obj_get_int(item, "provider_agent", &prov);
            const char* sk = json_obj_get_str(item, "skill");

            char caller_str[32], prov_str[32];
            snprintf(caller_str, sizeof(caller_str), "agent %d", caller);
            snprintf(prov_str, sizeof(prov_str), "agent %d", prov);

            printf("%-6d %-20s %-12s %s\n", id, sk ? sk : "", caller_str, prov_str);
        }
    } else if (strcmp(cmd, "approve") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s approve <id>\n", argv[0]);
            intent_close(fd);
            return 1;
        }
        int id = atoi(argv[2]);
        char err[128];
        if (intent_resolve_approval(fd, (uint32_t)id, true, err, sizeof(err)) == 0) {
            printf("Approved call #%d\n", id);
        } else {
            fprintf(stderr, "Error approving call #%d: %s\n", id, err[0] ? err : "Failed");
            intent_close(fd);
            return 1;
        }
    } else if (strcmp(cmd, "deny") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s deny <id>\n", argv[0]);
            intent_close(fd);
            return 1;
        }
        int id = atoi(argv[2]);
        char err[128];
        if (intent_resolve_approval(fd, (uint32_t)id, false, err, sizeof(err)) == 0) {
            printf("Denied call #%d\n", id);
        } else {
            fprintf(stderr, "Error denying call #%d: %s\n", id, err[0] ? err : "Failed");
            intent_close(fd);
            return 1;
        }
    } else if (strcmp(cmd, "stats") == 0) {
        char buf[1024];
        if (intent_get_stats(fd, buf, sizeof(buf)) != 0) {
            fprintf(stderr, "intentctl: failed to get stats\n");
            intent_close(fd);
            return 1;
        }
        printf("%s\n", buf);
    } else {
        print_usage(argv[0]);
        intent_close(fd);
        return 1;
    }

    intent_close(fd);
    return 0;
}
