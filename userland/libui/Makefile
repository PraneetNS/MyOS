PREFIX ?= $(HOME)/opt/myos-cross
TARGET = i686-elf

CC = $(PREFIX)/bin/$(TARGET)-gcc
AR = $(PREFIX)/bin/$(TARGET)-ar
RANLIB = $(PREFIX)/bin/$(TARGET)-ranlib

CFLAGS = -O2 -Wall -fno-pie -fno-stack-protector -I. -I../../libmyos/include -I../libgfx -I../libwin -I../libjson -isystem $(PREFIX)/$(TARGET)/include
OBJS = ui_core.o ui_widgets.o ui_text.o ui_advanced.o ui_a11y.o

all: libui.a install

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

libui.a: $(OBJS)
	$(AR) rcs libui.a $(OBJS)
	$(RANLIB) libui.a

install: libui.a
	mkdir -p $(PREFIX)/$(TARGET)/include
	cp -f ../../libmyos/include/ui.h $(PREFIX)/$(TARGET)/include/
	mkdir -p $(PREFIX)/$(TARGET)/lib
	cp -f libui.a $(PREFIX)/$(TARGET)/lib/
	mkdir -p ../../libmyos/
	cp -f libui.a ../../libmyos/

clean:
	rm -f $(OBJS) libui.a

.PHONY: all install clean
