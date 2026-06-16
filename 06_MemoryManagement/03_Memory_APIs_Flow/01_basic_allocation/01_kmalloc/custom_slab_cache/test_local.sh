#!/bin/bash

set -e

echo "========================================="
echo "  Testing Custom Slab Cache Locally"
echo "========================================="
echo ""

# Build the module for current system
echo "[1/3] Building module for current system..."
make clean 2>/dev/null || true
make

if [ ! -f "custom_slab.ko" ]; then
    echo "Error: Module build failed"
    exit 1
fi

echo ""
echo "[2/3] Module built successfully:"
file custom_slab.ko
modinfo custom_slab.ko

echo ""
echo "[3/3] Loading and testing module..."
echo "Note: This requires sudo privileges"
echo ""

sudo dmesg -C  # Clear dmesg

echo "Loading module..."
sudo insmod custom_slab.ko

echo ""
echo "=== Module Output (dmesg) ==="
sudo dmesg | tail -20

echo ""
echo "=== Checking /proc/slabinfo ==="
sudo cat /proc/slabinfo | grep my_cache || echo "Cache not found (may have been destroyed)"

echo ""
echo "Unloading module..."
sudo rmmod custom_slab

echo ""
echo "=== Final dmesg ==="
sudo dmesg | tail -5

echo ""
echo "========================================="
echo "  Test Complete!"
echo "========================================="
