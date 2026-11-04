#!/bin/bash
set -e
cd "$(dirname "$0")/.."

LOGFILE="/tmp/test_boot_run.log"

if [ "$1" = "--from-log" ] && [ -n "$2" ] && [ -f "$2" ]; then
    LOGFILE="$2"
else
    echo "Running full tools/test_boot.sh suite (this may take several minutes)..."
    ./tools/test_boot.sh 2>&1 | tee "$LOGFILE"
fi

echo "Generating docs/testing.md from test execution log..."

mkdir -p docs

cat << 'EOF' > docs/testing.md
# MyOS Automated Test Matrix

> **Source of Truth**: This document is generated automatically by `tools/gen_test_matrix.sh` directly from execution of `tools/test_boot.sh`. Do not edit by hand.

## Test Sessions Execution Matrix

| Session | Title | Status |
|:---|:---|:---:|
EOF

awk '/ONE-LINE-PER-SESSION SUMMARY/{flag=1; next} /============================================================/{if(flag) flag=0} flag {
    if ($0 ~ /^Session /) {
        split($0, parts, " \\| ");
        printf("| **%s** | %s | **%s** |\n", parts[1], parts[2], parts[3]);
    }
}' "$LOGFILE" >> docs/testing.md

cat << 'EOF' >> docs/testing.md

---

## Verification Methodology
- Every session boots inside a hermetic QEMU x86-64 long-mode machine with serial I/O logging.
- Memory leak checks assert baseline versus final free physical frames within strict tolerances.
- Filesystem consistency is checked with host-side `fsck.fat -v -n disk.img` after dirty reboots and poweroffs.
- Capability violations, CPU time budget overruns, and token budget limits are strictly audited against cryptographic SHA-256 hash chains.
EOF

echo "Successfully generated docs/testing.md:"
cat docs/testing.md

