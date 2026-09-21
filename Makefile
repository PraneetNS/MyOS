CC      = gcc
AS      = as
LD      = ld

CFLAGS  = -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
          -O2 -Wall -Wextra -nostdlib
ASFLAGS = --32
LDFLAGS = -m elf_i386 -T boot/linker.ld -nostdlib

OBJS = boot/boot.o src/kernel.o src/vga.o

KERNEL_BIN = isodir/boot/myos.bin
ISO        = myos.iso

all: $(ISO)

boot/boot.o: boot/boot.s
	$(AS) $(ASFLAGS) $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(OBJS)
	mkdir -p isodir/boot/grub
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(OBJS)

$(ISO): $(KERNEL_BIN) isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) isodir 2>/dev/null

isodir/boot/grub/grub.cfg:
	mkdir -p isodir/boot/grub
	cp boot/grub.cfg isodir/boot/grub/grub.cfg

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -serial stdio -display none -no-reboot

clean:
	rm -f boot/*.o src/*.o $(KERNEL_BIN) $(ISO)
	rm -rf isodir

.PHONY: all run clean
