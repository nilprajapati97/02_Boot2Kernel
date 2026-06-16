#!/bin/bash
# test_phase_order.sh — Verify phases execute in correct order
#
# Parses dmesg/log output and verifies each module loaded in the
# correct sequence (01 → 02 → ... → 13).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPL_DIR="$(dirname "$SCRIPT_DIR")"
LOG="${1:-${IMPL_DIR}/output/qemu_test_output.log}"

if [ ! -f "$LOG" ]; then
    echo "[ERROR] Log file not found: $LOG"
    echo "Usage: $0 [logfile]"
    echo "       Default: output/qemu_test_output.log"
    exit 1
fi

echo "========================================="
echo " Phase Order Validation"
echo "========================================="
echo "Log: $LOG"
echo ""

PREV_LINE=0
ALL_ORDERED=1

for i in $(seq 1 13); do
    padded=$(printf "%02d" "$i")
    pattern="Module ${padded}:"

    # Find first occurrence line number
    line=$(grep -n "$pattern" "$LOG" 2>/dev/null | head -1 | cut -d: -f1)

    if [ -z "$line" ]; then
        echo "  [MISS] Phase $padded: not found in log"
        ALL_ORDERED=0
        continue
    fi

    if [ "$line" -gt "$PREV_LINE" ]; then
        echo "  [OK]   Phase $padded: line $line (after $PREV_LINE)"
    else
        echo "  [ERR]  Phase $padded: line $line — OUT OF ORDER (prev=$PREV_LINE)"
        ALL_ORDERED=0
    fi

    PREV_LINE="$line"
done

echo ""
if [ "$ALL_ORDERED" -eq 1 ]; then
    echo "Result: ALL phases in correct order ✓"
    exit 0
else
    echo "Result: Phase ordering FAILED ✗"
    exit 1
fi
