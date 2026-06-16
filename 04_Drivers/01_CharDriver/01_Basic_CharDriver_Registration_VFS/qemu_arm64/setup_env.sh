#!/usr/bin/env bash
# =============================================================================
# setup_env.sh — One-time ARM64 QEMU environment setup
# =============================================================================
# Run this ONCE from WSL2 (Ubuntu 22.04+) before running run_qemu.sh.
#
# What it does:
#   1. Installs apt packages  (qemu, cross-toolchain, kernel build tools)
#   2. Downloads & builds a Linux 6.6 kernel for ARM64 (virt machine)
#   3. Downloads & builds BusyBox (static, ARM64) for the minimal rootfs
#
# Build time estimate (first run):
#   Kernel : ~15-30 min on 4 cores  (skipped if already built)
#   BusyBox: ~2-5  min              (skipped if already built)
#
# After this script succeeds, run:
#   ./run_qemu.sh
# =============================================================================
set -euo pipefail

# ── Configuration — change these if needed ───────────────────────────────────
KERNEL_VERSION="6.6.30"                    # Any stable 6.x.y works
BUSYBOX_VERSION="1.36.1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_DIR="${SCRIPT_DIR}/env"

KERNEL_SRC="${ENV_DIR}/linux-${KERNEL_VERSION}"
KERNEL_BUILD="${ENV_DIR}/linux-build"
KERNEL_IMAGE="${KERNEL_BUILD}/arch/arm64/boot/Image"

BUSYBOX_SRC="${ENV_DIR}/busybox-${BUSYBOX_VERSION}"
BUSYBOX_INSTALL="${ENV_DIR}/busybox-install"

NPROC=$(nproc)

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
log()  { echo -e "${GREEN}[SETUP]${NC} $*"; }
step() { echo -e "\n${CYAN}════════════════════════════════════════${NC}"; \
         echo -e "${CYAN}  $*${NC}"; \
         echo -e "${CYAN}════════════════════════════════════════${NC}"; }
warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
die()  { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

mkdir -p "${ENV_DIR}"

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Install prerequisites
# ─────────────────────────────────────────────────────────────────────────────
step "Step 1/3 — Installing Prerequisites"

sudo apt-get update -qq
sudo apt-get install -y \
    qemu-system-arm \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    libssl-dev libelf-dev \
    bc flex bison \
    wget xz-utils bzip2 cpio gzip file \
    make git ccache

log "Prerequisites installed."

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: Linux kernel — download, configure, build
# ─────────────────────────────────────────────────────────────────────────────
step "Step 2/3 — Linux Kernel ${KERNEL_VERSION} (ARM64)"

if [ -f "${KERNEL_IMAGE}" ]; then
    log "Kernel image already exists — skipping build."
    log "  ${KERNEL_IMAGE}"
else
    # Download source
    if [ ! -d "${KERNEL_SRC}" ]; then
        TARBALL="${ENV_DIR}/linux-${KERNEL_VERSION}.tar.xz"
        log "Downloading linux-${KERNEL_VERSION}.tar.xz ..."
        wget -q --show-progress \
            "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz" \
            -O "${TARBALL}"
        log "Extracting..."
        tar -xf "${TARBALL}" -C "${ENV_DIR}"
        rm "${TARBALL}"
    fi

    # Configure: defconfig + enable devtmpfs (auto /dev nodes) + modules
    log "Configuring kernel (defconfig)..."
    mkdir -p "${KERNEL_BUILD}"
    make -C "${KERNEL_SRC}" \
        O="${KERNEL_BUILD}" \
        ARCH=arm64 \
        CROSS_COMPILE=aarch64-linux-gnu- \
        defconfig

    # Ensure module support and devtmpfs are enabled
    "${KERNEL_SRC}/scripts/config" --file "${KERNEL_BUILD}/.config" \
        --enable CONFIG_MODULES \
        --enable CONFIG_MODULE_UNLOAD \
        --enable CONFIG_DEVTMPFS \
        --enable CONFIG_DEVTMPFS_MOUNT

    # Regenerate .config to resolve new dependencies
    make -C "${KERNEL_SRC}" \
        O="${KERNEL_BUILD}" \
        ARCH=arm64 \
        CROSS_COMPILE=aarch64-linux-gnu- \
        olddefconfig

    # Build — use ccache if available
    CCACHE=""
    command -v ccache &>/dev/null && CCACHE="ccache "
    log "Building kernel with ${NPROC} threads (this takes 15-30 min)..."
    make -C "${KERNEL_SRC}" \
        O="${KERNEL_BUILD}" \
        ARCH=arm64 \
        CROSS_COMPILE="${CCACHE}aarch64-linux-gnu-" \
        -j"${NPROC}" \
        Image

    log "Kernel built: ${KERNEL_IMAGE}"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 3: BusyBox — download, configure (static), build, install
# ─────────────────────────────────────────────────────────────────────────────
step "Step 3/3 — BusyBox ${BUSYBOX_VERSION} (static, ARM64)"

if [ -f "${BUSYBOX_INSTALL}/bin/busybox" ]; then
    log "BusyBox already installed — skipping build."
    log "  ${BUSYBOX_INSTALL}/bin/busybox"
else
    # Download source
    if [ ! -d "${BUSYBOX_SRC}" ]; then
        TARBALL="${ENV_DIR}/busybox-${BUSYBOX_VERSION}.tar.bz2"
        log "Downloading busybox-${BUSYBOX_VERSION}.tar.bz2 ..."
        wget -q --show-progress \
            "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2" \
            -O "${TARBALL}"
        log "Extracting..."
        tar -xf "${TARBALL}" -C "${ENV_DIR}"
        rm "${TARBALL}"
    fi

    # defconfig then enable static linking
    log "Configuring BusyBox (static)..."
    make -C "${BUSYBOX_SRC}" \
        CROSS_COMPILE=aarch64-linux-gnu- \
        defconfig

    # Remove any existing CONFIG_STATIC entry and force it on
    sed -i '/^.*CONFIG_STATIC.*/d' "${BUSYBOX_SRC}/.config"
    echo "CONFIG_STATIC=y" >> "${BUSYBOX_SRC}/.config"

    make -C "${BUSYBOX_SRC}" \
        CROSS_COMPILE=aarch64-linux-gnu- \
        olddefconfig

    log "Building BusyBox..."
    make -C "${BUSYBOX_SRC}" \
        CROSS_COMPILE=aarch64-linux-gnu- \
        -j"${NPROC}"

    log "Installing BusyBox to ${BUSYBOX_INSTALL}..."
    make -C "${BUSYBOX_SRC}" \
        CROSS_COMPILE=aarch64-linux-gnu- \
        CONFIG_PREFIX="${BUSYBOX_INSTALL}" \
        install

    log "BusyBox installed."
fi

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
echo -e "\n${GREEN}════════════════════════════════════════${NC}"
echo -e "${GREEN}  Setup Complete!${NC}"
echo -e "${GREEN}════════════════════════════════════════${NC}"
log "Kernel image : ${KERNEL_IMAGE}"
log "BusyBox      : ${BUSYBOX_INSTALL}/bin/busybox"
echo ""
log "Next step:"
echo -e "    ${CYAN}cd $(dirname "${SCRIPT_DIR}") && ./qemu_arm64/run_qemu.sh${NC}"
echo ""
