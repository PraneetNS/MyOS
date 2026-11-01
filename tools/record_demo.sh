#!/usr/bin/env bash
# tools/record_demo.sh
# Headless QEMU runner that drives agent demos, captures serial transcripts,
# takes screendump frames via QEMU QMP/monitor, and encodes video/GIF.
set -euo pipefail

DEMO_NAME="${1:-red-team}"
OUTPUT_DIR="demo_recordings/$DEMO_NAME"
mkdir -p "$OUTPUT_DIR"

echo "=== Recording Demo: $DEMO_NAME ==="
echo "Output directory: $OUTPUT_DIR"

TRANSCRIPT="$OUTPUT_DIR/transcript.txt"
SERIAL_LOG="$OUTPUT_DIR/serial.log"
rm -f "$TRANSCRIPT" "$SERIAL_LOG"

echo "[record] Launching headless QEMU with monitor..."
QMP_SOCK="/tmp/qemu_demo_${DEMO_NAME}.sock"
rm -f "$QMP_SOCK"

# Start testnet server in background if needed
if [ "$DEMO_NAME" = "researcher" ] || [ "$DEMO_NAME" = "red-team" ]; then
    python3 tools/testnet_server.py --bg >/dev/null 2>&1 || true
fi

# Run QEMU in background
qemu-system-i386 \
    -m 128 \
    -kernel isodir/boot/myos.bin \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0 \
    -serial file:"$SERIAL_LOG" \
    -qmp unix:"$QMP_SOCK",server,nowait \
    -display none &
QEMU_PID=$!

echo "[record] QEMU started with PID $QEMU_PID. Waiting for system boot..."
sleep 6

# Take periodic screendump snapshots via QMP if python3 socket available
python3 -c '
import socket, json, time, sys, os

sock_path = "'"$QMP_SOCK"'"
out_dir = "'"$OUTPUT_DIR"'"

def send_qmp(s, cmd, args=None):
    req = {"execute": cmd}
    if args: req["arguments"] = args
    s.sendall(json.dumps(req).encode() + b"\n")
    return s.recv(4096)

try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.recv(4096)
    send_qmp(s, "qmp_capabilities")

    for i in range(1, 10):
        frame_path = os.path.abspath(f"{out_dir}/frame_{i:03d}.ppm")
        send_qmp(s, "screendump", {"filename": frame_path})
        time.sleep(1)
    s.close()
except Exception as e:
    print(f"[record] QMP screendump note: {e}")
' || true

# Wait for demo run to finish
echo "[record] Collecting execution trace..."
sleep 4

# Terminate QEMU safely
kill -TERM "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

# Format transcript
echo "=== DEMO TRANSCRIPT: $DEMO_NAME ===" > "$TRANSCRIPT"
echo "Date: $(date)" >> "$TRANSCRIPT"
echo "--------------------------------------------------" >> "$TRANSCRIPT"
if [ -f "$SERIAL_LOG" ]; then
    grep -E "audit|agent|intent|model|netd|memd|PASS|DENY|ALLOWED" "$SERIAL_LOG" >> "$TRANSCRIPT" || cat "$SERIAL_LOG" | tail -n 50 >> "$TRANSCRIPT"
fi

echo "[record] Transcript written to $TRANSCRIPT"

# If ffmpeg is available, encode frames to MP4 and GIF
if command -v ffmpeg >/dev/null 2>&1 && ls "$OUTPUT_DIR"/frame_*.ppm >/dev/null 2>&1; then
    echo "[record] Encoding video using ffmpeg..."
    ffmpeg -y -framerate 2 -i "$OUTPUT_DIR/frame_%03d.ppm" -c:v libx264 -pix_fmt yuv420p "$OUTPUT_DIR/demo.mp4" 2>/dev/null || true
    ffmpeg -y -i "$OUTPUT_DIR/demo.mp4" "$OUTPUT_DIR/demo.gif" 2>/dev/null || true
    rm -f "$OUTPUT_DIR"/frame_*.ppm
    echo "[record] Recorded $OUTPUT_DIR/demo.mp4 and demo.gif"
fi

echo "=== Demo Recording Complete for $DEMO_NAME ==="

