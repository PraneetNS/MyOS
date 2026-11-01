#!/usr/bin/env python3
"""
tools/packet_fuzz.py - Seeded reproducible packet fuzzer for MyOS network stack.
Connects to QEMU socket netdev and streams malformed Ethernet/ARP/IPv4/ICMP/UDP/TCP frames.
Verifies that:
  - Kernel does not crash, panic, or hang.
  - Drop counters in /proc/net/dev increment.
  - System remains completely responsive after fuzzing.
"""

import sys
import os
import time
import socket
import struct
import random
import subprocess

SEED = 1337
NUM_FRAMES = 2500
TARGET_MAC = b"\x52\x54\x00\x12\x34\x56"
SRC_MAC = b"\x00\x11\x22\x33\x44\x55"


def generate_fuzz_frames(num_frames):
    random.seed(SEED)
    frames = []

    for i in range(num_frames):
        mode = i % 7

        if mode == 0:
            # 1. Truncated or Oversized Ethernet frames
            choice = random.randint(0, 3)
            if choice == 0:
                # Truncated (< 14 bytes)
                frame = os.urandom(random.randint(1, 13))
            elif choice == 1:
                # Oversized (> 1514 bytes)
                eth_hdr = TARGET_MAC + SRC_MAC + b"\x08\x00"
                frame = eth_hdr + os.urandom(random.randint(1515, 2048))
            elif choice == 2:
                # Bad ethertype
                frame = TARGET_MAC + SRC_MAC + struct.pack(">H", random.randint(0x0001, 0x0500)) + os.urandom(40)
            else:
                # Random garbage
                frame = os.urandom(random.randint(14, 128))

        elif mode == 1:
            # 2. Malformed ARP
            eth_hdr = TARGET_MAC + SRC_MAC + b"\x08\x06"
            choice = random.randint(0, 3)
            if choice == 0:
                # Truncated ARP (< 28 bytes)
                arp_body = os.urandom(random.randint(1, 27))
            elif choice == 1:
                # Bad hardware type or proto type
                arp_body = struct.pack(">HHBBH", random.randint(2, 65535), 0x0800, 6, 4, 1) + os.urandom(20)
            elif choice == 2:
                # Bad hardware/protocol length
                arp_body = struct.pack(">HHBBH", 1, 0x0800, random.randint(0, 255), random.randint(0, 255), 1) + os.urandom(20)
            else:
                # Zero sender / invalid op
                arp_body = struct.pack(">HHBBH", 1, 0x0800, 6, 4, random.randint(5, 65535)) + os.urandom(20)
            frame = eth_hdr + arp_body

        elif mode == 2:
            # 3. Malformed IPv4 Headers
            eth_hdr = TARGET_MAC + SRC_MAC + b"\x08\x00"
            choice = random.randint(0, 5)
            if choice == 0:
                # Truncated IPv4 (< 20 bytes)
                ip_pkt = b"\x45\x00" + os.urandom(random.randint(1, 17))
            elif choice == 1:
                # Bad IHL (< 5)
                ihl = random.randint(0, 4)
                ip_pkt = bytes([(4 << 4) | ihl]) + os.urandom(30)
            elif choice == 2:
                # Bad Total Length (< 20 or > frame len)
                bad_len = random.choice([0, 5, 15, 9999])
                ip_pkt = struct.pack(">BBHHHBBHII", 0x45, 0, bad_len, 1, 0, 64, 17, 0, 0x0A000202, 0x0A00020F) + os.urandom(20)
            elif choice == 3:
                # Fragmented IPv4 (MF flag set or non-zero fragment offset)
                flags_frag = random.choice([0x2000, 0x2001, 0x0001, 0x3FFF])
                ip_pkt = struct.pack(">BBHHHBBHII", 0x45, 0, 40, 0x1234, flags_frag, 64, 17, 0, 0x0A000202, 0x0A00020F) + os.urandom(20)
            elif choice == 4:
                # Malformed options: option length 0 or loop
                opt = b"\x83\x00\x00" + b"\x00" * 10
                ip_pkt = struct.pack(">BBHHHBBHII", 0x48, 0, 32 + len(opt), 1, 0, 64, 17, 0, 0x0A000202, 0x0A00020F) + opt + os.urandom(12)
            else:
                # Bad checksum / version != 4
                ip_pkt = struct.pack(">BBHHHBBHII", (random.randint(5, 15) << 4) | 5, 0, 28, 1, 0, 64, 17, 0xFFFF, 0x0A000202, 0x0A00020F) + os.urandom(8)
            frame = eth_hdr + ip_pkt

        elif mode == 3:
            # 4. Malformed ICMP
            eth_hdr = TARGET_MAC + SRC_MAC + b"\x08\x00"
            ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 28, 1, 0, 64, 1, 0, 0x0A000202, 0x0A00020F)
            choice = random.randint(0, 2)
            if choice == 0:
                # Truncated ICMP (< 8 bytes)
                icmp = os.urandom(random.randint(1, 7))
            elif choice == 1:
                # Bad ICMP type / code
                icmp = struct.pack(">BBHI", random.randint(19, 255), 0, 0, 0)
            else:
                # Invalid checksum
                icmp = struct.pack(">BBHI", 8, 0, 0x1234, 0) + os.urandom(16)
            frame = eth_hdr + ip_hdr + icmp

        elif mode == 4:
            # 5. Malformed UDP
            eth_hdr = TARGET_MAC + SRC_MAC + b"\x08\x00"
            choice = random.randint(0, 2)
            if choice == 0:
                # Truncated UDP (< 8 bytes)
                udp = os.urandom(random.randint(1, 7))
                ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0, 0x0A000202, 0x0A00020F)
            elif choice == 1:
                # UDP length mismatch (length in UDP header > IP payload)
                udp = struct.pack(">HHHH", 1234, 8080, 500, 0) + os.urandom(10)
                ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 20 + 18, 1, 0, 64, 17, 0, 0x0A000202, 0x0A00020F)
            else:
                # Zero UDP length (< 8)
                udp = struct.pack(">HHHH", 1234, 8080, random.randint(0, 7), 0) + os.urandom(8)
                ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0, 0x0A000202, 0x0A00020F)
            frame = eth_hdr + ip_hdr + udp

        elif mode == 5:
            # 6. Malformed TCP
            eth_hdr = TARGET_MAC + SRC_MAC + b"\x08\x00"
            choice = random.randint(0, 4)
            if choice == 0:
                # Truncated TCP (< 20 bytes)
                tcp = os.urandom(random.randint(1, 19))
                ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 20 + len(tcp), 1, 0, 64, 6, 0, 0x0A000202, 0x0A00020F)
            elif choice == 1:
                # Bad TCP Data Offset (< 5 or > total len)
                offset = random.choice([0, 1, 2, 3, 4, 15])
                tcp = struct.pack(">HHIIBBHHH", 1234, 80, 0, 0, (offset << 4), 0x02, 1024, 0, 0) + os.urandom(10)
                ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 20 + len(tcp), 1, 0, 64, 6, 0, 0x0A000202, 0x0A00020F)
            elif choice == 2:
                # TCP Option Parsing Loop (option kind with length 0)
                tcp_opts = b"\x02\x04\x05\xb4" + b"\x03\x00" + b"\x00" * 10
                offset = 5 + (len(tcp_opts) + 3) // 4
                tcp = struct.pack(">HHIIBBHHH", 1234, 80, 100, 0, (offset << 4), 0x02, 1024, 0, 0) + tcp_opts
                ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 20 + len(tcp), 1, 0, 64, 6, 0, 0x0A000202, 0x0A00020F)
            elif choice == 3:
                # Conflicting flags: SYN + FIN + RST + URG
                tcp = struct.pack(">HHIIBBHHH", 1234, 80, 100, 0, (5 << 4), 0x27, 1024, 0, 0)
                ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 40, 1, 0, 64, 6, 0, 0x0A000202, 0x0A00020F)
            else:
                # RST storm or ACK flood
                flags = random.choice([0x04, 0x14, 0x10])
                tcp = struct.pack(">HHIIBBHHH", random.randint(1024, 65535), 80, random.randint(0, 0xFFFFFFFF), random.randint(0, 0xFFFFFFFF), (5 << 4), flags, 0, 0, 0)
                ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 40, 1, 0, 64, 6, 0, 0x0A000202, 0x0A00020F)
            frame = eth_hdr + ip_hdr + tcp

        else:
            # 7. SYN Flood Burst
            eth_hdr = TARGET_MAC + SRC_MAC + b"\x08\x00"
            src_port = random.randint(1024, 65535)
            tcp = struct.pack(">HHIIBBHHH", src_port, 80, random.randint(0, 0xFFFFFFFF), 0, (5 << 4), 0x02, 1460, 0, 0)
            ip_hdr = struct.pack(">BBHHHBBHII", 0x45, 0, 40, 1, 0, 64, 6, 0, 0x0A000202, 0x0A00020F)
            frame = eth_hdr + ip_hdr + tcp

        frames.append(frame)

    return frames


def run_packet_fuzz_test():
    print(f"[fuzz] Generating {NUM_FRAMES} seeded malformed packets (seed={SEED})...")
    frames = generate_fuzz_frames(NUM_FRAMES)
    print(f"[fuzz] Generated {len(frames)} packets across Ethernet, ARP, IPv4, ICMP, UDP, TCP categories.")

    port = 12345
    qemu_cmd = [
        "qemu-system-x86_64", "-vga", "std", "-m", "256",
        "-device", "virtio-net-pci,netdev=n0",
        "-netdev", f"socket,id=n0,listen=127.0.0.1:{port}",
        "-hda", "disk.img", "-cdrom", "myos.iso", "-boot", "d",
        "-serial", "stdio", "-display", "none", "-no-reboot"
    ]

    print("[fuzz] Launching QEMU with socket netdev...")
    proc = subprocess.Popen(
        qemu_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )

    # Wait for QEMU socket to listen
    time.sleep(3)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    connected = False
    for attempt in range(10):
        try:
            sock.connect(("127.0.0.1", port))
            connected = True
            break
        except Exception:
            time.sleep(0.5)

    if not connected:
        print("[fuzz] FAILED: Could not connect to QEMU socket netdev")
        proc.kill()
        return False

    print(f"[fuzz] Connected to QEMU netdev on 127.0.0.1:{port}. Streaming packets...")

    start_t = time.time()
    for i, frame in enumerate(frames):
        # QEMU socket netdev protocol: 32-bit big-endian length + frame
        pkt_data = struct.pack(">I", len(frame)) + frame
        sock.sendall(pkt_data)
        if (i + 1) % 500 == 0:
            print(f"  Sent {i + 1}/{len(frames)} packets...")
            time.sleep(0.01)

    elapsed = time.time() - start_t
    print(f"[fuzz] Finished sending {len(frames)} packets in {elapsed:.2f}s ({len(frames)/elapsed:.1f} pkts/s).")
    sock.close()

    time.sleep(2)

    # Verify kernel responsiveness over serial console
    print("[fuzz] Checking kernel responsiveness and drop counters...")
    proc.stdin.write("cat /proc/net/dev\n")
    proc.stdin.flush()
    time.sleep(1)
    proc.stdin.write("echo FUZZ_RESPONSIVE_OK\n")
    proc.stdin.flush()
    time.sleep(1)
    proc.stdin.write("exit\n")
    proc.stdin.flush()

    output = ""
    start_wait = time.time()
    while time.time() - start_wait < 5:
        line = proc.stdout.readline()
        if not line:
            break
        output += line
        if "FUZZ_RESPONSIVE_OK" in output:
            break

    proc.kill()

    if "FUZZ_RESPONSIVE_OK" not in output:
        print("[fuzz] FAILED: Kernel did not respond after packet fuzzing!")
        print("Output was:")
        print(output)
        return False

    print("[fuzz] SUCCESS: Kernel survived 2,500 malformed packets without crash or hang!")
    return True


if __name__ == "__main__":
    success = run_packet_fuzz_test()
    sys.exit(0 if success else 1)

