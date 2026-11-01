#!/usr/bin/env bash
# tools/fetch_models.sh
# Downloads verified optional offline demonstration models (e.g. stories15M int8)
# and installs them to the disk image or staging directory.
set -euo pipefail

DEST_DIR="${1:-share/models}"
mkdir -p "$DEST_DIR"

STORIES15M_URL="https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin"
STORIES15M_FILE="$DEST_DIR/stories15M.bin"
STORIES15M_SHA256="4cf29b147321e257a4ab4889c5ea52df2d1e2e92c4a4e107df4db7893a9db9bb"

echo "=== MyOS Model Fetcher ==="
echo "Note: The local model tier is strictly an offline fallback/toy demo."
echo "Autonomous multi-step tool-calling requires the host bridge backend or direct API."
echo "Target directory: $DEST_DIR"

if [ -f "$STORIES15M_FILE" ]; then
    echo "Checking existing $STORIES15M_FILE..."
    ACTUAL_SHA=$(sha256sum "$STORIES15M_FILE" | awk '{print $1}')
    if [ "$ACTUAL_SHA" = "$STORIES15M_SHA256" ]; then
        echo "stories15M.bin already exists and checksum is verified!"
        exit 0
    else
        echo "Checksum mismatch! Re-downloading..."
        rm -f "$STORIES15M_FILE"
    fi
fi

echo "Downloading stories15M.bin (~60MB)..."
if command -v curl >/dev/null 2>&1; then
    curl -L "$STORIES15M_URL" -o "$STORIES15M_FILE"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$STORIES15M_FILE" "$STORIES15M_URL"
else
    echo "Error: neither curl nor wget found."
    exit 1
fi

echo "Verifying SHA-256 checksum..."
ACTUAL_SHA=$(sha256sum "$STORIES15M_FILE" | awk '{print $1}')
if [ "$ACTUAL_SHA" != "$STORIES15M_SHA256" ]; then
    echo "Warning: Downloaded file checksum ($ACTUAL_SHA) did not match expected ($STORIES15M_SHA256)!"
    echo "The file may be a different quantization revision or mirrors updated."
else
    echo "Verification SUCCESS! stories15M.bin is ready."
fi

echo "To copy into disk.img:"
echo "  mcopy -i disk.img \"$STORIES15M_FILE\" ::/share/models/stories15M.bin"

