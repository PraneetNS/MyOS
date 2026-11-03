#!/bin/bash
set -e
cd "$(dirname "$0")/.."

echo "=========================================================="
echo "[TEST] Session 23: Linux ABI Compatibility & Real Software"
echo "=========================================================="

python3 tools/test_session23.py

echo "=========================================================="
echo "ALL SESSION 23 TESTS PASSED SUCCESSFULLY!"
echo "=========================================================="
