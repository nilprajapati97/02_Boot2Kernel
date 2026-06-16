#!/bin/bash
# create_rootfs.sh — Build minimal BusyBox initramfs with MM inspection modules
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPL_DIR="$(dirname "$SCRIPT_DIR")"
ROOTFS_DIR="$IMPL_DIR/rootfs"
BUSYBOX_VER="1.36.1"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"

echo "=== Creating ARM64 initramfs ==="

# ── Step 1: Build BusyBox ───────────────────────────────────────────
BUSYBOX_DIR="$IMPL_DIR/busybox-$BUSYBOX_VER"

if [ ! -f "$BUSYBOX_DIR/busybox" ]; then
    echo "[1/4] Building BusyBox $BUSYBOX_VER..."

    if [ ! -d "$BUSYBOX_DIR" ]; then
        TARBALL="busybox-${BUSYBOX_VER}.tar.bz2"
        if [ ! -f "$IMPL_DIR/$TARBALL" ]; then
            wget -P "$IMPL_DIR" "https://busybox.net/downloads/$TARBALL"
        fi
        tar -xf "$IMPL_DIR/$TARBALL" -C "$IMPL_DIR"
    fi

    cd "$BUSYBOX_DIR"
    make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" defconfig
    # Enable static build
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)"
else
    echo "[1/4] BusyBox already built."
fi

# ── Step 2: Create rootfs directory structure ───────────────────────
echo "[2/4] Creating rootfs structure..."
rm -rf "$ROOTFS_DIR"
mkdir -p "$ROOTFS_DIR"/{bin,sbin,etc,proc,sys,dev,tmp,var/log,lib/modules}

cd "$BUSYBOX_DIR"
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
    CONFIG_PREFIX="$ROOTFS_DIR" install

# ── Step 3: Copy kernel modules ─────────────────────────────────────
echo "[3/4] Copying inspection modules..."
if ls "$IMPL_DIR"/modules/*.ko 1>/dev/null 2>&1; then
    cp "$IMPL_DIR"/modules/*.ko "$ROOTFS_DIR/lib/modules/"
    echo "  Copied $(ls "$IMPL_DIR"/modules/*.ko | wc -l) modules"
else
    echo "  WARNING: No .ko files found in modules/. Build them first: make modules"
fi

# Copy test scripts
cp "$IMPL_DIR/tests/init_script.sh" "$ROOTFS_DIR/init"
chmod +x "$ROOTFS_DIR/init"

# ── Step 4: Create cpio archive ─────────────────────────────────────
echo "[4/4] Creating initramfs cpio..."
cd "$ROOTFS_DIR"
find . | cpio -o -H newc 2>/dev/null | gzip > "$IMPL_DIR/rootfs.cpio.gz"

echo ""
echo "=== Rootfs created: $IMPL_DIR/rootfs.cpio.gz ==="
echo "  Size: $(du -h "$IMPL_DIR/rootfs.cpio.gz" | cut -f1)"
echo "Next: ./scripts/run_qemu.sh"
