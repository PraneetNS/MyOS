CC = ../tools/myos-gcc
CFLAGS ?= -O2 -Wall
SRCS = $(wildcard *.c)
PROGS = $(SRCS:.c=.elf)

all: $(PROGS)

%.elf: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f *.elf *.o

.PHONY: all clean
