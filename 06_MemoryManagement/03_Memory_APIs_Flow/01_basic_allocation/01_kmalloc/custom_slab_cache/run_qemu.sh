#!/bin/bash

WORK_DIR="$(pwd)/qemu_arm64"

echo "=== Starting QEMU ARM64 ==="

if [ ! -f "$WORK_DIR/linux/arch/arm64/boot/Image" ]; then
    echo "Error: Kernel image not found. Run ./setup_qemu.sh first"
    exit 1
fi

if [ ! -f "$WORK_DIR/rootfs.cpio.gz" ]; then
    echo "Error: Root filesystem not found. Run ./create_rootfs.sh first"
    exit 1
fi

echo "Booting ARM64 kernel with custom slab cache module..."
echo ""

qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a57 \
    -smp 2 \
    -m 1G \
    -kernel $WORK_DIR/linux/arch/arm64/boot/Image \
    -initrd $WORK_DIR/rootfs.cpio.gz \
    -append "console=ttyAMA0 rdinit=/init" \
    -nographic \
    -no-reboot

echo ""
echo "=== QEMU Exited ==="
