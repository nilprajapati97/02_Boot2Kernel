#!/bin/bash
# test_registers.sh — Validate register bit fields from log output
#
# Parses QEMU output log for register values and validates expected
# bit fields for cortex-a72 on QEMU virt machine.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPL_DIR="$(dirname "$SCRIPT_DIR")"
LOG="${1:-${IMPL_DIR}/output/qemu_test_output.log}"

if [ ! -f "$LOG" ]; then
    echo "[ERROR] Log file not found: $LOG"
    echo "Usage: $0 [logfile]"
    exit 1
fi

echo "========================================="
echo " Register Bit-Field Validation"
echo " Expected: QEMU virt + cortex-a72"
echo "========================================="
echo ""

PASS=0
FAIL=0

check() {
    local desc="$1"
    local pattern="$2"

    if grep -qE "$pattern" "$LOG" 2>/dev/null; then
        echo "  [PASS] $desc"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc (pattern: $pattern)"
        FAIL=$((FAIL + 1))
    fi
}

echo "--- SCTLR_EL1 ---"
check "MMU enabled (M=1)"          "M.*=.*1"
check "Data cache (C=1)"           "C.*=.*1"
check "Instruction cache (I=1)"    "I.*=.*1"
check "SP alignment (SA=1)"        "SA.*=.*1"
check "WXN set"                    "WXN"

echo ""
echo "--- TCR_EL1 ---"
check "T0SZ present"               "T0SZ"
check "T1SZ present"               "T1SZ"
check "TG0 (granule)"              "TG0"
check "TG1 (granule)"              "TG1"
check "IPS (PA size)"              "IPS"
check "Inner shareable"            "Inner Shareable"

echo ""
echo "--- MAIR_EL1 ---"
check "MAIR decoded"               "MAIR_EL1"
check "Attr0 present"              "Attr\\[0\\]"
check "Normal memory attr"         "Normal\\|Write-Back\\|Cacheable"

echo ""
echo "--- TTBR ---"
check "TTBR0_EL1 present"          "TTBR0_EL1"
check "TTBR1_EL1 present"          "TTBR1_EL1"
check "BADDR field"                "BADDR"

echo ""
echo "--- MIDR_EL1 ---"
check "Implementer ARM (0x41)"     "Implementer.*0x41\\|ARM"
check "PartNum cortex-a72 (0xd08)" "0xd08\\|Cortex-A72"

echo ""
echo "--- ID_AA64MMFR0_EL1 ---"
check "PARange present"            "PARange"
check "TGran4 supported"           "TGran4"

echo ""
echo "--- Memory Layout ---"
check "PAGE_OFFSET"                "PAGE_OFFSET"
check "VMALLOC_START"              "VMALLOC_START"
check "memstart_addr"              "memstart_addr"
check "kimage_voffset"             "kimage_voffset"

echo ""
echo "--- Allocation Tests ---"
check "alloc_pages pass"           "alloc_pages.*PASS\\|alloc_pages.*✓"
check "kmalloc test"               "kmalloc.*→"
check "vmalloc test"               "vmalloc.*="

echo ""
echo "========================================="
echo " Summary: $PASS passed, $FAIL failed"
echo "========================================="

exit "$FAIL"
