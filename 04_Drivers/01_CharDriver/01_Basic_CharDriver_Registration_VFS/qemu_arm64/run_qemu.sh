#!/usr/bin/env bash
# =============================================================================
# run_qemu.sh — Build the kernel module, pack an initramfs, launch QEMU ARM64
# =============================================================================
# Prerequisites: run setup_env.sh first (once).
#
# What this script does on each invocation:
#   1. Cross-compiles basic_chardriver.ko against the built kernel
#   2. Builds a minimal initramfs:
#        busybox (static) + .ko + guest_init.sh (/init)
#   3. Launches qemu-system-aarch64 with -nographic
#      (exit QEMU with Ctrl-A then X)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMPL_DIR="${SCRIPT_DIR}/../01_Implementation"
ENV_DIR="${SCRIPT_DIR}/env"

KERNEL_BUILD="${ENV_DIR}/linux-build"
KERNEL_IMAGE="${KERNEL_BUILD}/arch/arm64/boot/Image"
BUSYBOX_INSTALL="${ENV_DIR}/busybox-install"

ROOTFS_DIR="${ENV_DIR}/rootfs"
INITRAMFS="${ENV_DIR}/initramfs.cpio.gz"
MODULE_NAME="basic_chardriver"

# ── Colour helpers ────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; CYAN='\033[0;36m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}[RUN]${NC} $*"; }
step() { echo -e "\n${CYAN}── $* ──${NC}"; }
die()  { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

# ── Sanity checks ─────────────────────────────────────────────────────────────
[ -f "${KERNEL_IMAGE}" ] || \
    die "Kernel image not found: ${KERNEL_IMAGE}\n       Run setup_env.sh first."
[ -f "${BUSYBOX_INSTALL}/bin/busybox" ] || \
    die "BusyBox not found: ${BUSYBOX_INSTALL}\n       Run setup_env.sh first."
[ -f "${IMPL_DIR}/basic_chardriver.c" ] || \
    die "Source not found: ${IMPL_DIR}/basic_chardriver.c"

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Cross-compile the kernel module
# ─────────────────────────────────────────────────────────────────────────────
step "Step 1/3 — Cross-compiling ${MODULE_NAME}.ko"

make -C "${IMPL_DIR}" \
    KDIR="${KERNEL_BUILD}" \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu-

log "${MODULE_NAME}.ko built."

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: Build minimal initramfs
#   Layout:
#     /bin, /sbin, /usr      ← busybox
#     /lib/modules/          ← our .ko
#     /proc, /sys, /dev      ← mount points
#     /init                  ← guest_init.sh (entry point)
# ─────────────────────────────────────────────────────────────────────────────
step "Step 2/3 — Building initramfs"

rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}"/{bin,sbin,lib/modules,proc,sys,dev,etc,tmp}

# BusyBox and its symlinks
cp -a "${BUSYBOX_INSTALL}"/* "${ROOTFS_DIR}/"

# Kernel module
cp "${IMPL_DIR}/${MODULE_NAME}.ko" "${ROOTFS_DIR}/lib/modules/"

# Init script (guest entry point)
cp "${SCRIPT_DIR}/guest_init.sh" "${ROOTFS_DIR}/init"
chmod 755 "${ROOTFS_DIR}/init"

# Pack as cpio.gz
log "Packing initramfs → ${INITRAMFS}"
(
    cd "${ROOTFS_DIR}"
    find . | cpio -o -H newc --quiet | gzip -9 > "${INITRAMFS}"
)
log "initramfs size: $(du -sh "${INITRAMFS}" | cut -f1)"

# ─────────────────────────────────────────────────────────────────────────────
# Step 3: Launch QEMU
# ─────────────────────────────────────────────────────────────────────────────
step "Step 3/3 — Launching QEMU (ARM64 virt)"
echo ""
echo "  Machine  : virt  (ARM64)"
echo "  CPU      : cortex-a57"
echo "  Memory   : 512M"
echo "  Kernel   : ${KERNEL_IMAGE}"
echo "  Initramfs: ${INITRAMFS}"
echo ""
echo -e "  ${CYAN}To exit QEMU: Ctrl-A then X${NC}"
echo ""

qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a57 \
    -m 512M \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRAMFS}" \
    -append "console=ttyAMA0 rdinit=/init nokaslr loglevel=3" \
    -nographic \
    -no-reboot
