#!/bin/bash
# run_qemu.sh — Launch QEMU with ARM64 kernel and MM debug output
# Simulates SDM660 (Kryo 260) using virt machine + cortex-a72
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPL_DIR="$(dirname "$SCRIPT_DIR")"

IMAGE="$IMPL_DIR/Image"
INITRD="$IMPL_DIR/rootfs.cpio.gz"
LOGFILE="$IMPL_DIR/qemu_output.log"

if [ ! -f "$IMAGE" ]; then
    echo "ERROR: Kernel Image not found: $IMAGE"
    echo "Run: ./scripts/build_kernel.sh"
    exit 1
fi

if [ ! -f "$INITRD" ]; then
    echo "ERROR: Initramfs not found: $INITRD"
    echo "Run: ./scripts/create_rootfs.sh"
    exit 1
fi

echo "=== QEMU ARM64 — SDM660 Proxy (cortex-a72) ==="
echo "  CPU:    cortex-a72 (8 cores)"
echo "  RAM:    4GB"
echo "  GIC:    v3"
echo "  Log:    $LOGFILE"
echo "  Exit:   Ctrl-A X"
echo ""

qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu cortex-a72 \
    -smp 8 \
    -m 4G \
    -kernel "$IMAGE" \
    -initrd "$INITRD" \
    -append "console=ttyAMA0 earlycon=pl011,0x09000000 \
             memblock=debug loglevel=8 \
             page_owner=on slub_debug=FZP \
             nokaslr printk.devkmsg=on" \
    -nographic \
    -serial mon:stdio \
    -no-reboot \
    2>&1 | tee "$LOGFILE"
