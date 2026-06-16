#!/bin/bash

set -e

WORK_DIR="$(pwd)/qemu_arm64"
MODULE_DIR="$(pwd)"

echo "=== Building Custom Slab Cache Module for ARM64 ==="

if [ ! -d "$WORK_DIR/linux" ]; then
    echo "Error: Kernel source not found. Run ./setup_qemu.sh first"
    exit 1
fi

# Build the module
echo "Building module..."
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     -C $WORK_DIR/linux M=$MODULE_DIR modules

if [ -f "custom_slab.ko" ]; then
    echo ""
    echo "=== Module built successfully ==="
    file custom_slab.ko
    echo ""
    echo "Next: Run ./create_rootfs.sh to create root filesystem"
else
    echo "Error: Module build failed"
    exit 1
fi
