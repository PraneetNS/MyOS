#include "libc.h"

#define MAX_MODEL 64
#define TOTAL_OPS 500
#define CHECKPOINT_INTERVAL 25
#define STRESS_ROOT "/lfn_stress"

typedef struct {
    int active;
    int is_dir;
    char path[256];
    char name[128];
    char parent_path[256];
    char content[64];
    int content_len;
} model_entry_t;

static model_entry_t g_model[MAX_MODEL];
static int g_next_id = 1;

static uint32_t g_rng = 0x2468ACE1;

static uint32_t rng_next(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static int rng_range(int min, int max) {
    if (min >= max) return min;
    return min + (int)(rng_next() % (uint32_t)(max - min + 1));
}

static int count_active(int* out_files, int* out_dirs) {
    int f = 0, d = 0;
    for (int i = 0; i < MAX_MODEL; i++) {
        if (g_model[i].active) {
            if (g_model[i].is_dir) d++;
            else f++;
        }
    }
    if (out_files) *out_files = f;
    if (out_dirs) *out_dirs = d;
    return f + d;
}

static int count_children(const char* dirpath) {
    int c = 0;
    for (int i = 0; i < MAX_MODEL; i++) {
        if (g_model[i].active && strcmp(g_model[i].parent_path, dirpath) == 0) {
            c++;
        }
    }
    return c;
}

static int name_exists_in_dir(const char* dirpath, const char* name) {
    for (int i = 0; i < MAX_MODEL; i++) {
        if (g_model[i].active && strcmp(g_model[i].parent_path, dirpath) == 0) {
            if (strcasecmp(g_model[i].name, name) == 0) return 1;
        }
    }
    return 0;
}

static void gen_random_name(char* out, int is_dir) {
    static const char* prefixes[] = {
        "Document", "Archive", "Report", "Spreadsheet", "Config",
        "Backup", "Project", "LogFile", "Payload", "Resource"
    };
    static const char* extensions[] = { ".txt", ".dat", ".log", ".conf", ".bin" };
    static const char special_chars[] = "+,;=[]- ";

    int id = g_next_id++;
    int pidx = rng_range(0, 9);
    int sidx = rng_range(0, 7);
    char sc = special_chars[sidx];

    if (is_dir) {
        snprintf(out, 128, "%s_Folder_%c_%03d_Dir", prefixes[pidx], sc == ' ' ? '_' : sc, id);
    } else {
        int eidx = rng_range(0, 4);
        snprintf(out, 128, "%s_Item_%c_%03d%s", prefixes[pidx], sc == ' ' ? '_' : sc, id, extensions[eidx]);
    }
}

static void safe_copy(char* dst, const char* src, size_t dst_size) {
    if (dst_size == 0) return;
    size_t i = 0;
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void update_children_parent_paths(const char* old_parent, const char* new_parent) {
    for (int i = 0; i < MAX_MODEL; i++) {
        if (!g_model[i].active) continue;
        if (strcmp(g_model[i].parent_path, old_parent) == 0) {
            safe_copy(g_model[i].parent_path, new_parent, sizeof(g_model[i].parent_path));
            char new_child_path[256];
            snprintf(new_child_path, sizeof(new_child_path), "%s/%s", new_parent, g_model[i].name);
            if (g_model[i].is_dir) {
                update_children_parent_paths(g_model[i].path, new_child_path);
            }
            safe_copy(g_model[i].path, new_child_path, sizeof(g_model[i].path));
        }
    }
}

static void verify_checkpoint(int op_num) {
    /* 1. Verify contents and size of every active file */
    for (int i = 0; i < MAX_MODEL; i++) {
        if (!g_model[i].active || g_model[i].is_dir) continue;

        struct stat st;
        if (stat(g_model[i].path, &st) < 0) {
            fprintf(stderr, "[lfnstress] Checkpoint op %d: stat failed for %s\n", op_num, g_model[i].path);
            exit(1);
        }
        if (st.st_size != g_model[i].content_len) {
            fprintf(stderr, "[lfnstress] Checkpoint op %d: size mismatch for %s: got %ld expected %d\n",
                    op_num, g_model[i].path, (long)st.st_size, g_model[i].content_len);
            exit(1);
        }

        int fd = open(g_model[i].path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "[lfnstress] Checkpoint op %d: open failed for %s\n", op_num, g_model[i].path);
            exit(1);
        }
        char buf[128];
        memset(buf, 0, sizeof(buf));
        int nr = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (nr != g_model[i].content_len || strcmp(buf, g_model[i].content) != 0) {
            fprintf(stderr, "[lfnstress] Checkpoint op %d: content mismatch for %s: got '%s' expected '%s'\n",
                    op_num, g_model[i].path, buf, g_model[i].content);
            exit(1);
        }
    }

    /* 2. Verify readdir on STRESS_ROOT and all active subdirectories */
    DIR* d = opendir(STRESS_ROOT);
    if (!d) {
        int err = errno;
        int tfd = open(STRESS_ROOT, O_RDONLY);
        struct stat tst;
        int sret = -1;
        if (tfd >= 0) {
            sret = fstat(tfd, &tst);
            close(tfd);
        }
        fprintf(stderr, "[lfnstress] Checkpoint op %d: opendir %s failed, errno=%d, open_fd=%d, sret=%d, mode=0x%x\n",
                op_num, STRESS_ROOT, err, tfd, sret, (unsigned int)(sret == 0 ? tst.st_mode : 0));
        exit(1);
    }
    struct dirent* de;
    int found_count = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (!name_exists_in_dir(STRESS_ROOT, de->d_name)) {
            fprintf(stderr, "[lfnstress] Checkpoint op %d: unexpected entry '%s' in %s\n", op_num, de->d_name, STRESS_ROOT);
            exit(1);
        }
        found_count++;
    }
    closedir(d);
    int expected_count = count_children(STRESS_ROOT);
    if (found_count != expected_count) {
        fprintf(stderr, "[lfnstress] Checkpoint op %d: count mismatch in %s: got %d expected %d\n",
                op_num, STRESS_ROOT, found_count, expected_count);
        exit(1);
    }

    /* Check each active subdirectory */
    for (int i = 0; i < MAX_MODEL; i++) {
        if (!g_model[i].active || !g_model[i].is_dir) continue;
        d = opendir(g_model[i].path);
        if (!d) {
            fprintf(stderr, "[lfnstress] Checkpoint op %d: opendir failed for %s\n", op_num, g_model[i].path);
            exit(1);
        }
        found_count = 0;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (!name_exists_in_dir(g_model[i].path, de->d_name)) {
                fprintf(stderr, "[lfnstress] Checkpoint op %d: unexpected entry '%s' in %s\n",
                        op_num, de->d_name, g_model[i].path);
                exit(1);
            }
            found_count++;
        }
        closedir(d);
        expected_count = count_children(g_model[i].path);
        if (found_count != expected_count) {
            fprintf(stderr, "[lfnstress] Checkpoint op %d: count mismatch in %s: got %d expected %d\n",
                    op_num, g_model[i].path, found_count, expected_count);
            exit(1);
        }
    }

    int files = 0, dirs = 0;
    count_active(&files, &dirs);
    printf("[lfnstress] Checkpoint op %d/%d PASSED (active: %d files, %d dirs)\n",
           op_num, TOTAL_OPS, files, dirs);
}

static void cleanup_dir_recursive(const char* dirpath) {
    while (1) {
        DIR* d = opendir(dirpath);
        if (!d) return;
        struct dirent* de;
        int found_any = 0;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            found_any = 1;
            char sub[300];
            snprintf(sub, sizeof(sub), "%s/%s", dirpath, de->d_name);
            struct stat st;
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
                cleanup_dir_recursive(sub);
                rmdir(sub);
            } else {
                if (unlink(sub) < 0) {
                    cleanup_dir_recursive(sub);
                    rmdir(sub);
                }
            }
            break;
        }
        closedir(d);
        if (!found_any) break;
    }
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    printf("[lfnstress] Starting 500-op Model-Based LFN Stress Test...\n");

    /* Ensure STRESS_ROOT exists and is clean */
    mkdir(STRESS_ROOT, 0755);
    cleanup_dir_recursive(STRESS_ROOT);

    memset(g_model, 0, sizeof(g_model));

    for (int op = 1; op <= TOTAL_OPS; op++) {
        int files = 0, dirs = 0;
        int total_act = count_active(&files, &dirs);

        /* Choose op type:
         * 0: CREATE file
         * 1: WRITE / overwrite file
         * 2: RENAME (file or dir)
         * 3: UNLINK file
         * 4: MKDIR
         * 5: RMDIR
         */
        int op_type = rng_range(0, 5);

        /* Bias when empty or full */
        if (total_act < 5) {
            op_type = (rng_range(0, 1) == 0) ? 0 : 4; /* CREATE or MKDIR */
        } else if (total_act >= MAX_MODEL - 4) {
            op_type = (files > 0) ? 3 : 5; /* UNLINK or RMDIR */
        }

        if (op_type == 0) {
            /* CREATE file */
            int slot = -1;
            for (int i = 0; i < MAX_MODEL; i++) {
                if (!g_model[i].active) { slot = i; break; }
            }
            if (slot >= 0) {
                /* Pick parent directory: either STRESS_ROOT or an active dir */
                char parent[256];
                strcpy(parent, STRESS_ROOT);
                if (dirs > 0 && rng_range(0, 1) == 1) {
                    int dtarget = rng_range(0, dirs - 1);
                    int dcur = 0;
                    for (int i = 0; i < MAX_MODEL; i++) {
                        if (g_model[i].active && g_model[i].is_dir) {
                            if (dcur == dtarget) {
                                strcpy(parent, g_model[i].path);
                                break;
                            }
                            dcur++;
                        }
                    }
                }

                char name[128];
                do {
                    gen_random_name(name, 0);
                } while (name_exists_in_dir(parent, name));

                char fpath[256];
                snprintf(fpath, sizeof(fpath), "%s/%s", parent, name);

                int fd = open(fpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (fd >= 0) {
                    char payload[64];
                    snprintf(payload, sizeof(payload), "DATA_OP_%d_RND_%lu", op, (unsigned long)(rng_next() % 10000));
                    int plen = strlen(payload);
                    write(fd, payload, plen);
                    close(fd);

                    g_model[slot].active = 1;
                    g_model[slot].is_dir = 0;
                    safe_copy(g_model[slot].path, fpath, sizeof(g_model[slot].path));
                    safe_copy(g_model[slot].name, name, sizeof(g_model[slot].name));
                    safe_copy(g_model[slot].parent_path, parent, sizeof(g_model[slot].parent_path));
                    safe_copy(g_model[slot].content, payload, sizeof(g_model[slot].content));
                    g_model[slot].content_len = plen;
                }
            }
        } else if (op_type == 1) {
            /* WRITE / overwrite */
            if (files > 0) {
                int ftarget = rng_range(0, files - 1);
                int fcur = 0;
                for (int i = 0; i < MAX_MODEL; i++) {
                    if (g_model[i].active && !g_model[i].is_dir) {
                        if (fcur == ftarget) {
                            int fd = open(g_model[i].path, O_WRONLY | O_TRUNC);
                            if (fd >= 0) {
                                char payload[64];
                                snprintf(payload, sizeof(payload), "OVERWRITE_OP_%d_RND_%lu", op, (unsigned long)(rng_next() % 10000));
                                int plen = strlen(payload);
                                write(fd, payload, plen);
                                close(fd);
                                safe_copy(g_model[i].content, payload, sizeof(g_model[i].content));
                                g_model[i].content_len = plen;
                            }
                            break;
                        }
                        fcur++;
                    }
                }
            }
        } else if (op_type == 2) {
            /* RENAME */
            if (total_act > 0) {
                int target = rng_range(0, total_act - 1);
                int cur = 0;
                for (int i = 0; i < MAX_MODEL; i++) {
                    if (!g_model[i].active) continue;
                    if (cur == target) {
                        /* Pick target parent directory */
                        char new_parent[256];
                        strcpy(new_parent, STRESS_ROOT);
                        if (dirs > 0 && rng_range(0, 1) == 1) {
                            int dtarget = rng_range(0, dirs - 1);
                            int dcur = 0;
                            for (int k = 0; k < MAX_MODEL; k++) {
                                if (g_model[k].active && g_model[k].is_dir) {
                                    /* Cannot move a directory inside itself */
                                    if (g_model[i].is_dir && strncmp(g_model[k].path, g_model[i].path, strlen(g_model[i].path)) == 0) {
                                        dcur++;
                                        continue;
                                    }
                                    if (dcur == dtarget) {
                                        strcpy(new_parent, g_model[k].path);
                                        break;
                                    }
                                    dcur++;
                                }
                            }
                        }

                        char new_name[128];
                        do {
                            gen_random_name(new_name, g_model[i].is_dir);
                        } while (name_exists_in_dir(new_parent, new_name));

                        char new_path[256];
                        snprintf(new_path, sizeof(new_path), "%s/%s", new_parent, new_name);

                        if (rename(g_model[i].path, new_path) == 0) {
                            if (g_model[i].is_dir) {
                                update_children_parent_paths(g_model[i].path, new_path);
                            }
                            safe_copy(g_model[i].path, new_path, sizeof(g_model[i].path));
                            safe_copy(g_model[i].name, new_name, sizeof(g_model[i].name));
                            safe_copy(g_model[i].parent_path, new_parent, sizeof(g_model[i].parent_path));
                        }
                        break;
                    }
                    cur++;
                }
            }
        } else if (op_type == 3) {
            /* UNLINK */
            if (files > 0) {
                int ftarget = rng_range(0, files - 1);
                int fcur = 0;
                for (int i = 0; i < MAX_MODEL; i++) {
                    if (g_model[i].active && !g_model[i].is_dir) {
                        if (fcur == ftarget) {
                            if (unlink(g_model[i].path) == 0) {
                                g_model[i].active = 0;
                            }
                            break;
                        }
                        fcur++;
                    }
                }
            }
        } else if (op_type == 4) {
            /* MKDIR */
            int slot = -1;
            for (int i = 0; i < MAX_MODEL; i++) {
                if (!g_model[i].active) { slot = i; break; }
            }
            if (slot >= 0) {
                char parent[256];
                strcpy(parent, STRESS_ROOT);
                if (dirs > 0 && rng_range(0, 1) == 1) {
                    int dtarget = rng_range(0, dirs - 1);
                    int dcur = 0;
                    for (int i = 0; i < MAX_MODEL; i++) {
                        if (g_model[i].active && g_model[i].is_dir) {
                            if (dcur == dtarget) {
                                strcpy(parent, g_model[i].path);
                                break;
                            }
                            dcur++;
                        }
                    }
                }

                char name[128];
                do {
                    gen_random_name(name, 1);
                } while (name_exists_in_dir(parent, name));

                char dpath[256];
                snprintf(dpath, sizeof(dpath), "%s/%s", parent, name);

                if (mkdir(dpath, 0755) == 0) {
                    g_model[slot].active = 1;
                    g_model[slot].is_dir = 1;
                    safe_copy(g_model[slot].path, dpath, sizeof(g_model[slot].path));
                    safe_copy(g_model[slot].name, name, sizeof(g_model[slot].name));
                    safe_copy(g_model[slot].parent_path, parent, sizeof(g_model[slot].parent_path));
                    g_model[slot].content[0] = '\0';
                    g_model[slot].content_len = 0;
                }
            }
        } else if (op_type == 5) {
            /* RMDIR (only empty directories) */
            if (dirs > 0) {
                for (int i = 0; i < MAX_MODEL; i++) {
                    if (g_model[i].active && g_model[i].is_dir) {
                        if (count_children(g_model[i].path) == 0) {
                            if (rmdir(g_model[i].path) == 0) {
                                g_model[i].active = 0;
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (op % CHECKPOINT_INTERVAL == 0) {
            verify_checkpoint(op);
        }
    }

    /* Print final model entries to stdout so host can verify */
    printf("\n=== FINAL MODEL ENTRIES (%d total) ===\n", count_active(NULL, NULL));
    for (int i = 0; i < MAX_MODEL; i++) {
        if (g_model[i].active) {
            printf("[MODEL_ENTRY] %s %s\n", g_model[i].is_dir ? "DIR" : "FILE", g_model[i].name);
        }
    }
    printf("=== END MODEL ENTRIES ===\n");

    printf("[lfnstress] PASS: 500 ops verified against in-memory model!\n");
    return 0;
}
