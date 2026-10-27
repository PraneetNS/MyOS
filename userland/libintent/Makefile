PREFIX ?= $(HOME)/opt/myos-cross
TARGET = i686-elf

CC = $(PREFIX)/bin/$(TARGET)-gcc
AR = $(PREFIX)/bin/$(TARGET)-ar
RANLIB = $(PREFIX)/bin/$(TARGET)-ranlib

CFLAGS = -O2 -Wall -fno-pie -fno-stack-protector -I. -I../libjson -I../../libmyos/include -isystem $(PREFIX)/$(TARGET)/include
OBJS = intent_client.o

all: libintent.a install

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

libintent.a: $(OBJS)
	$(AR) rcs libintent.a $(OBJS)
	$(RANLIB) libintent.a

install: libintent.a
	mkdir -p $(PREFIX)/$(TARGET)/include
	cp -f intent.h $(PREFIX)/$(TARGET)/include/
	mkdir -p $(PREFIX)/$(TARGET)/lib
	cp -f libintent.a $(PREFIX)/$(TARGET)/lib/
	mkdir -p ../../libmyos/
	cp -f libintent.a ../../libmyos/

clean:
	rm -f $(OBJS) libintent.a

.PHONY: all install clean
