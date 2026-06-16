#!/bin/bash
# run_all_tests.sh — Automated test runner
#
# Boots QEMU, captures output, verifies each phase module loaded
# and produced expected output. Returns pass/fail count.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPL_DIR="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="${IMPL_DIR}/output"
LOG="${OUTPUT_DIR}/qemu_test_output.log"
TIMEOUT=120

mkdir -p "$OUTPUT_DIR"

# Check prerequisites
KERNEL="${IMPL_DIR}/output/Image"
INITRAMFS="${IMPL_DIR}/output/initramfs.cpio.gz"

if [ ! -f "$KERNEL" ]; then
    echo "[ERROR] Kernel image not found: $KERNEL"
    echo "        Run: ./scripts/build_kernel.sh"
    exit 1
fi

if [ ! -f "$INITRAMFS" ]; then
    echo "[ERROR] Initramfs not found: $INITRAMFS"
    echo "        Run: ./scripts/create_rootfs.sh"
    exit 1
fi

echo "========================================="
echo " ARM64 MM Validation — Automated Tests"
echo "========================================="
echo ""
echo "Kernel:    $KERNEL"
echo "Initramfs: $INITRAMFS"
echo "Log:       $LOG"
echo "Timeout:   ${TIMEOUT}s"
echo ""

# Run QEMU with timeout, capture all output
echo "[*] Booting QEMU..."
timeout "$TIMEOUT" qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu cortex-a72 \
    -smp 8 \
    -m 4G \
    -nographic \
    -kernel "$KERNEL" \
    -initrd "$INITRAMFS" \
    -append "console=ttyAMA0 earlycon loglevel=8 memblock=debug nokaslr slub_debug=FZP" \
    -no-reboot \
    2>&1 | tee "$LOG" || true

echo ""
echo "========================================="
echo " Test Results"
echo "========================================="

TOTAL=0
PASS=0
FAIL=0

check_phase() {
    local num="$1"
    local pattern="$2"
    local desc="$3"

    TOTAL=$((TOTAL + 1))
    if grep -q "$pattern" "$LOG" 2>/dev/null; then
        echo "  [PASS] Phase $num: $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] Phase $num: $desc (pattern '$pattern' not found)"
        FAIL=$((FAIL + 1))
    fi
}

# Check each module loaded and produced output
check_phase "01" "Module 01: MMU State"      "MMU State Inspector"
check_phase "02" "Module 02: Identity Map"    "Identity Map Walker"
check_phase "03" "Module 03: Page Table"      "Page Table Walker"
check_phase "04" "Module 04: TTBR Translate"  "TTBR Translation"
check_phase "05" "Module 05: Kernel Segments" "Kernel Segments"
check_phase "06" "Module 06: Boot Info"       "Boot Info"
check_phase "07" "Module 07: Fixmap"          "Fixmap Inspector"
check_phase "08" "Module 08: Memblock"        "Memblock Dump"
check_phase "09" "Module 09: Linear Map"      "Linear Map Verify"
check_phase "10" "Module 10: Zone"            "Zone Inspector"
check_phase "11" "Module 11: Buddy"           "Buddy Inspector"
check_phase "12" "Module 12: SLAB"            "SLAB/SLUB Inspector"
check_phase "13" "Module 13: Vmalloc"         "Vmalloc Inspector"

# Check specific bit-level validations
echo ""
echo "--- Bit-Level Checks ---"

check_phase "01-sctlr" "SCTLR_EL1"      "SCTLR register decoded"
check_phase "01-tcr"   "TCR_EL1"         "TCR register decoded"
check_phase "01-mair"  "MAIR_EL1"        "MAIR register decoded"
check_phase "02-va-pa" "VA==PA"          "Identity map VA==PA"
check_phase "03-walk"  "PGD\\|PUD\\|PMD\\|PTE"  "Page table walk levels"
check_phase "08-mem"   "memstart_addr"   "memstart_addr found"
check_phase "09-trip"  "PASS"            "Round-trip verification"
check_phase "11-alloc" "alloc_pages"     "Buddy allocation test"
check_phase "12-kmalloc" "kmalloc"       "kmalloc test"
check_phase "13-vmalloc" "vmalloc"       "vmalloc test"

echo ""
echo "========================================="
echo " Summary: $PASS/$TOTAL passed, $FAIL failed"
echo "========================================="

# Extract per-module logs
echo ""
echo "[*] Extracting per-module logs..."
for i in $(seq -w 1 13); do
    modlog="${OUTPUT_DIR}/module_${i}.log"
    grep "Module ${i#0}" "$LOG" > "$modlog" 2>/dev/null || true
    lines=$(wc -l < "$modlog")
    echo "  ${modlog}: ${lines} lines"
done

exit "$FAIL"
