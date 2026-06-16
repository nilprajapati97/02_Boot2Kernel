#!/bin/bash

set -e

echo "========================================="
echo "  Quick Start: Custom Slab Cache QEMU"
echo "========================================="
echo ""
echo "This will:"
echo "1. Setup QEMU ARM64 environment"
echo "2. Build the kernel module"
echo "3. Create root filesystem"
echo "4. Run in QEMU"
echo ""
read -p "Continue? (y/n) " -n 1 -r
echo ""

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    exit 0
fi

# Make scripts executable
chmod +x setup_qemu.sh build_module.sh create_rootfs.sh run_qemu.sh

# Run all steps
./setup_qemu.sh
./build_module.sh
./create_rootfs.sh
./run_qemu.sh
