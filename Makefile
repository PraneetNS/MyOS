CC      = /root/opt/myos-cross/bin/x86_64-elf-gcc
AS      = /root/opt/myos-cross/bin/x86_64-elf-as
LD      = /root/opt/myos-cross/bin/x86_64-elf-ld

CFLAGS  = -m64 -std=gnu11 -ffreestanding -mcmodel=kernel -mno-red-zone \
          -mno-sse -mno-mmx -fno-pie -fno-pic -fno-stack-protector \
          -fno-strict-aliasing -O2 -Wall -Wextra -nostdlib -MMD -MP \
          -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DMYOS_TESTING=1 -Iinclude -Isrc -Isrc/net
ASFLAGS = --64
LDFLAGS = -m elf_x86_64 -T boot/linker.ld -nostdlib -z max-page-size=0x1000

OBJS = boot/boot.o boot/gdt_flush.o boot/idt_flush.o boot/isr.o boot/paging_asm.o \
       boot/usermode_asm.o boot/task_switch.o boot/resume_state.o boot/uaccess.o \
       boot/syscall_entry.o \
       src/kernel.o src/console.o src/font.o src/gdt.o src/idt.o src/timer.o src/keyboard.o \
       src/pmm.o src/paging.o src/kheap.o src/kstack.o src/tss.o src/syscall.o src/futex.o \
       src/ata.o src/fs.o src/elf.o src/shell.o src/vmm.o src/vma.o \
       src/process.o src/scheduler.o src/kmutex.o src/pipe.o src/serial.o src/bcache.o src/vfs.o src/fat16.o \
       src/tty.o src/devfs.o src/signal.o src/rtc.o src/procfs.o src/fpu.o \
       src/ioremap.o src/fb.o src/mouse.o src/input.o src/tmpfs.o src/socket.o src/pty.o \
       src/sha256.o src/agent.o src/pci.o src/random.o \
       src/net/pbuf.o src/net/netdev.o src/net/virtio_net.o src/net/ethernet.o \
       src/net/arp.o src/net/ipv4.o src/net/icmp.o src/net/udp.o src/net/tcp.o

DEPS = $(OBJS:.o=.d)

KERNEL_BIN = isodir/boot/myos.bin
ISO        = myos.iso

all: $(ISO)

-include $(DEPS)

boot/%.o: boot/%.s
	$(AS) $(ASFLAGS) $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/net/%.o: src/net/%.c
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
	qemu-system-x86_64 -vga std -m 256 -cdrom $(ISO) -serial stdio -display none -no-reboot

test: $(ISO) disk.img
	timeout 10s qemu-system-x86_64 -vga std -m 256 -hda disk.img -cdrom $(ISO) -boot d -serial stdio -display none -no-reboot || true

disk.img:
	bash tools/build_disk.sh

release: $(ISO) disk.img
	mkdir -p dist
	cp $(ISO) dist/myos.iso
	cp disk.img dist/disk.img
	cd dist && sha256sum myos.iso disk.img > SHA256SUMS

clean:
	rm -f boot/*.o src/*.o src/net/*.o boot/*.d src/*.d src/net/*.d $(KERNEL_BIN) $(ISO)
	rm -rf isodir dist

.PHONY: all run test clean release

demo: demo-redteam

demo-organizer: all disk.img
	./tools/record_demo.sh organizer

demo-researcher: all disk.img
	./tools/record_demo.sh researcher

demo-redteam: all disk.img
	./tools/record_demo.sh red-team

.PHONY: demo demo-organizer demo-researcher demo-redteam
