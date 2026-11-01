PREFIX ?= $(HOME)/opt/myos-cross
TARGET = x86_64-elf

CC = $(PREFIX)/bin/$(TARGET)-gcc
AR = $(PREFIX)/bin/$(TARGET)-ar
RANLIB = $(PREFIX)/bin/$(TARGET)-ranlib

CFLAGS = -O2 -Wall -fno-pie -fno-stack-protector -mcmodel=small -Iinc -Isrc -isystem $(PREFIX)/$(TARGET)/include -DBR_LOMUL
SRCS = $(wildcard src/*/*.c) $(wildcard src/*.c) tls.c
OBJS = $(SRCS:.c=.o)

all: libtls.a install

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

libtls.a: $(OBJS)
	$(AR) rcs libtls.a $(OBJS)
	$(RANLIB) libtls.a

install: libtls.a
	mkdir -p $(PREFIX)/$(TARGET)/include/bearssl
	cp -f inc/*.h $(PREFIX)/$(TARGET)/include/bearssl/
	cp -f inc/bearssl.h $(PREFIX)/$(TARGET)/include/
	cp -f tls.h $(PREFIX)/$(TARGET)/include/
	mkdir -p $(PREFIX)/$(TARGET)/lib
	cp -f libtls.a $(PREFIX)/$(TARGET)/lib/
	mkdir -p ../../libmyos/
	cp -f libtls.a ../../libmyos/

clean:
	rm -f $(OBJS) libtls.a

.PHONY: all install clean
