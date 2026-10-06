/* hexdump.c -- canonical hex+ASCII dump */

#include "libc.h"

static void print_hex_byte(unsigned char b) {
    const char* hex = "0123456789abcdef";
    char out[2];
    out[0] = hex[(b >> 4) & 0xF];
    out[1] = hex[b & 0xF];
    write(1, out, 2);
}

static void print_hex_offset(unsigned int off) {
    const char* hex = "0123456789abcdef";
    char out[8];
    for (int i = 7; i >= 0; i--) {
        out[i] = hex[off & 0xF];
        off >>= 4;
    }
    write(1, out, 8);
}

int main(int argc, char** argv) {
    const char* filename = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            filename = argv[i];
            break;
        }
    }

    int fd = 0;
    if (filename) {
        fd = open(filename, O_RDONLY, 0);
        if (fd < 0) {
            write(2, "hexdump: cannot open file\n", 26);
            exit(1);
        }
    }

    unsigned char buf[16];
    unsigned int total_offset = 0;
    int n;

    while ((n = read(fd, buf, 16)) > 0) {
        print_hex_offset(total_offset);
        write(1, "  ", 2);

        for (int i = 0; i < 16; i++) {
            if (i < n) {
                print_hex_byte(buf[i]);
                write(1, " ", 1);
            } else {
                write(1, "   ", 3);
            }
            if (i == 7) write(1, " ", 1);
        }

        write(1, " |", 2);
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c >= 32 && c <= 126) {
                write(1, &c, 1);
            } else {
                write(1, ".", 1);
            }
        }
        write(1, "|\n", 2);

        total_offset += n;
    }

    if (total_offset > 0) {
        print_hex_offset(total_offset);
        write(1, "\n", 1);
    }

    if (filename) {
        close(fd);
    }

    exit(0);
    return 0;
}
