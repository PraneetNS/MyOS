PREFIX ?= $(HOME)/opt/myos-cross
TARGET = i686-elf

CC = $(PREFIX)/bin/$(TARGET)-gcc
AR = $(PREFIX)/bin/$(TARGET)-ar
RANLIB = $(PREFIX)/bin/$(TARGET)-ranlib

CFLAGS = -O2 -Wall -fno-pie -fno-stack-protector -I. -isystem $(PREFIX)/$(TARGET)/include
OBJS = llm.o

all: libllm.a install

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

libllm.a: $(OBJS)
	$(AR) rcs libllm.a $(OBJS)
	$(RANLIB) libllm.a

install: libllm.a
	mkdir -p $(PREFIX)/$(TARGET)/include
	cp -f llm.h $(PREFIX)/$(TARGET)/include/
	mkdir -p $(PREFIX)/$(TARGET)/lib
	cp -f libllm.a $(PREFIX)/$(TARGET)/lib/
	mkdir -p ../../libmyos/
	cp -f libllm.a ../../libmyos/

clean:
	rm -f $(OBJS) libllm.a

.PHONY: all install clean

