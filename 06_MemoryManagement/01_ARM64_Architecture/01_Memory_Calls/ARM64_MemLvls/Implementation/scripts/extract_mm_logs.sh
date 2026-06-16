#!/bin/bash
# extract_mm_logs.sh — Parse dmesg/QEMU output for MM initialization phases
set -euo pipefail

LOGFILE="${1:-$(dirname "$(dirname "$0")")/qemu_output.log}"

if [ ! -f "$LOGFILE" ]; then
    echo "Usage: $0 [logfile]"
    echo "Default: qemu_output.log"
    exit 1
fi

echo "========================================="
echo " ARM64 Memory Init — Phase Extraction"
echo "========================================="
echo "Source: $LOGFILE"
echo ""

echo "--- Phase 1-6: Early Boot (Assembly) ---"
grep -n -E "Booting Linux|Boot CPU|CPU:|Machine model|Hardware name" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Phase 7: setup_arch ---"
grep -n -E "setup_arch|fixmap|earlycon|Machine model" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Phase 8: Memblock ---"
grep -n -E "memblock|MEMBLOCK|memory\[|reserved\[|memstart_addr" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Phase 9: Paging Init ---"
grep -n -E "Virtual kernel memory layout|modules|vmalloc|fixmap|linear|\.text|\.rodata|\.data|\.init" "$LOGFILE" | head -20 || echo "  (not found)"
echo ""

echo "--- Phase 10: Bootmem Init ---"
grep -n -E "Zone ranges|DMA|DMA32|Normal|Movable|max_pfn|NUMA|node|CMA|crashkernel" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Phase 11: MM Core Init ---"
grep -n -E "Freeing unused|free_area_init|zonelists|buddy|totalram" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Phase 12: SLAB/SLUB ---"
grep -n -E "SLUB|slab|kmem_cache|kmalloc" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Phase 13: vmalloc ---"
grep -n -E "vmalloc|vmap|VMALLOC" "$LOGFILE" || echo "  (not found)"
echo ""

echo "--- Phase Order (timestamps) ---"
grep -E "Booting Linux|setup_arch|memblock|Virtual kernel|Zone ranges|Freeing unused|SLUB|vmalloc_init" "$LOGFILE" | head -20 || echo "  (not found)"
