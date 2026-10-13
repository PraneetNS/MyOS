#include "libc.h"

#define NUM_WORKERS 4
#define ITERATIONS 10

static void worker_run(int worker_id) {
    char dir_lvl1[64];
    char dir_lvl2[64];
    char dir_lvl3[64];
    char dir_lvl4[64];

    snprintf(dir_lvl1, sizeof(dir_lvl1), "/cstress");
    snprintf(dir_lvl2, sizeof(dir_lvl2), "/cstress/w%d", worker_id);
    snprintf(dir_lvl3, sizeof(dir_lvl3), "/cstress/w%d/sub1", worker_id);
    snprintf(dir_lvl4, sizeof(dir_lvl4), "/cstress/w%d/sub1/sub2", worker_id);

    int m1 = mkdir(dir_lvl1, 0755);
    int m2 = mkdir(dir_lvl2, 0755);
    int m3 = mkdir(dir_lvl3, 0755);
    int m4 = mkdir(dir_lvl4, 0755);
    printf("[worker %d] mkdirs: m1=%d m2=%d m3=%d m4=%d (errno=%d)\n", worker_id, m1, m2, m3, m4, errno);

    for (int it = 0; it < ITERATIONS; it++) {
        char old_path[128];
        char new_path[128];
        snprintf(old_path, sizeof(old_path), "%s/f_orig%d.txt", dir_lvl4, it);
        snprintf(new_path, sizeof(new_path), "%s/f_ren%d.txt", dir_lvl4, it);

        /* 1. open() + write() */
        int fd = open(old_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "[worker %d] open failed: %s (errno=%d)\n", worker_id, old_path, errno);
            exit(1);
        }

        char payload[64];
        int plen = snprintf(payload, sizeof(payload), "Worker%d_Iter%d_MagicPayloadData", worker_id, it);
        int nw = write(fd, payload, plen);
        if (nw != plen) {
            fprintf(stderr, "[worker %d] write mismatch: %d != %d\n", worker_id, nw, plen);
            close(fd);
            exit(1);
        }
        close(fd);

        /* 2. stat() on old path */
        struct stat st;
        if (stat(old_path, &st) < 0) {
            fprintf(stderr, "[worker %d] stat old_path failed: %s\n", worker_id, old_path);
            exit(1);
        }
        if (st.st_size != plen) {
            fprintf(stderr, "[worker %d] stat size mismatch: %u != %d\n", worker_id, (unsigned)st.st_size, plen);
            exit(1);
        }

        /* 3. readdir() verifying old entry appears */
        DIR* d = opendir(dir_lvl4);
        if (!d) {
            fprintf(stderr, "[worker %d] opendir failed: %s\n", worker_id, dir_lvl4);
            exit(1);
        }
        int found = 0;
        char exp_name[32];
        snprintf(exp_name, sizeof(exp_name), "f_orig%d.txt", it);
        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            if (strcasecmp(de->d_name, exp_name) == 0) {
                found = 1;
                break;
            }
        }
        closedir(d);
        if (!found) {
            fprintf(stderr, "[worker %d] readdir did not find %s\n", worker_id, exp_name);
            exit(1);
        }

        /* 4. rename() from old_path to new_path */
        if (rename(old_path, new_path) < 0) {
            fprintf(stderr, "[worker %d] rename failed: %s -> %s\n", worker_id, old_path, new_path);
            exit(1);
        }

        /* Verify old_path is gone */
        if (stat(old_path, &st) == 0) {
            fprintf(stderr, "[worker %d] old_path still exists after rename: %s\n", worker_id, old_path);
            exit(1);
        }

        /* 5. stat() on new_path */
        if (stat(new_path, &st) < 0) {
            fprintf(stderr, "[worker %d] stat new_path failed: %s\n", worker_id, new_path);
            exit(1);
        }
        if (st.st_size != plen) {
            fprintf(stderr, "[worker %d] new_path size mismatch: %u != %d\n", worker_id, (unsigned)st.st_size, plen);
            exit(1);
        }

        /* 6. read() from new_path and verify data integrity (no path/data corruption) */
        int rfd = open(new_path, O_RDONLY);
        if (rfd < 0) {
            fprintf(stderr, "[worker %d] open new_path failed: %s\n", worker_id, new_path);
            exit(1);
        }
        char read_buf[64];
        int nr = read(rfd, read_buf, sizeof(read_buf) - 1);
        close(rfd);
        if (nr != plen) {
            fprintf(stderr, "[worker %d] read length mismatch: %d != %d\n", worker_id, nr, plen);
            exit(1);
        }
        read_buf[nr] = '\0';
        if (strcmp(read_buf, payload) != 0) {
            fprintf(stderr, "[worker %d] data corruption! expected '%s' got '%s'\n", worker_id, payload, read_buf);
            exit(1);
        }

        /* 7. unlink() new_path */
        if (unlink(new_path) < 0) {
            fprintf(stderr, "[worker %d] unlink failed: %s (errno=%d)\n", worker_id, new_path, errno);
            exit(1);
        }
        if (stat(new_path, &st) == 0) {
            fprintf(stderr, "[worker %d] new_path still exists after unlink\n", worker_id);
            exit(1);
        }
    }

    /* Clean up directories */
    rmdir(dir_lvl4);
    rmdir(dir_lvl3);
    rmdir(dir_lvl2);

    exit(0);
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    printf("[concur_test] Starting concurrent filesystem stress test...\n");

    /* Warm-up run with 1 worker for 1 iteration to fault in heap and stack pages */
    {
        int p = fork();
        if (p == 0) {
            worker_run(99);
        }
        int st = 0;
        waitpid(p, &st, 0);
        rmdir("/cstress");
    }

    unsigned int base_frames = sys_free_frames();
    printf("[concur_test] Baseline free frames: %u\n", base_frames);

    /* Ensure root test directory exists */
    mkdir("/cstress", 0755);

    /* 1. Spawn spinner process to keep CPU busy and preemption firing at 100 Hz */
    int spin_pid = fork();
    if (spin_pid == 0) {
        volatile unsigned int counter = 0;
        while (1) {
            counter++;
        }
    }

    /* 2. Spawn NUM_WORKERS concurrent worker processes */
    int worker_pids[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_pids[i] = fork();
        if (worker_pids[i] < 0) {
            fprintf(stderr, "[concur_test] fork worker %d failed\n", i);
            kill(spin_pid, SIGTERM);
            exit(1);
        }
        if (worker_pids[i] == 0) {
            /* Child worker */
            worker_run(i);
        }
    }

    /* 3. Wait for all workers to finish */
    int failures = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        int status = 0;
        int reaped = waitpid(worker_pids[i], &status, 0);
        if (reaped != worker_pids[i]) {
            fprintf(stderr, "[concur_test] waitpid failed for worker %d\n", i);
            failures++;
        } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "[concur_test] worker %d exited with status %d\n", i, WEXITSTATUS(status));
            failures++;
        }
    }

    /* 4. Terminate spinner process */
    kill(spin_pid, SIGTERM);
    int spin_status = 0;
    waitpid(spin_pid, &spin_status, 0);

    /* Clean up top-level test directory */
    rmdir("/cstress");

    if (failures > 0) {
        fprintf(stderr, "[concur_test] FAILED with %d worker error(s)!\n", failures);
        exit(1);
    }

    unsigned int final_frames = sys_free_frames();
    printf("[concur_test] Final free frames: %u\n", final_frames);

    if (final_frames != base_frames) {
        fprintf(stderr, "[concur_test] FRAME LEAK DETECTED: base=%u, final=%u (diff=%d)\n",
                base_frames, final_frames, (int)(base_frames - final_frames));
        exit(1);
    }

    printf("[concur_test] PASS: 4 concurrent workers + spinner, no path corruption, zero leaks!\n");
    return 0;
}
