#include "libc.h"

static void test_pass(const char* msg) {
    printf("[lfntest] PASS: %s\n", msg);
}

static void test_fail(const char* msg) {
    fprintf(stderr, "[lfntest] FAIL: %s\n", msg);
    exit(1);
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

    /* ==============================================================
     * 11. Alias Collision Test (30 files sharing first 6 characters)
     * ============================================================== */
    printf("[lfntest] Running Test 11: 30-file alias collision & exact resolution...\n");
    cleanup_dir_recursive("/col_test");
    rmdir("/col_test");
    if (mkdir("/col_test", 0755) < 0) test_fail("mkdir /col_test failed");

    char fname[64];
    char falias[64];
    char fbuf[64];

    /* Create 30 files: longname_one_01.txt ... longname_one_30.txt sharing first 6 chars "longna" */
    for (int i = 1; i <= 30; i++) {
        snprintf(fname, sizeof(fname), "/col_test/longname_one_%02d.txt", i);
        fd = open(fname, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "Failed to create %s\n", fname);
            test_fail("Collision test file creation failed");
        }
        snprintf(fbuf, sizeof(fbuf), "PAYLOAD_FILE_%02d", i);
        write(fd, fbuf, strlen(fbuf));
        close(fd);
    }

    /* Verify every file reads back its OWN content and only resolves to itself */
    for (int i = 1; i <= 30; i++) {
        snprintf(fname, sizeof(fname), "/col_test/longname_one_%02d.txt", i);
        fd = open(fname, O_RDONLY);
        if (fd < 0) test_fail("Failed to open collision test file by LFN");
        memset(fbuf, 0, sizeof(fbuf));
        read(fd, fbuf, sizeof(fbuf) - 1);
        close(fd);

        char expected[64];
        snprintf(expected, sizeof(expected), "PAYLOAD_FILE_%02d", i);
        if (strcmp(fbuf, expected) != 0) {
            fprintf(stderr, "File %s content mismatch: got '%s' expected '%s'\n", fname, fbuf, expected);
            test_fail("Collision file read wrong content");
        }

        /* Check short alias: LONGNA~1..9, then LONGN~10..30 */
        if (i <= 9) {
            snprintf(falias, sizeof(falias), "/col_test/LONGNA~%d.TXT", i);
        } else {
            snprintf(falias, sizeof(falias), "/col_test/LONGN~%d.TXT", i);
        }

        fd = open(falias, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "Failed to open alias %s for file %s\n", falias, fname);
            test_fail("Failed to open unique short alias");
        }
        memset(fbuf, 0, sizeof(fbuf));
        read(fd, fbuf, sizeof(fbuf) - 1);
        close(fd);
        if (strcmp(fbuf, expected) != 0) {
            fprintf(stderr, "Alias %s returned '%s' instead of '%s'\n", falias, fbuf, expected);
            test_fail("Alias returned wrong file content");
        }

        /* Cross-check: looking up alias of next file must return that other file's content, never this one */
        int other_idx = (i % 30) + 1;
        char other_alias[64];
        if (other_idx <= 9) {
            snprintf(other_alias, sizeof(other_alias), "/col_test/LONGNA~%d.TXT", other_idx);
        } else {
            snprintf(other_alias, sizeof(other_alias), "/col_test/LONGN~%d.TXT", other_idx);
        }
        int ofd = open(other_alias, O_RDONLY);
        if (ofd >= 0) {
            char obuf[64];
            memset(obuf, 0, sizeof(obuf));
            read(ofd, obuf, sizeof(obuf) - 1);
            close(ofd);
            if (strcmp(obuf, expected) == 0) {
                test_fail("Looking up another file's alias returned this file's content");
            }
        }
    }

    /* Verify lookup never guesses aliases: nonexistent alias must fail */
    if (open("/col_test/LONGNA~99.TXT", O_RDONLY) >= 0) {
        test_fail("Nonexistent alias unexpectedly opened");
    }
    if (open("/col_test/LONGN~99.TXT", O_RDONLY) >= 0) {
        test_fail("Nonexistent alias unexpectedly opened");
    }
    if (open("/col_test/LONGNA~0.TXT", O_RDONLY) >= 0) {
        test_fail("Nonexistent alias unexpectedly opened");
    }
    if (open("/col_test/longname_one_99.txt", O_RDONLY) >= 0) {
        test_fail("Nonexistent LFN unexpectedly opened");
    }

    /* Clean up collision test */
    for (int i = 1; i <= 30; i++) {
        snprintf(fname, sizeof(fname), "/col_test/longname_one_%02d.txt", i);
        unlink(fname);
    }
    rmdir("/col_test");
    test_pass("30-file alias collision, unique aliases, and exact resolution verified");

    /* ==============================================================
     * 12. Directory Growth Test (150 long-named files, multiple clusters)
     * ============================================================== */
    printf("[lfntest] Running Test 12: Directory growth (150 files across multiple clusters)...\n");
    cleanup_dir_recursive("/growth_test");
    rmdir("/growth_test");
    if (mkdir("/growth_test", 0755) < 0) test_fail("mkdir /growth_test failed");

    struct {
        char name[80];
        char content[32];
        int present;
    } model[150];

    char gpath[128];
    for (int i = 0; i < 150; i++) {
        snprintf(model[i].name, sizeof(model[i].name), "growth_file_%03d_cluster_expansion.dat", i);
        snprintf(model[i].content, sizeof(model[i].content), "GROWTH_DATA_%03d", i);
        model[i].present = 1;

        snprintf(gpath, sizeof(gpath), "/growth_test/%s", model[i].name);
        fd = open(gpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "Failed creating %s (i=%d)\n", gpath, i);
            test_fail("Directory growth creation failed");
        }
        write(fd, model[i].content, strlen(model[i].content));
        close(fd);
    }

    /* Verify all 150 files returned by readdir against in-memory model */
    d = opendir("/growth_test");
    if (!d) test_fail("opendir /growth_test failed");
    int count = 0;
    int seen[150];
    memset(seen, 0, sizeof(seen));
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        int found_idx = -1;
        for (int k = 0; k < 150; k++) {
            if (model[k].present && strcmp(de->d_name, model[k].name) == 0) {
                found_idx = k;
                break;
            }
        }
        if (found_idx < 0) test_fail("readdir returned entry not in model");
        if (seen[found_idx]) test_fail("Duplicate entry returned by readdir");
        seen[found_idx] = 1;
        count++;
    }
    closedir(d);
    if (count != 150) {
        fprintf(stderr, "Directory growth readdir count mismatch: expected 150, got %d\n", count);
        test_fail("Readdir count mismatch on 150 files");
    }

    /* Delete every other file (even indices: 0, 2, ..., 148 -> 75 deleted) */
    for (int i = 0; i < 150; i += 2) {
        snprintf(gpath, sizeof(gpath), "/growth_test/%s", model[i].name);
        if (unlink(gpath) < 0) test_fail("Failed unlinking growth file");
        model[i].present = 0;
    }

    /* Verify 75 files remain by checking readdir against model */
    d = opendir("/growth_test");
    if (!d) test_fail("opendir /growth_test after delete failed");
    count = 0;
    memset(seen, 0, sizeof(seen));
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        int found_idx = -1;
        for (int k = 0; k < 150; k++) {
            if (model[k].present && strcmp(de->d_name, model[k].name) == 0) {
                found_idx = k;
                break;
            }
        }
        if (found_idx < 0) test_fail("Deleted entry still returned by readdir");
        seen[found_idx] = 1;
        count++;
    }
    closedir(d);
    if (count != 75) {
        fprintf(stderr, "Directory readdir count after deleting half: expected 75, got %d\n", count);
        test_fail("Readdir count mismatch after deleting half");
    }

    /* Recreate the deleted 75 files with new payload */
    for (int i = 0; i < 150; i += 2) {
        snprintf(model[i].content, sizeof(model[i].content), "RECREATED_DATA_%03d", i);
        model[i].present = 1;
        snprintf(gpath, sizeof(gpath), "/growth_test/%s", model[i].name);
        fd = open(gpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) test_fail("Failed recreating growth file");
        write(fd, model[i].content, strlen(model[i].content));
        close(fd);
    }

    /* Verify all 150 files exist and contents match model */
    d = opendir("/growth_test");
    if (!d) test_fail("opendir /growth_test after recreate failed");
    count = 0;
    memset(seen, 0, sizeof(seen));
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        int found_idx = -1;
        for (int k = 0; k < 150; k++) {
            if (model[k].present && strcmp(de->d_name, model[k].name) == 0) {
                found_idx = k;
                break;
            }
        }
        if (found_idx < 0) test_fail("Entry not found in model after recreate");
        seen[found_idx] = 1;
        count++;
    }
    closedir(d);
    if (count != 150) test_fail("Readdir count mismatch after recreate");

    for (int i = 0; i < 150; i++) {
        snprintf(gpath, sizeof(gpath), "/growth_test/%s", model[i].name);
        fd = open(gpath, O_RDONLY);
        if (fd < 0) test_fail("Failed opening recreated growth file");
        memset(fbuf, 0, sizeof(fbuf));
        read(fd, fbuf, sizeof(fbuf) - 1);
        close(fd);

        if (strcmp(fbuf, model[i].content) != 0) {
            fprintf(stderr, "Growth file %s content mismatch: got '%s' expected '%s'\n", gpath, fbuf, model[i].content);
            test_fail("Growth file content mismatch");
        }
    }

    /* Clean up 150 files */
    for (int i = 0; i < 150; i++) {
        snprintf(gpath, sizeof(gpath), "/growth_test/%s", model[i].name);
        unlink(gpath);
    }
    rmdir("/growth_test");
    test_pass("Directory growth across clusters (150 files, half delete, recreate) verified");

    /* ==============================================================
     * 13. Names & Validation Test
     * ============================================================== */
    printf("[lfntest] Running Test 13: Names (255/256-char, UTF-8, special chars, .hidden, trailing rejection, case collision)...\n");

    /* 255-character filename (must succeed) */
    char name_255[300];
    memset(name_255, 'a', 251);
    memcpy(name_255 + 251, ".txt", 5); /* 251 + 4 = 255 chars, + '\0' */
    char path_255[320];
    snprintf(path_255, sizeof(path_255), "/%s", name_255);

    fd = open(path_255, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) test_fail("255-character filename creation failed");
    write(fd, "255_OK", 6);
    close(fd);

    fd = open(path_255, O_RDONLY);
    if (fd < 0) test_fail("255-character filename read open failed");
    memset(fbuf, 0, sizeof(fbuf));
    read(fd, fbuf, sizeof(fbuf) - 1);
    close(fd);
    if (strcmp(fbuf, "255_OK") != 0) test_fail("255-character file content mismatch");
    unlink(path_255);
    test_pass("255-character filename succeeded");

    /* 256-character filename (must fail with ENAMETOOLONG) */
    char name_256[300];
    memset(name_256, 'b', 252);
    memcpy(name_256 + 252, ".txt", 5); /* 252 + 4 = 256 chars */
    char path_256[320];
    snprintf(path_256, sizeof(path_256), "/%s", name_256);

    fd = open(path_256, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
        unlink(path_256);
        test_fail("256-character filename unexpectedly succeeded");
    }
    if (errno != ENAMETOOLONG && errno != 36) {
        fprintf(stderr, "256-char error was %d instead of ENAMETOOLONG (%d or 36)\n", errno, ENAMETOOLONG);
        test_fail("256-char filename did not return ENAMETOOLONG");
    }
    test_pass("256-character filename rejected with -ENAMETOOLONG");

    /* UTF-8 filenames: café.txt, नमस्ते.txt, 日本語.txt */
    const char* utf8_names[] = { "/café.txt", "/नमस्ते.txt", "/日本語.txt" };
    for (int i = 0; i < 3; i++) {
        fd = open(utf8_names[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "Failed creating UTF-8 file %s\n", utf8_names[i]);
            test_fail("UTF-8 file creation failed");
        }
        snprintf(fbuf, sizeof(fbuf), "UTF8_PAYLOAD_%d", i);
        write(fd, fbuf, strlen(fbuf));
        close(fd);

        fd = open(utf8_names[i], O_RDONLY);
        if (fd < 0) test_fail("Failed opening UTF-8 file");
        memset(rdata, 0, sizeof(rdata));
        read(fd, rdata, sizeof(rdata) - 1);
        close(fd);
        if (strcmp(rdata, fbuf) != 0) test_fail("UTF-8 file read content mismatch");
        unlink(utf8_names[i]);
    }
    test_pass("UTF-8 filenames (café.txt, नमस्ते.txt, 日本語.txt) verified");

    /* Legal-in-LFN characters: (+ , ; = [ ]) */
    const char* legal_lfn = "/test+file,with;semi=and[brackets].txt";
    fd = open(legal_lfn, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) test_fail("Creating legal-in-LFN-only filename failed");
    write(fd, "LEGAL_CHARS", 11);
    close(fd);
    fd = open(legal_lfn, O_RDONLY);
    if (fd < 0) test_fail("Opening legal-in-LFN-only filename failed");
    memset(fbuf, 0, sizeof(fbuf));
    read(fd, fbuf, sizeof(fbuf) - 1);
    close(fd);
    if (strcmp(fbuf, "LEGAL_CHARS") != 0) test_fail("Legal LFN content mismatch");
    unlink(legal_lfn);
    test_pass("Legal-in-LFN characters (+ , ; = [ ]) verified");

    /* .hidden file */
    const char* hidden_file = "/.hidden";
    fd = open(hidden_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) test_fail("Creating .hidden file failed");
    write(fd, "HIDDEN_SECRET", 13);
    close(fd);
    fd = open(hidden_file, O_RDONLY);
    if (fd < 0) test_fail("Opening .hidden file failed");
    memset(fbuf, 0, sizeof(fbuf));
    read(fd, fbuf, sizeof(fbuf) - 1);
    close(fd);
    if (strcmp(fbuf, "HIDDEN_SECRET") != 0) test_fail(".hidden file content mismatch");
    unlink(hidden_file);
    test_pass("Leading dot (.hidden) verified");

    /* Names with trailing dot or space (must be rejected with -EINVAL) */
    if (open("/trailing_dot.", O_CREAT | O_WRONLY, 0644) >= 0) {
        unlink("/trailing_dot.");
        test_fail("Trailing dot filename unexpectedly succeeded");
    }
    if (errno != EINVAL) test_fail("Trailing dot did not return EINVAL");

    if (open("/trailing_space ", O_CREAT | O_WRONLY, 0644) >= 0) {
        unlink("/trailing_space ");
        test_fail("Trailing space filename unexpectedly succeeded");
    }
    if (errno != EINVAL) test_fail("Trailing space did not return EINVAL");

    if (mkdir("/trailing_dir.", 0755) >= 0) {
        rmdir("/trailing_dir.");
        test_fail("Trailing dot directory unexpectedly succeeded");
    }
    if (errno != EINVAL) test_fail("Trailing dot dir did not return EINVAL");

    if (mkdir("/trailing_dir ", 0755) >= 0) {
        rmdir("/trailing_dir ");
        test_fail("Trailing space directory unexpectedly succeeded");
    }
    if (errno != EINVAL) test_fail("Trailing space dir did not return EINVAL");

    test_pass("Trailing dot and space filenames rejected with -EINVAL");

    /* Case collision: UPPER.TXT vs upper.txt (-EEXIST) */
    fd = open("/UPPER.TXT", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) test_fail("Failed creating /UPPER.TXT");
    close(fd);
    if (open("/upper.txt", O_CREAT | O_EXCL | O_WRONLY, 0644) >= 0) {
        unlink("/upper.txt");
        unlink("/UPPER.TXT");
        test_fail("open upper.txt with O_CREAT|O_EXCL succeeded while UPPER.TXT exists");
    }
    if (errno != EEXIST) {
        unlink("/UPPER.TXT");
        test_fail("Case collision did not set errno to EEXIST");
    }
    unlink("/UPPER.TXT");

    fd = open("/upper.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) test_fail("Failed creating /upper.txt");
    close(fd);
    if (open("/UPPER.TXT", O_CREAT | O_EXCL | O_WRONLY, 0644) >= 0) {
        unlink("/upper.txt");
        unlink("/UPPER.TXT");
        test_fail("open UPPER.TXT with O_CREAT|O_EXCL succeeded while upper.txt exists");
    }
    if (errno != EEXIST) {
        unlink("/upper.txt");
        test_fail("Case collision did not set errno to EEXIST");
    }
    unlink("/upper.txt");
    test_pass("Case collision (UPPER.TXT vs upper.txt) returns -EEXIST");

    /* ==============================================================
     * 14. Rename Matrix
     * ============================================================== */
    printf("[lfntest] Running Test 14: Rename matrix (short<->long, case-only, cross-dir, replace, dir .. fix, no orphans)...\n");
    cleanup_dir_recursive("/rdir1");
    rmdir("/rdir1");
    cleanup_dir_recursive("/rdir2");
    rmdir("/rdir2");

    /* short <-> long rename */
    fd = open("/short.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "SHORT_DATA", 10);
    close(fd);
    if (rename("/short.txt", "/this_is_a_very_long_file_name_now.txt") < 0) {
        test_fail("short->long rename failed");
    }
    fd = open("/this_is_a_very_long_file_name_now.txt", O_RDONLY);
    if (fd < 0) test_fail("Failed opening long name after short->long rename");
    memset(fbuf, 0, sizeof(fbuf));
    read(fd, fbuf, sizeof(fbuf) - 1);
    close(fd);
    if (strcmp(fbuf, "SHORT_DATA") != 0) test_fail("Data mismatch after short->long rename");

    if (rename("/this_is_a_very_long_file_name_now.txt", "/short2.txt") < 0) {
        test_fail("long->short rename failed");
    }
    fd = open("/short2.txt", O_RDONLY);
    if (fd < 0) test_fail("Failed opening short name after long->short rename");
    close(fd);
    unlink("/short2.txt");
    test_pass("short <-> long rename verified");

    /* Case-only rename */
    fd = open("/MixedCaseRename.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "CASE_DATA", 9);
    close(fd);
    if (rename("/MixedCaseRename.txt", "/mixedcaserename.txt") < 0) {
        test_fail("Case-only rename failed");
    }
    fd = open("/mixedcaserename.txt", O_RDONLY);
    if (fd < 0) test_fail("Failed opening after case-only rename");
    close(fd);
    unlink("/mixedcaserename.txt");
    test_pass("Case-only rename verified");

    /* Cross-directory rename & replace existing file */
    if (mkdir("/rdir1", 0755) < 0) test_fail("mkdir /rdir1 failed");
    if (mkdir("/rdir2", 0755) < 0) test_fail("mkdir /rdir2 failed");

    fd = open("/rdir1/src_file.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "SRC_DATA_123", 12);
    close(fd);

    fd = open("/rdir2/target_file.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "OLD_TARGET_DATA", 15);
    close(fd);

    /* Move /rdir1/src_file.txt -> /rdir2/target_file.txt (cross-dir + replace existing) */
    if (rename("/rdir1/src_file.txt", "/rdir2/target_file.txt") < 0) {
        test_fail("Cross-directory replace rename failed");
    }
    if (open("/rdir1/src_file.txt", O_RDONLY) >= 0) {
        test_fail("Source file still exists after cross-dir rename");
    }
    fd = open("/rdir2/target_file.txt", O_RDONLY);
    if (fd < 0) test_fail("Target file missing after replace rename");
    memset(fbuf, 0, sizeof(fbuf));
    read(fd, fbuf, sizeof(fbuf) - 1);
    close(fd);
    if (strcmp(fbuf, "SRC_DATA_123") != 0) {
        test_fail("Replaced target content mismatch");
    }
    unlink("/rdir2/target_file.txt");

    /* Same-directory replace existing file */
    fd = open("/rdir1/same_dir_src.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "SAME_SRC_123", 12);
    close(fd);
    fd = open("/rdir1/same_dir_dest.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "SAME_DEST_OLD", 13);
    close(fd);
    if (rename("/rdir1/same_dir_src.txt", "/rdir1/same_dir_dest.txt") < 0) {
        test_fail("Same-directory replace rename failed");
    }
    if (open("/rdir1/same_dir_src.txt", O_RDONLY) >= 0) {
        test_fail("same_dir_src still exists after replace rename");
    }
    fd = open("/rdir1/same_dir_dest.txt", O_RDONLY);
    if (fd < 0) test_fail("same_dir_dest missing after replace rename");
    memset(fbuf, 0, sizeof(fbuf));
    read(fd, fbuf, sizeof(fbuf) - 1);
    close(fd);
    if (strcmp(fbuf, "SAME_SRC_123") != 0) {
        test_fail("Same-dir replaced target content mismatch");
    }
    unlink("/rdir1/same_dir_dest.txt");

    test_pass("Cross-directory and same-directory file replacement verified");

    /* Directory rename with ".." fix */
    if (mkdir("/rdir1/moved_subdir", 0755) < 0) test_fail("mkdir /rdir1/moved_subdir failed");
    fd = open("/rdir1/moved_subdir/child.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "CHILD_DATA", 10);
    close(fd);

    /* Create marker in rdir2 */
    fd = open("/rdir2/marker_in_rdir2.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "MARKER2", 7);
    close(fd);

    /* Rename directory /rdir1/moved_subdir -> /rdir2/moved_subdir */
    if (rename("/rdir1/moved_subdir", "/rdir2/moved_subdir") < 0) {
        test_fail("Directory cross-directory rename failed");
    }

    /* Verify child inside moved directory */
    fd = open("/rdir2/moved_subdir/child.txt", O_RDONLY);
    if (fd < 0) test_fail("Child file missing inside moved directory");
    close(fd);

    /* Verify ".." in moved directory points to new parent (/rdir2) */
    if (chdir("/rdir2/moved_subdir") < 0) test_fail("chdir into moved_subdir failed");
    fd = open("../marker_in_rdir2.txt", O_RDONLY);
    if (fd < 0) test_fail("Moved directory '..' did not resolve to new parent");
    close(fd);
    chdir("/");

    /* Verify old directory location is completely gone */
    if (open("/rdir1/moved_subdir", O_RDONLY) >= 0) {
        test_fail("Old moved_subdir path still accessible");
    }
    if (open("/rdir1/moved_subdir/child.txt", O_RDONLY) >= 0) {
        test_fail("Old moved_subdir/child.txt still accessible");
    }

    /* Clean up rename test */
    unlink("/rdir2/marker_in_rdir2.txt");
    unlink("/rdir2/moved_subdir/child.txt");
    if (rmdir("/rdir2/moved_subdir") < 0) test_fail("rmdir /rdir2/moved_subdir failed");
    if (rmdir("/rdir1") < 0) test_fail("rmdir /rdir1 failed (possible orphan entries)");
    if (rmdir("/rdir2") < 0) test_fail("rmdir /rdir2 failed (possible orphan entries)");
    test_pass("Directory cross-directory rename with '..' fix and clean entries verified");

    /* ==============================================================
     * 15. Zero Frame Leak Verification
     * ============================================================== */
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
