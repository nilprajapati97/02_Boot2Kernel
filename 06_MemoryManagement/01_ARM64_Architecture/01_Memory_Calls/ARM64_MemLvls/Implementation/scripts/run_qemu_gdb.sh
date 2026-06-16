#!/bin/bash
# run_qemu_gdb.sh — Launch QEMU halted, waiting for GDB connection
# Use with: gdb-multiarch -x gdb/early_boot.gdb
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPL_DIR="$(dirname "$SCRIPT_DIR")"

IMAGE="$IMPL_DIR/Image"
INITRD="$IMPL_DIR/rootfs.cpio.gz"
TRACE_LOG="$IMPL_DIR/qemu_mmu_trace.log"

if [ ! -f "$IMAGE" ]; then
    echo "ERROR: Kernel Image not found. Run: ./scripts/build_kernel.sh"
    exit 1
fi

echo "=== QEMU ARM64 — GDB Debug Mode ==="
echo "  GDB stub:   localhost:1234"
echo "  CPU halted:  YES (waiting for GDB 'continue')"
echo "  MMU trace:   $TRACE_LOG"
echo ""
echo "  Connect GDB:"
echo "    gdb-multiarch -x $IMPL_DIR/gdb/early_boot.gdb"
echo ""

INITRD_OPT=""
if [ -f "$INITRD" ]; then
    INITRD_OPT="-initrd $INITRD"
fi

qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu cortex-a72 \
    -smp 8 \
    -m 4G \
    -kernel "$IMAGE" \
    $INITRD_OPT \
    -append "console=ttyAMA0 earlycon=pl011,0x09000000 \
             memblock=debug loglevel=8 nokaslr" \
    -nographic \
    -serial mon:stdio \
    -no-reboot \
    -d mmu,int \
    -D "$TRACE_LOG" \
    -S -s
