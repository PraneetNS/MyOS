#include "libc.h"

static void heavy_disk_worker(int id) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/tdisk_%d.dat", id);
    char buf[1024];
    memset(buf, 'A' + (id % 26), sizeof(buf));

    while (1) {
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            for (int k = 0; k < 16; k++) {
                write(fd, buf, sizeof(buf));
            }
            close(fd);
        }

        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char rbuf[1024];
            while (read(fd, rbuf, sizeof(rbuf)) > 0) {}
            close(fd);
        }

        unlink(path);
    }
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    printf("[ticktest] Starting 20-second timer tick-loss test under heavy disk workload...\n");

    /* Spawn 4 concurrent heavy disk writer/reader processes */
    int workers[4];
    for (int i = 0; i < 4; i++) {
        workers[i] = fork();
        if (workers[i] == 0) {
            heavy_disk_worker(i);
            exit(0);
        }
    }

    /* Wait for the next RTC second boundary */
    time_t t0 = time(NULL);
    while (time(NULL) == t0) {
        usleep(5000);
    }

    time_t start_rtc = time(NULL);
    unsigned int start_ticks = sys_ticks();

    /* Run for 20 RTC seconds while disk I/O churns */
    while (time(NULL) < start_rtc + 20) {
        /* Do small sleeps to yield CPU and let workers saturate ATA PIO */
        usleep(100000);
    }

    unsigned int end_ticks = sys_ticks();
    time_t end_rtc = time(NULL);

    /* Terminate heavy disk workers */
    for (int i = 0; i < 4; i++) {
        if (workers[i] > 0) {
            kill(workers[i], SIGTERM);
            int st;
            waitpid(workers[i], &st, 0);
            char path[64];
            snprintf(path, sizeof(path), "/tmp/tdisk_%d.dat", i);
            unlink(path);
        }
    }

    unsigned int elapsed_sec = (unsigned int)(end_rtc - start_rtc);
    unsigned int elapsed_ticks = end_ticks - start_ticks;
    unsigned int tps = (elapsed_sec > 0) ? (elapsed_ticks / elapsed_sec) : 0;

    printf("[ticktest] Elapsed: %u RTC seconds, %u timer ticks -> %u ticks/sec\n",
           elapsed_sec, elapsed_ticks, tps);

    if (tps < 98 || tps > 102) {
        fprintf(stderr, "[ticktest] FAIL: systematic tick loss! %u ticks/s (expected 98-102)\n", tps);
        return 1;
    }

    printf("[ticktest] PASS: %u ticks/sec (within [98, 102] window, zero systematic tick loss)\n", tps);
    sync();
    return 0;
}
