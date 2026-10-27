PREFIX ?= $(HOME)/opt/myos-cross
TARGET = i686-elf

CC = $(PREFIX)/bin/$(TARGET)-gcc
AR = $(PREFIX)/bin/$(TARGET)-ar
RANLIB = $(PREFIX)/bin/$(TARGET)-ranlib

CFLAGS = -O2 -Wall -fno-pie -fno-stack-protector -I. -isystem $(PREFIX)/$(TARGET)/include
OBJS = json.o

all: libjson.a install

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

libjson.a: $(OBJS)
	$(AR) rcs libjson.a $(OBJS)
	$(RANLIB) libjson.a

install: libjson.a
	mkdir -p $(PREFIX)/$(TARGET)/include
	cp -f json.h $(PREFIX)/$(TARGET)/include/
	mkdir -p $(PREFIX)/$(TARGET)/lib
	cp -f libjson.a $(PREFIX)/$(TARGET)/lib/
	mkdir -p ../../libmyos/
	cp -f libjson.a ../../libmyos/

clean:
	rm -f $(OBJS) libjson.a

.PHONY: all install clean
