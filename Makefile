CC      = gcc
AS      = as
LD      = ld

CFLAGS  = -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
          -O2 -Wall -Wextra -nostdlib
ASFLAGS = --32
LDFLAGS = -m elf_i386 -T boot/linker.ld -nostdlib

OBJS = boot/boot.o boot/gdt_flush.o boot/idt_flush.o boot/isr.o boot/paging_asm.o \
       boot/usermode_asm.o boot/task_switch.o \
       src/kernel.o src/vga.o src/gdt.o src/idt.o src/timer.o src/keyboard.o \
       src/pmm.o src/paging.o src/kheap.o src/tss.o src/syscall.o \
       src/usermode_demo.o src/task.o src/demo_tasks.o \
       src/ata.o src/fs.o src/elf.o src/shell.o

KERNEL_BIN = isodir/boot/myos.bin
ISO        = myos.iso

all: $(ISO)

boot/%.o: boot/%.s
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
