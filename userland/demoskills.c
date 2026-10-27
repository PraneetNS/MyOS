#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <intent.h>
#include <json.h>

static int handle_fs_list(const char* skill, const char* args_json,
                          char* result_json, size_t result_cap,
                          char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;
    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK || !doc.root) {
        snprintf(err_buf, err_cap, "Failed to parse args JSON");
        return -1;
    }

    const char* path = json_obj_get_str(doc.root, "path");
    if (!path) path = ".";

    DIR* dir = opendir(path);
    if (!dir) {
        snprintf(err_buf, err_cap, "Cannot open directory '%s': %s", path, strerror(errno));
        return -1;
    }

    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "entries");
    json_buf_arr_start(&jb);

    struct dirent* de;
    int first = 1;
    while ((de = readdir(dir)) != NULL) {
        if (!first) json_buf_append_raw(&jb, ",", 1);
        first = 0;
        json_buf_append_str(&jb, de->d_name);
    }
    closedir(dir);

    json_buf_arr_end(&jb);
    json_buf_obj_end(&jb);

    if (jb.error) {
        snprintf(err_buf, err_cap, "Result buffer too small");
        return -1;
    }
    return 0;
}

static int handle_fs_read(const char* skill, const char* args_json,
                          char* result_json, size_t result_cap,
                          char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;
    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK || !doc.root) {
        snprintf(err_buf, err_cap, "Failed to parse args JSON");
        return -1;
    }

    const char* path = json_obj_get_str(doc.root, "path");
    if (!path) {
        snprintf(err_buf, err_cap, "Missing required argument 'path'");
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err_buf, err_cap, "Failed to open '%s': %s", path, strerror(errno));
        return -1;
    }

    char content[4096];
    ssize_t n = read(fd, content, sizeof(content) - 1);
    close(fd);
    if (n < 0) {
        snprintf(err_buf, err_cap, "Failed to read '%s': %s", path, strerror(errno));
        return -1;
    }
    content[n] = '\0';

    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "content");
    json_buf_append_str_n(&jb, content, (size_t)n);
    json_buf_obj_end(&jb);

    if (jb.error) {
        snprintf(err_buf, err_cap, "Result buffer too small");
        return -1;
    }
    return 0;
}

static int handle_sys_meminfo(const char* skill, const char* args_json,
                              char* result_json, size_t result_cap,
                              char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)args_json; (void)udata;
    int fd = open("/proc/meminfo", O_RDONLY);
    char buf[1024];
    buf[0] = '\0';
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n >= 0) buf[n] = '\0';
    }

    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "meminfo");
    json_buf_append_str(&jb, buf[0] ? buf : "MemTotal: 65536 kB\nMemFree: 32768 kB\n");
    json_buf_obj_end(&jb);

    if (jb.error) {
        snprintf(err_buf, err_cap, "Result buffer too small");
        return -1;
    }
    return 0;
}

static int handle_fs_delete(const char* skill, const char* args_json,
                            char* result_json, size_t result_cap,
                            char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;
    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK || !doc.root) {
        snprintf(err_buf, err_cap, "Failed to parse args JSON");
        return -1;
    }

    const char* path = json_obj_get_str(doc.root, "path");
    if (!path) {
        snprintf(err_buf, err_cap, "Missing required argument 'path'");
        return -1;
    }

    if (unlink(path) < 0) {
        snprintf(err_buf, err_cap, "Failed to delete '%s': %s", path, strerror(errno));
        return -1;
    }

    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "deleted");
    json_buf_append_bool(&jb, true);
    json_buf_obj_key(&jb, "path");
    json_buf_append_str(&jb, path);
    json_buf_obj_end(&jb);

    if (jb.error) {
        snprintf(err_buf, err_cap, "Result buffer too small");
        return -1;
    }
    return 0;
}

static const char* s_path_schema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
static const char* s_empty_schema = "{\"type\":\"object\"}";

int main(int argc, char** argv) {
    const char* mode = (argc >= 2) ? argv[1] : "all";

    int fd = intent_connect();
    if (fd < 0) {
        fprintf(stderr, "demoskills: failed to connect to intent bus\n");
        return 1;
    }

    if (strcmp(mode, "fs") == 0 || strcmp(mode, "all") == 0) {
        if (intent_register_skill(fd, "fs.list", "List directory entries",
                                  s_path_schema, NULL, false, handle_fs_list, NULL) != 0) {
            fprintf(stderr, "demoskills: failed to register fs.list\n");
        }
        if (intent_register_skill(fd, "fs.read", "Read file contents",
                                  s_path_schema, NULL, false, handle_fs_read, NULL) != 0) {
            fprintf(stderr, "demoskills: failed to register fs.read\n");
        }
    }

    if (strcmp(mode, "sys") == 0 || strcmp(mode, "all") == 0) {
        if (intent_register_skill(fd, "sys.meminfo", "System memory statistics",
                                  s_empty_schema, NULL, false, handle_sys_meminfo, NULL) != 0) {
            fprintf(stderr, "demoskills: failed to register sys.meminfo\n");
        }
    }

    if (strcmp(mode, "delete") == 0 || strcmp(mode, "all") == 0) {
        if (intent_register_skill(fd, "fs.delete", "Delete a file (sensitive)",
                                  s_path_schema, NULL, true, handle_fs_delete, NULL) != 0) {
            fprintf(stderr, "demoskills: failed to register fs.delete\n");
        }
    }

    printf("demoskills: provider serving mode '%s'\n", mode);
    fflush(stdout);

    intent_serve(fd);
    intent_close(fd);
    return 0;
}
