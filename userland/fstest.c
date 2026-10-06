#include "libc.h"

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    sys_write(1, "[fstest] starting fstest suite...\n", 34);

    /* 1. Edge case: open nonexistent file (ENOENT) */
    int fd = open("/nonexistent_xyz.txt", O_RDONLY, 0);
    if (fd < 0) {
        sys_write(1, "[fstest] ENOENT verified\n", 25);
    } else {
        sys_write(1, "[fstest] FAIL: opened nonexistent file\n", 39);
        close(fd);
    }

    /* 2. Edge case: mkdir existing (EEXIST) */
    mkdir("/testdir", 0755);
    int mret = mkdir("/testdir", 0755);
    if (mret < 0) {
        sys_write(1, "[fstest] EEXIST verified\n", 25);
    } else {
        sys_write(1, "[fstest] FAIL: re-created existing dir\n", 39);
    }

    /* 3. Edge case: rmdir non-empty */
    int sfd = open("/testdir/file.txt", O_WRONLY | O_CREAT, 0644);
    if (sfd >= 0) {
        write(sfd, "data", 4);
        close(sfd);
    }
    int rret = rmdir("/testdir");
    if (rret < 0) {
        sys_write(1, "[fstest] ENOTEMPTY verified\n", 28);
    } else {
        sys_write(1, "[fstest] FAIL: removed non-empty dir\n", 37);
    }
    unlink("/testdir/file.txt");
    rmdir("/testdir");

    /* 4. Edge case: bad user pointer to sys_read (EFAULT, no crash) */
    int rfd = open("/fat.txt", O_RDONLY, 0);
    if (rfd >= 0) {
        int pret = sys_read(rfd, (void*)0x100000, 32);
        if (pret < 0) {
            sys_write(1, "[fstest] bad pointer EFAULT verified\n", 37);
        } else {
            sys_write(1, "[fstest] FAIL: read into kernel memory\n", 39);
        }
        close(rfd);
    }

    /* 5. Edge case: read past EOF */
    int efd = open("/fat.txt", O_RDONLY, 0);
    if (efd >= 0) {
        char ebuf[64];
        while (read(efd, ebuf, sizeof(ebuf)) > 0);
        int eof_bytes = read(efd, ebuf, sizeof(ebuf));
        if (eof_bytes == 0) {
            sys_write(1, "[fstest] read past EOF verified\n", 32);
        } else {
            sys_write(1, "[fstest] FAIL: did not return 0 at EOF\n", 39);
        }
        close(efd);
    }

    /* 6. Multi-cluster write and read-back */
    int mcfd = open("/multi.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mcfd >= 0) {
        char cbuf[512];
        for (int chunk = 0; chunk < 12; chunk++) {
            for (int b = 0; b < 512; b++) {
                cbuf[b] = (char)((chunk * 17 + b) & 0xFF);
            }
            write(mcfd, cbuf, 512);
        }
        close(mcfd);

        int mcrfd = open("/multi.bin", O_RDONLY, 0);
        int match = 1;
        if (mcrfd >= 0) {
            for (int chunk = 0; chunk < 12; chunk++) {
                int n = read(mcrfd, cbuf, 512);
                if (n != 512) { match = 0; break; }
                for (int b = 0; b < 512; b++) {
                    if (cbuf[b] != (char)((chunk * 17 + b) & 0xFF)) {
                        match = 0;
                        break;
                    }
                }
                if (!match) break;
            }
            close(mcrfd);
        } else {
            match = 0;
        }
        if (match) {
            sys_write(1, "[fstest] multi-cluster write and read verified\n", 47);
        } else {
            sys_write(1, "[fstest] FAIL: multi-cluster mismatch\n", 38);
        }
    }

    /* 7. Nested directories */
    mkdir("/nest1", 0755);
    mkdir("/nest1/nest2", 0755);
    int nfd = open("/nest1/nest2/test.txt", O_WRONLY | O_CREAT, 0644);
    if (nfd >= 0) {
        write(nfd, "nested_ok\n", 10);
        close(nfd);
    }
    int nrfd = open("/nest1/nest2/test.txt", O_RDONLY, 0);
    if (nrfd >= 0) {
        char nbuf[16];
        int n = read(nrfd, nbuf, 10);
        close(nrfd);
        if (n == 10 && nbuf[0] == 'n' && nbuf[7] == 'o' && nbuf[8] == 'k') {
            sys_write(1, "[fstest] nested dirs verified\n", 30);
        } else {
            sys_write(1, "[fstest] FAIL: nested dirs read\n", 32);
        }
    }

    /* 8. Write persistence file and sync */
    int pfd = open("/persist.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pfd >= 0) {
        write(pfd, "PERSISTENCE_TEST_OK\n", 20);
        close(pfd);
    }
    sys_sync();
    sys_write(1, "[fstest] persistence file created and synced\n", 45);

    sys_write(1, "[fstest] ALL FSTESTS COMPLETED\n", 31);
    return 0;
}
