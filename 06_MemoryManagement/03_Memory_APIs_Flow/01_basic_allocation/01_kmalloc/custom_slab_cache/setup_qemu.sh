#!/bin/bash

set -e

WORK_DIR="$(pwd)/qemu_arm64"
KERNEL_VERSION="6.1"
BUSYBOX_VERSION="1.36.1"

echo "=== QEMU ARM64 Setup for Custom Slab Cache ==="

# Create working directory
mkdir -p $WORK_DIR
cd $WORK_DIR

# Install dependencies
echo "[1/5] Checking dependencies..."
sudo apt-get update
sudo apt-get install -y qemu-system-aarch64 gcc-aarch64-linux-gnu \
    build-essential libssl-dev bc flex bison libncurses-dev \
    wget cpio

# Download and build Linux kernel for ARM64
if [ ! -d "linux" ]; then
    echo "[2/5] Downloading Linux kernel..."
    wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz
    tar xf linux-${KERNEL_VERSION}.tar.xz
    mv linux-${KERNEL_VERSION} linux
fi

cd linux
if [ ! -f "arch/arm64/boot/Image" ]; then
    echo "[3/5] Building Linux kernel for ARM64..."
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
fi
cd ..

# Download and build BusyBox
if [ ! -d "busybox" ]; then
    echo "[4/5] Downloading BusyBox..."
    wget https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2
    tar xf busybox-${BUSYBOX_VERSION}.tar.bz2
    mv busybox-${BUSYBOX_VERSION} busybox
fi

cd busybox
if [ ! -f "_install/bin/busybox" ]; then
    echo "[5/5] Building BusyBox for ARM64..."
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- install
fi
cd ..

echo ""
echo "=== Setup Complete ==="
echo "Kernel: $WORK_DIR/linux/arch/arm64/boot/Image"
echo "BusyBox: $WORK_DIR/busybox/_install"
echo ""
echo "Next: Run ./build_module.sh to build the kernel module"
