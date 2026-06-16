#!/bin/bash

set -e

WORK_DIR="$(pwd)/qemu_arm64"
MODULE_DIR="$(pwd)"

echo "=== Creating Root Filesystem ==="

if [ ! -d "$WORK_DIR/busybox/_install" ]; then
    echo "Error: BusyBox not found. Run ./setup_qemu.sh first"
    exit 1
fi

cd $WORK_DIR

# Create rootfs structure
mkdir -p rootfs
cd rootfs
mkdir -p bin sbin etc proc sys dev lib modules

# Copy BusyBox
echo "Copying BusyBox..."
cp -a ../busybox/_install/* .

# Copy kernel module
echo "Copying kernel module..."
cp $MODULE_DIR/custom_slab.ko modules/

# Create init script
echo "Creating init script..."
cat > init << 'EOF'
#!/bin/sh

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

echo ""
echo "==================================="
echo "  ARM64 QEMU - Custom Slab Cache"
echo "==================================="
echo ""

# Load the module
echo "Loading custom_slab module..."
insmod /modules/custom_slab.ko

echo ""
echo "Module output:"
dmesg | tail -15

echo ""
echo "Checking /proc/slabinfo for my_cache:"
cat /proc/slabinfo | grep my_cache || echo "Cache not found in slabinfo"

echo ""
echo "Unloading module..."
rmmod custom_slab

echo ""
echo "Module unloaded. Final dmesg:"
dmesg | tail -5

echo ""
echo "==================================="
echo "Test completed. System will halt."
echo "==================================="

poweroff -f
EOF

chmod +x init

# Create initramfs
echo "Creating initramfs..."
find . | cpio -o -H newc | gzip > ../rootfs.cpio.gz

cd ..
echo ""
echo "=== Root filesystem created ==="
echo "Location: $WORK_DIR/rootfs.cpio.gz"
echo ""
echo "Next: Run ./run_qemu.sh to start QEMU"
