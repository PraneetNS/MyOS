PREFIX ?= $(HOME)/opt/myos-cross
TARGET = i686-elf

CC = $(PREFIX)/bin/$(TARGET)-gcc
AR = $(PREFIX)/bin/$(TARGET)-ar
RANLIB = $(PREFIX)/bin/$(TARGET)-ranlib

CFLAGS = -O2 -Wall -fno-pie -fno-stack-protector -I. -I../../libmyos/include -isystem $(PREFIX)/$(TARGET)/include
OBJS = gfx_core.o gfx_draw.o gfx_font.o gfx_font_data.o gfx_image.o gfx_event.o

all: libgfx.a install

%.o: %.c gfx.h
	$(CC) $(CFLAGS) -c $< -o $@

libgfx.a: $(OBJS)
	$(AR) rcs libgfx.a $(OBJS)
	$(RANLIB) libgfx.a

install: libgfx.a
	mkdir -p $(PREFIX)/$(TARGET)/include
	cp gfx.h $(PREFIX)/$(TARGET)/include/
	mkdir -p $(PREFIX)/$(TARGET)/lib
	cp libgfx.a $(PREFIX)/$(TARGET)/lib/
	mkdir -p ../../libmyos/include
	cp gfx.h ../../libmyos/include/
	cp libgfx.a ../../libmyos/

clean:
	rm -f $(OBJS) libgfx.a

.PHONY: all install clean
