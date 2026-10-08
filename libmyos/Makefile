PREFIX ?= $(HOME)/opt/myos-cross
TARGET = i686-elf

CC = $(PREFIX)/bin/$(TARGET)-gcc
AR = $(PREFIX)/bin/$(TARGET)-ar
RANLIB = $(PREFIX)/bin/$(TARGET)-ranlib

CFLAGS = -O2 -Wall -fno-pie -fno-stack-protector -Iinclude -isystem $(PREFIX)/$(TARGET)/include
ASFLAGS =

OBJS = stubs.o posix.o init.o

all: crt0.o libmyos.a install-headers

crt0.o: crt0.S
	$(CC) $(ASFLAGS) -c crt0.S -o crt0.o

stubs.o: stubs.c syscall.h
	$(CC) $(CFLAGS) -c stubs.c -o stubs.o

posix.o: posix.c syscall.h
	$(CC) $(CFLAGS) -c posix.c -o posix.o

init.o: init.c
	$(CC) $(CFLAGS) -c init.c -o init.o

libmyos.a: $(OBJS)
	$(AR) rcs libmyos.a $(OBJS)
	$(RANLIB) libmyos.a

install-headers:
	mkdir -p $(PREFIX)/$(TARGET)/include/sys
	cp -r include/* $(PREFIX)/$(TARGET)/include/

clean:
	rm -f crt0.o $(OBJS) libmyos.a

.PHONY: all clean install-headers
