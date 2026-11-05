#!/bin/bash
set -e
cd "$(dirname "$0")/.."

echo "=========================================================="
echo "[TEST] Session 24: POSIX Threads, Futex & Signal Routing"
echo "=========================================================="

python3 tools/test_session24.py

echo "=========================================================="
echo "ALL SESSION 24 TESTS PASSED SUCCESSFULLY!"
echo "=========================================================="

