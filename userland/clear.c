#include <stdio.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    (void) argc;
    (void) argv;
    /* Clear screen and move cursor to home position */
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
    return 0;
}
