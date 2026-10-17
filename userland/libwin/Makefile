PREFIX ?= $(HOME)/opt/myos-cross
TARGET = i686-elf

CC = $(PREFIX)/bin/$(TARGET)-gcc
AR = $(PREFIX)/bin/$(TARGET)-ar
RANLIB = $(PREFIX)/bin/$(TARGET)-ranlib

CFLAGS = -O2 -Wall -fno-pie -fno-stack-protector -I. -I../../libmyos/include -I../libgfx -isystem $(PREFIX)/$(TARGET)/include
OBJS = win_client.o

all: libwin.a install

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

libwin.a: $(OBJS)
	$(AR) rcs libwin.a $(OBJS)
	$(RANLIB) libwin.a

install: libwin.a
	mkdir -p $(PREFIX)/$(TARGET)/include
	cp -f ../../libmyos/include/win.h $(PREFIX)/$(TARGET)/include/
	cp -f ../../libmyos/include/win_protocol.h $(PREFIX)/$(TARGET)/include/
	mkdir -p $(PREFIX)/$(TARGET)/lib
	cp -f libwin.a $(PREFIX)/$(TARGET)/lib/
	mkdir -p ../../libmyos/
	cp -f libwin.a ../../libmyos/

clean:
	rm -f $(OBJS) libwin.a

.PHONY: all install clean
