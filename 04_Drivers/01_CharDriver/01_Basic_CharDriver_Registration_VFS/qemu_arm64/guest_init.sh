#!/bin/sh
# =============================================================================
# guest_init.sh — /init inside the QEMU ARM64 initramfs
# =============================================================================
# This script runs as PID 1 inside the guest.
# It mounts essential filesystems, tests basic_chardriver, then drops to shell.
# =============================================================================

# ── Mount essential filesystems ───────────────────────────────────────────────
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || mount -t tmpfs tmpfs /dev

# stdin/stdout/stderr
[ -c /dev/console ] && exec </dev/console >/dev/console 2>/dev/console

echo ""
echo "╔═══════════════════════════════════════════════════╗"
echo "║   Basic Char Driver Test  —  ARM64 QEMU Guest     ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""

MODULE=/lib/modules/basic_chardriver.ko
DEV=/dev/MyAnilDev

# ─────────────────────────────────────────────────────────────────────────────
# 1. Load the module
# ─────────────────────────────────────────────────────────────────────────────
echo "─── [1] insmod basic_chardriver.ko ─────────────────────"
insmod "${MODULE}"
echo "    insmod returned: $?"
sleep 0.2          # give udev/devtmpfs a moment

# ─────────────────────────────────────────────────────────────────────────────
# 2. Verify registration in /proc/devices
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "─── [2] /proc/devices (char section) ───────────────────"
cat /proc/devices

# ─────────────────────────────────────────────────────────────────────────────
# 3. Device node — devtmpfs creates it automatically;
#    fall back to manual mknod if needed
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "─── [3] Device node check ───────────────────────────────"
if [ -c "${DEV}" ]; then
    echo "    ${DEV} exists (auto-created by devtmpfs)."
else
    MAJOR=$(awk '/basic_chrdev/{print $1}' /proc/devices)
    if [ -n "${MAJOR}" ]; then
        echo "    Creating ${DEV} manually (major=${MAJOR})"
        mknod "${DEV}" c "${MAJOR}" 0
    else
        echo "    ERROR: could not find major number for basic_chrdev"
        echo "    Dumping dmesg for diagnosis:"
        dmesg | tail -20
        echo ""
        echo "Dropping to shell for manual debug."
        exec /bin/sh
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 4. Write to the device
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "─── [4] Write: echo 'HelloFromARMGuest' > ${DEV} ───────"
echo "HelloFromARMGuest" > "${DEV}"
echo "    write returned: $?"

# ─────────────────────────────────────────────────────────────────────────────
# 5. Read back from the device
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "─── [5] Read back from ${DEV} ───────────────────────────"
DATA=$(cat "${DEV}")
echo "    read: '${DATA}'"

# Verify round-trip
if echo "${DATA}" | grep -q "HelloFromARMGuest"; then
    echo "    PASS: read/write round-trip verified."
else
    echo "    MISMATCH: data does not match what was written."
fi

# ─────────────────────────────────────────────────────────────────────────────
# 6. Kernel log — shows driver pr_info() messages
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "─── [6] dmesg — basic_chrdev messages ──────────────────"
dmesg | grep basic_chrdev

# ─────────────────────────────────────────────────────────────────────────────
# 7. Remove the module
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "─── [7] rmmod basic_chardriver ─────────────────────────"
rmmod basic_chardriver
echo "    rmmod returned: $?"

echo ""
echo "─── [8] dmesg after rmmod ───────────────────────────────"
dmesg | tail -6

echo ""
echo "╔═══════════════════════════════════════════════════╗"
echo "║   Test Complete!                                  ║"
echo "║   Interactive shell below — type 'poweroff' exit ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""

# Drop to interactive shell so you can inspect /proc, dmesg, etc.
exec /bin/sh
