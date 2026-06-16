#!/bin/bash
# build_kernel.sh — Download Linux 6.12 and cross-compile for ARM64
# Target: SDM660 (Kryo 260) simulated on QEMU virt + cortex-a72
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPL_DIR="$(dirname "$SCRIPT_DIR")"
KERNEL_VER="6.12"
KERNEL_DIR="$IMPL_DIR/linux-$KERNEL_VER"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
NPROC="$(nproc)"

echo "=== ARM64 Kernel Build for QEMU (SDM660 proxy) ==="
echo "  Kernel:  Linux $KERNEL_VER"
echo "  Cross:   $CROSS_COMPILE"
echo "  Jobs:    $NPROC"
echo ""

# ── Step 1: Check toolchain ─────────────────────────────────────────
if ! command -v "${CROSS_COMPILE}gcc" &>/dev/null; then
    echo "ERROR: Cross compiler not found: ${CROSS_COMPILE}gcc"
    echo "Install: sudo apt install gcc-aarch64-linux-gnu"
    exit 1
fi

echo "[1/5] Toolchain: $(${CROSS_COMPILE}gcc --version | head -1)"

# ── Step 2: Download kernel if needed ────────────────────────────────
if [ ! -d "$KERNEL_DIR" ]; then
    echo "[2/5] Downloading Linux $KERNEL_VER..."
    TARBALL="linux-${KERNEL_VER}.tar.xz"
    if [ ! -f "$IMPL_DIR/$TARBALL" ]; then
        wget -P "$IMPL_DIR" "https://cdn.kernel.org/pub/linux/kernel/v6.x/$TARBALL"
    fi
    echo "  Extracting..."
    tar -xf "$IMPL_DIR/$TARBALL" -C "$IMPL_DIR"
    echo "  Done."
else
    echo "[2/5] Kernel source already present: $KERNEL_DIR"
fi

# ── Step 3: Configure kernel ────────────────────────────────────────
echo "[3/5] Configuring kernel..."
cd "$KERNEL_DIR"

# Start with QEMU virt defconfig
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" defconfig

# Apply our MM-debug config fragment
"$KERNEL_DIR/scripts/kconfig/merge_config.sh" \
    -m .config "$IMPL_DIR/configs/kernel.config"

# Ensure all options are resolved
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" olddefconfig

echo "  Config written to $KERNEL_DIR/.config"

# ── Step 4: Build kernel image ──────────────────────────────────────
echo "[4/5] Building kernel (this takes a while)..."
make ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" -j"$NPROC" Image

if [ ! -f "$KERNEL_DIR/arch/arm64/boot/Image" ]; then
    echo "ERROR: Kernel Image not found after build!"
    exit 1
fi

# Copy artifacts to Implementation root
cp "$KERNEL_DIR/arch/arm64/boot/Image" "$IMPL_DIR/Image"
cp "$KERNEL_DIR/vmlinux" "$IMPL_DIR/vmlinux"
cp "$KERNEL_DIR/System.map" "$IMPL_DIR/System.map"

echo "  Image:      $IMPL_DIR/Image"
echo "  vmlinux:    $IMPL_DIR/vmlinux (with debug symbols)"
echo "  System.map: $IMPL_DIR/System.map"

# ── Step 5: Build modules ──────────────────────────────────────────
echo "[5/5] Building inspection modules..."
make -C "$IMPL_DIR" modules KDIR="$KERNEL_DIR"

echo ""
echo "=== Build complete ==="
echo "Next: ./scripts/create_rootfs.sh"
