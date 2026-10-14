CC      = gcc
AS      = as
LD      = ld

CFLAGS  = -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
          -fno-strict-aliasing -O2 -Wall -Wextra -nostdlib -MMD -MP
ASFLAGS = --32
LDFLAGS = -m elf_i386 -T boot/linker.ld -nostdlib

OBJS = boot/boot.o boot/gdt_flush.o boot/idt_flush.o boot/isr.o boot/paging_asm.o \
       boot/usermode_asm.o boot/task_switch.o boot/resume_state.o boot/uaccess.o \
       src/kernel.o src/console.o src/font.o src/gdt.o src/idt.o src/timer.o src/keyboard.o \
       src/pmm.o src/paging.o src/kheap.o src/tss.o src/syscall.o \
       src/ata.o src/fs.o src/elf.o src/shell.o src/vmm.o src/vma.o \
       src/process.o src/scheduler.o src/kmutex.o src/pipe.o src/serial.o src/bcache.o src/vfs.o src/fat16.o \
       src/tty.o src/devfs.o src/signal.o src/rtc.o src/procfs.o src/fpu.o \
       src/ioremap.o src/fb.o src/mouse.o src/input.o

DEPS = $(OBJS:.o=.d)

KERNEL_BIN = isodir/boot/myos.bin
ISO        = myos.iso

all: $(ISO)

-include $(DEPS)

boot/%.o: boot/%.s
	$(AS) $(ASFLAGS) $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(OBJS)
	mkdir -p isodir/boot/grub
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(OBJS)

$(ISO): $(KERNEL_BIN) isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) isodir 2>/dev/null

isodir/boot/grub/grub.cfg: boot/grub.cfg
	mkdir -p isodir/boot/grub
	cp boot/grub.cfg isodir/boot/grub/grub.cfg

run: $(ISO)
	qemu-system-i386 -vga std -m 256 -cdrom $(ISO) -serial stdio -display none -no-reboot

test: $(ISO) disk.img
	timeout 10s qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom $(ISO) -boot d -serial stdio -display none -no-reboot || true

disk.img:
	bash tools/build_disk.sh

clean:
	rm -f boot/*.o src/*.o boot/*.d src/*.d $(KERNEL_BIN) $(ISO)
	rm -rf isodir

.PHONY: all run test clean
