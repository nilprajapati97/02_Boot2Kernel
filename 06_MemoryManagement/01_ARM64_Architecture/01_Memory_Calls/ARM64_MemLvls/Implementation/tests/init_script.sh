#!/bin/sh
# init_script.sh — BusyBox initramfs /init script
# Loads all MM inspection modules in order and captures dmesg
#
# This is placed at /init inside the initramfs (cpio archive).

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo "========================================="
echo " ARM64 Memory Management Inspector"
echo " SDM660 / QEMU cortex-a72"
echo "========================================="
echo ""

# Kernel version
echo "Kernel: $(uname -r)"
echo "Machine: $(uname -m)"
echo ""

# Check if modules directory exists
MODDIR="/modules"
if [ ! -d "$MODDIR" ]; then
    echo "[ERROR] /modules directory not found!"
    echo "        Make sure initramfs includes modules."
    exec /bin/sh
fi

# Load modules in phase order
MODULES="
01_mmu_state_inspect
02_idmap_walk
03_pagetable_walk
04_ttbr_translate
05_kernel_segments
06_boot_info
07_fixmap_inspect
08_memblock_dump
09_linear_map_verify
10_zone_inspect
11_buddy_inspect
12_slab_inspect
13_vmalloc_inspect
"

PASS=0
FAIL=0

for mod in $MODULES; do
    modfile="${MODDIR}/${mod}.ko"
    if [ -f "$modfile" ]; then
        echo "--- Loading: ${mod} ---"
        if insmod "$modfile" 2>/dev/null; then
            PASS=$((PASS + 1))
            echo "    [OK] ${mod} loaded"
        else
            FAIL=$((FAIL + 1))
            echo "    [FAIL] ${mod} failed to load"
        fi
    else
        echo "    [SKIP] ${modfile} not found"
    fi
    echo ""
done

echo "========================================="
echo " Results: ${PASS} loaded, ${FAIL} failed"
echo "========================================="
echo ""

# Dump full dmesg for analysis
echo "--- Full kernel log follows ---"
dmesg

# Export per-phase logs
echo ""
echo "--- Exporting /proc info ---"
echo "== /proc/meminfo =="
cat /proc/meminfo
echo ""
echo "== /proc/buddyinfo =="
cat /proc/buddyinfo
echo ""
echo "== /proc/pagetypeinfo (first 50 lines) =="
head -50 /proc/pagetypeinfo
echo ""
echo "== /proc/vmallocinfo (first 30 lines) =="
head -30 /proc/vmallocinfo
echo ""

# Page table dump if debugfs available
if [ -d /sys/kernel/debug ]; then
    mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
    if [ -f /sys/kernel/debug/kernel_page_tables ]; then
        echo "== kernel_page_tables (first 100 lines) =="
        head -100 /sys/kernel/debug/kernel_page_tables
    fi
fi

echo ""
echo "Dropping to shell. Type 'dmesg | grep mi_' to review."
echo ""
exec /bin/sh
