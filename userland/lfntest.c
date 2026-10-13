#include "libc.h"

static void test_pass(const char* msg) {
    printf("[lfntest] PASS: %s\n", msg);
}

static void test_fail(const char* msg) {
    fprintf(stderr, "[lfntest] FAIL: %s\n", msg);
    exit(1);
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    printf("[lfntest] Starting FAT16 Long Filename (LFN) test suite...\n");

    /* 1. Read existing long filenames placed by mcopy during disk build */
    DIR* d = opendir("/bin");
    if (!d) test_fail("opendir /bin failed");

    int found_crash = 0;
    int found_mouse = 0;
    int found_screen = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (strcasecmp(de->d_name, "crash_safety_test.elf") == 0) found_crash = 1;
        if (strcasecmp(de->d_name, "mousetest.elf") == 0) found_mouse = 1;
        if (strcasecmp(de->d_name, "screenshot.elf") == 0) found_screen = 1;
    }
    closedir(d);

    if (!found_crash) test_fail("crash_safety_test.elf not found in /bin");
    if (!found_mouse) test_fail("mousetest.elf not found in /bin");
    if (!found_screen) test_fail("screenshot.elf not found in /bin");
    test_pass("Found existing LFN entries (crash_safety_test, mousetest, screenshot) in /bin");

    /* 2. Direct stat & open of existing long filename */
    struct stat st;
    if (stat("/bin/crash_safety_test.elf", &st) < 0) {
        test_fail("stat /bin/crash_safety_test.elf failed");
    }
    if (st.st_size <= 0) test_fail("crash_safety_test.elf size is 0");

    int fd = open("/bin/crash_safety_test.elf", O_RDONLY);
    if (fd < 0) test_fail("open /bin/crash_safety_test.elf failed");
    char elf_hdr[4];
    if (read(fd, elf_hdr, 4) != 4 || elf_hdr[0] != 0x7F || elf_hdr[1] != 'E') {
        test_fail("ELF magic header mismatch");
    }
    close(fd);
    test_pass("Direct lookup, stat, and read of long filename /bin/crash_safety_test.elf");

    /* 3. Create a new file with a long filename (> 11 chars, mixed case, spaces) */
    const char* test_file = "/My Long Filename Document.txt";
    const char* test_payload = "LFN_Payload_Data_12345_ABCDE";
    int plen = strlen(test_payload);

    fd = open(test_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) test_fail("Failed to create file with long filename");
    if (write(fd, test_payload, plen) != plen) test_fail("Write payload failed");
    close(fd);
    test_pass("Created file with long filename 'My Long Filename Document.txt'");

    /* 4. Stat and read back */
    if (stat(test_file, &st) < 0) test_fail("stat created LFN file failed");
    if (st.st_size != plen) test_fail("File size mismatch");

    fd = open(test_file, O_RDONLY);
    if (fd < 0) test_fail("open created LFN file failed");
    char rdata[64];
    int nr = read(fd, rdata, sizeof(rdata) - 1);
    close(fd);
    if (nr != plen) test_fail("Read size mismatch");
    rdata[nr] = '\0';
    if (strcmp(rdata, test_payload) != 0) test_fail("Payload content mismatch");
    test_pass("Verified content and size of newly created long filename");

    /* 5. Verify readdir returns the exact LFN name */
    d = opendir("/");
    if (!d) test_fail("opendir / failed");
    int found_lfn = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, "My Long Filename Document.txt") == 0) {
            found_lfn = 1;
            break;
        }
    }
    closedir(d);
    if (!found_lfn) test_fail("readdir / did not return exact LFN name");
    test_pass("readdir returned exact long filename 'My Long Filename Document.txt'");

    /* 6. Verify 8.3 short alias access */
    int afd = open("/MYLONG~1.TXT", O_RDONLY);
    if (afd >= 0) {
        char abuf[64];
        int anr = read(afd, abuf, sizeof(abuf) - 1);
        close(afd);
        if (anr == plen && strncmp(abuf, test_payload, plen) == 0) {
            test_pass("Accessed long filename via short alias /MYLONG~1.TXT");
        } else {
            test_fail("Short alias read content mismatch");
        }
    } else {
        test_fail("Failed to open via short alias /MYLONG~1.TXT");
    }

    /* 7. Case-only rename */
    const char* lower_file = "/my long filename document.txt";
    if (rename(test_file, lower_file) < 0) {
        test_fail("Case-only rename failed");
    }
    if (stat(lower_file, &st) < 0) test_fail("stat after case-only rename failed");
    test_pass("Case-only rename succeeded");

    /* 8. Full rename to another long name */
    const char* ren_file = "/Completely_Different_Long_Name.dat";
    if (rename(lower_file, ren_file) < 0) {
        test_fail("Full rename to different long name failed");
    }
    if (stat(ren_file, &st) < 0) test_fail("stat after full rename failed");
    if (st.st_size != plen) test_fail("Size changed after rename");
    test_pass("Renamed to 'Completely_Different_Long_Name.dat'");

    /* 9. Create long directory and file inside it */
    const char* long_dir = "/very_long_directory_name_test";
    if (mkdir(long_dir, 0755) < 0) test_fail("mkdir long directory failed");

    char nested_file[128];
    snprintf(nested_file, sizeof(nested_file), "%s/nested_long_file.txt", long_dir);
    fd = open(nested_file, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) test_fail("open nested long file failed");
    write(fd, "nested_data", 11);
    close(fd);

    if (stat(nested_file, &st) < 0 || st.st_size != 11) {
        test_fail("stat nested long file failed");
    }
    test_pass("Created and verified nested file inside long directory");

    /* 10. Unlink and rmdir */
    if (unlink(nested_file) < 0) test_fail("unlink nested file failed");
    if (rmdir(long_dir) < 0) test_fail("rmdir long directory failed");
    if (unlink(ren_file) < 0) test_fail("unlink renamed file failed");
    test_pass("Unlink and rmdir cleaned up all long filename entries");

    /* 11. Check frame leaks across child process lifecycle */
    unsigned int base_child = sys_free_frames();
    pid_t pid = fork();
    if (pid == 0) {
        int cfd = open("/child_temp_lfn_file.txt", O_CREAT | O_WRONLY, 0644);
        if (cfd >= 0) {
            write(cfd, "test", 4);
            close(cfd);
            unlink("/child_temp_lfn_file.txt");
        }
        exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    unsigned int final_child = sys_free_frames();
    if (final_child != base_child) {
        fprintf(stderr, "[lfntest] Child frame leak: base=%u, final=%u\n", base_child, final_child);
        test_fail("Frame leak detected across LFN child process");
    }
    test_pass("Zero frame leaks across LFN process lifecycle");

    printf("[lfntest] ALL LFN TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
