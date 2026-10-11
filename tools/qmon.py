#!/usr/bin/env python3
"""
qmon.py - QEMU Monitor Client & Verification Tool for MyOS

Connects to QEMU's UNIX domain monitor socket and provides helper functions
and CLI commands to control QEMU and inspect framebuffers:
  - screendump <filename>
  - sendkey <key>
  - mouse_move <dx> <dy>
  - mouse_button <state>
  - pixel inspection and assertions on PPM files
"""

import sys
import os
import time
import socket
import argparse

DEFAULT_SOCK = "/tmp/myos-mon.sock"

class QemuMonitor:
    def __init__(self, sock_path=DEFAULT_SOCK, timeout=10.0):
        self.sock_path = sock_path
        self.timeout = timeout
        self.sock = None
        self.connect()

    def connect(self):
        end_time = time.time() + self.timeout
        last_err = None
        while time.time() < end_time:
            if os.path.exists(self.sock_path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.settimeout(2.0)
                    s.connect(self.sock_path)
                    self.sock = s
                    # Consume banner
                    try:
                        self.sock.recv(4096)
                    except Exception:
                        pass
                    return
                except Exception as e:
                    last_err = e
            time.sleep(0.1)
        raise ConnectionError(f"Failed to connect to QEMU monitor at {self.sock_path}: {last_err}")

    def cmd(self, command, wait=0.2):
        if not self.sock:
            raise RuntimeError("Not connected to QEMU monitor")
        if not command.endswith("\n"):
            command += "\n"
        self.sock.sendall(command.encode("utf-8"))
        time.sleep(wait)
        out = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                out += chunk
                if b"(qemu)" in chunk:
                    break
        except socket.timeout:
            pass
        return out.decode("utf-8", errors="ignore")

    def screendump(self, path):
        # Convert path to absolute
        abs_path = os.path.abspath(path)
        if os.path.exists(abs_path):
            try:
                os.remove(abs_path)
            except OSError:
                pass
        res = self.cmd(f"screendump {abs_path}", wait=0.5)
        # Wait up to 3 seconds for file to be flushed
        for _ in range(30):
            if os.path.exists(abs_path) and os.path.getsize(abs_path) > 100:
                break
            time.sleep(0.1)
        if not os.path.exists(abs_path):
            raise FileNotFoundError(f"screendump failed to generate {abs_path}: {res}")
        return res

    def sendkey(self, key):
        return self.cmd(f"sendkey {key}", wait=0.1)

    def mouse_move(self, dx, dy):
        return self.cmd(f"mouse_move {dx} {dy}", wait=0.1)

    def mouse_button(self, state):
        return self.cmd(f"mouse_button {state}", wait=0.1)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


def read_ppm(path):
    """Parses a P6 binary PPM file, returning (width, height, bytes)."""
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            raise ValueError(f"Invalid PPM magic: {magic} in {path}")
        # Parse dimensions, skipping comment lines
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.strip().split())
        # Parse max color value
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        maxval = int(line.strip())
        if maxval != 255:
            raise ValueError(f"Unsupported PPM maxval {maxval}")
        data = f.read()
        if len(data) < w * h * 3:
            raise ValueError(f"Corrupted PPM data: expected {w*h*3} bytes, got {len(data)}")
        return w, h, data


def get_pixel(path, x, y):
    """Returns RGB tuple (r, g, b) at coordinate (x, y) from PPM file."""
    w, h, data = read_ppm(path)
    if not (0 <= x < w and 0 <= y < h):
        raise IndexError(f"Coordinates ({x}, {y}) out of bounds ({w}x{h})")
    idx = (y * w + x) * 3
    return (data[idx], data[idx + 1], data[idx + 2])


def assert_pixel(path, x, y, expected_rgb, tolerance=5):
    """Asserts that pixel at (x, y) matches expected (r, g, b) within tolerance."""
    actual_rgb = get_pixel(path, x, y)
    diff = max(abs(a - b) for a, b in zip(actual_rgb, expected_rgb))
    if diff > tolerance:
        raise AssertionError(
            f"Pixel mismatch at ({x}, {y}): expected RGB{expected_rgb}, got RGB{actual_rgb} (diff={diff})"
        )
    return actual_rgb


def has_text_pixels(path, row_start=0, row_end=64, min_count=20):
    """Checks if there are non-black pixels in specified rows indicating text console output."""
    w, h, data = read_ppm(path)
    count = 0
    for y in range(row_start, min(h, row_end)):
        for x in range(w):
            idx = (y * w + x) * 3
            if data[idx] > 30 or data[idx+1] > 30 or data[idx+2] > 30:
                count += 1
                if count >= min_count:
                    return True
    return False


def main():
    parser = argparse.ArgumentParser(description="QEMU Monitor Utility for MyOS")
    parser.add_argument("--sock", default=DEFAULT_SOCK, help="Path to QEMU monitor socket")
    subparsers = parser.add_subparsers(dest="action", required=True)

    dump_p = subparsers.add_parser("screendump", help="Capture framebuffer to PPM file")
    dump_p.add_argument("file", help="Destination PPM file")

    key_p = subparsers.add_parser("sendkey", help="Send key event via QEMU monitor")
    key_p.add_argument("key", help="Key name (e.g. esc, ret, a, b)")

    move_p = subparsers.add_parser("mouse_move", help="Inject mouse movement")
    move_p.add_argument("dx", type=int, help="Relative X motion")
    move_p.add_argument("dy", type=int, help="Relative Y motion")

    btn_p = subparsers.add_parser("mouse_button", help="Inject mouse button state")
    btn_p.add_argument("state", type=int, help="Button bitmask (1=left, 2=middle, 4=right)")

    check_p = subparsers.add_parser("check_pixel", help="Inspect pixel in PPM file")
    check_p.add_argument("file", help="PPM file path")
    check_p.add_argument("x", type=int)
    check_p.add_argument("y", type=int)
    check_p.add_argument("--expect", nargs=3, type=int, metavar=("R", "G", "B"), help="Expected RGB")

    args = parser.parse_args()

    if args.action == "check_pixel":
        rgb = get_pixel(args.file, args.x, args.y)
        print(f"Pixel ({args.x}, {args.y}) = RGB{rgb} (0x{rgb[0]:02X}{rgb[1]:02X}{rgb[2]:02X})")
        if args.expect:
            expected = tuple(args.expect)
            assert_pixel(args.file, args.x, args.y, expected)
            print(f"[PASS] Pixel matches expected RGB{expected}")
        return

    with QemuMonitor(sock_path=args.sock) as mon:
        if args.action == "screendump":
            mon.screendump(args.file)
            print(f"[qmon] Captured screendump to {args.file}")
        elif args.action == "sendkey":
            mon.sendkey(args.key)
            print(f"[qmon] Sent key: {args.key}")
        elif args.action == "mouse_move":
            mon.mouse_move(args.dx, args.dy)
            print(f"[qmon] Injected mouse move: dx={args.dx}, dy={args.dy}")
        elif args.action == "mouse_button":
            mon.mouse_button(args.state)
            print(f"[qmon] Injected mouse button: state={args.state}")

if __name__ == "__main__":
    main()
