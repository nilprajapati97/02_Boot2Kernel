#!/bin/bash
# extract_early_boot.sh — Extract early boot info (assembly phases) from dmesg
set -euo pipefail

LOGFILE="${1:-$(dirname "$(dirname "$0")")/qemu_output.log}"

if [ ! -f "$LOGFILE" ]; then
    echo "Usage: $0 [logfile]"
    exit 1
fi

echo "=== Early Boot Extraction ==="
echo ""

echo "--- CPU Identification ---"
grep -E "CPU:|MIDR|Booting Linux on physical CPU|Detected.*CPU" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Boot Mode ---"
grep -E "boot mode|EL[12]|HYP mode|VHE" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- FDT ---"
grep -E "FDT|fdt|Machine model|device.tree|OF:" "$LOGFILE" | head -10 || echo "  (not found)"
echo ""

echo "--- Memory Detection ---"
grep -E "memory scan|cma|DRAM|Physical memory|memblock_add" "$LOGFILE" | head -20 || echo "  (not found)"
echo ""

echo "--- Page Table Setup ---"
grep -E "page table|pgtable|swapper|idmap|TTBR|ttbr" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Kernel Image ---"
grep -E "Kernel image|_text|\.text|kernel memory" "$LOGFILE" | head -10 || echo "  (not found)"
