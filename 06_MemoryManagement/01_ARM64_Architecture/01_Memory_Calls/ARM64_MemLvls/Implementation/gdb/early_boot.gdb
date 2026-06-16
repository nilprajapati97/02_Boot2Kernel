# early_boot.gdb — GDB script for ARM64 early boot register inspection
# Usage: gdb-multiarch -x early_boot.gdb
#
# Requires: QEMU launched with -S -s (halted, GDB on :1234)
#           vmlinux with debug symbols in Implementation/ directory

set architecture aarch64
set pagination off
set confirm off

# Connect to QEMU GDB stub
target remote localhost:1234

# Load symbols from vmlinux
# Adjust path if needed
file vmlinux

printf "\n"
printf "=============================================\n"
printf " ARM64 Early Boot — Register Inspector\n"
printf " Target: SDM660 proxy (cortex-a72 on QEMU)\n"
printf "=============================================\n\n"

# ── Helper: decode SCTLR_EL1 bits ──────────────────────────────────
define decode_sctlr
    set $val = $arg0
    printf "  SCTLR_EL1 = 0x%016lx\n", $val
    printf "    M    [0]  = %d  (MMU %s)\n",    ($val >> 0) & 1, ($val & 1) ? "ON" : "OFF"
    printf "    A    [1]  = %d  (Alignment check)\n", ($val >> 1) & 1
    printf "    C    [2]  = %d  (D-cache %s)\n", ($val >> 2) & 1, (($val >> 2) & 1) ? "ON" : "OFF"
    printf "    SA   [3]  = %d  (SP alignment EL1)\n", ($val >> 3) & 1
    printf "    SA0  [4]  = %d  (SP alignment EL0)\n", ($val >> 4) & 1
    printf "    I    [12] = %d  (I-cache %s)\n", ($val >> 12) & 1, (($val >> 12) & 1) ? "ON" : "OFF"
    printf "    DZE  [14] = %d  (DC ZVA at EL0)\n", ($val >> 14) & 1
    printf "    nTWE [18] = %d\n", ($val >> 18) & 1
    printf "    WXN  [19] = %d  (Write-Execute Never)\n", ($val >> 19) & 1
    printf "    E0E  [24] = %d  (EL0 endianness)\n", ($val >> 24) & 1
    printf "    EE   [25] = %d  (EL1 endianness: %s)\n", ($val >> 25) & 1, (($val >> 25) & 1) ? "BE" : "LE"
    printf "    EnIA [31] = %d  (PAC IA)\n", ($val >> 31) & 1
end

# ── Helper: decode TCR_EL1 fields ──────────────────────────────────
define decode_tcr
    set $val = $arg0
    printf "  TCR_EL1 = 0x%016lx\n", $val
    set $t0sz = $val & 0x3f
    set $t1sz = ($val >> 16) & 0x3f
    set $tg0  = ($val >> 14) & 0x3
    set $tg1  = ($val >> 30) & 0x3
    set $ips  = ($val >> 32) & 0x7
    set $sh0  = ($val >> 12) & 0x3
    set $sh1  = ($val >> 28) & 0x3
    set $ha   = ($val >> 39) & 1
    set $hd   = ($val >> 40) & 1
    printf "    T0SZ    [5:0]   = %d  → TTBR0 VA bits = %d\n", $t0sz, 64 - $t0sz
    printf "    EPD0    [7]     = %d  (TTBR0 walk %s)\n", ($val >> 7) & 1, (($val >> 7) & 1) ? "DISABLED" : "enabled"
    printf "    IRGN0   [9:8]   = %d  (inner cache for TTBR0 walks)\n", ($val >> 8) & 0x3
    printf "    ORGN0   [11:10] = %d  (outer cache for TTBR0 walks)\n", ($val >> 10) & 0x3
    printf "    SH0     [13:12] = %d  (shareability TTBR0: %s)\n", $sh0, ($sh0 == 3) ? "Inner" : ($sh0 == 2) ? "Outer" : "None"
    printf "    TG0     [15:14] = %d  (TTBR0 granule: %s)\n", $tg0, ($tg0 == 0) ? "4KB" : ($tg0 == 1) ? "64KB" : "16KB"
    printf "    T1SZ    [21:16] = %d  → TTBR1 VA bits = %d\n", $t1sz, 64 - $t1sz
    printf "    A1      [22]    = %d  (ASID from TTBR%d)\n", ($val >> 22) & 1, ($val >> 22) & 1
    printf "    EPD1    [23]    = %d  (TTBR1 walk %s)\n", ($val >> 23) & 1, (($val >> 23) & 1) ? "DISABLED" : "enabled"
    printf "    IRGN1   [25:24] = %d  (inner cache for TTBR1 walks)\n", ($val >> 24) & 0x3
    printf "    ORGN1   [27:26] = %d  (outer cache for TTBR1 walks)\n", ($val >> 26) & 0x3
    printf "    SH1     [29:28] = %d  (shareability TTBR1: %s)\n", $sh1, ($sh1 == 3) ? "Inner" : ($sh1 == 2) ? "Outer" : "None"
    printf "    TG1     [31:30] = %d  (TTBR1 granule: %s)\n", $tg1, ($tg1 == 2) ? "4KB" : ($tg1 == 1) ? "16KB" : "64KB"
    printf "    IPS     [34:32] = %d  (PA size: %s bits)\n", $ips, ($ips == 0) ? "32" : ($ips == 1) ? "36" : ($ips == 2) ? "40" : ($ips == 3) ? "42" : ($ips == 4) ? "44" : ($ips == 5) ? "48" : "52"
    printf "    AS      [36]    = %d  (ASID size: %s)\n", ($val >> 36) & 1, (($val >> 36) & 1) ? "16-bit" : "8-bit"
    printf "    HA      [39]    = %d  (HW Access Flag %s)\n", $ha, $ha ? "ON" : "OFF"
    printf "    HD      [40]    = %d  (HW Dirty Bit %s)\n", $hd, $hd ? "ON" : "OFF"
    printf "    DS      [59]    = %d  (Descriptor Size / LPA2)\n", ($val >> 59) & 1
end

# ── Helper: decode MAIR_EL1 attributes ─────────────────────────────
define decode_mair
    set $val = $arg0
    printf "  MAIR_EL1 = 0x%016lx\n", $val
    set $i = 0
    while $i < 8
        set $attr = ($val >> ($i * 8)) & 0xff
        printf "    Attr%d = 0x%02x  ", $i, $attr
        if $attr == 0x00
            printf "(Device-nGnRnE)\n"
        end
        if $attr == 0x04
            printf "(Device-nGnRE)\n"
        end
        if $attr == 0x0c
            printf "(Device-GRE)\n"
        end
        if $attr == 0x44
            printf "(Normal Non-Cacheable)\n"
        end
        if $attr == 0xff
            printf "(Normal WB-RW-Alloc — MT_NORMAL)\n"
        end
        if $attr == 0xbb
            printf "(Normal Write-Through — MT_NORMAL_WT)\n"
        end
        if $attr == 0xf0
            printf "(Normal Tagged — MT_NORMAL_TAG)\n"
        end
        if $attr != 0x00 && $attr != 0x04 && $attr != 0x0c && $attr != 0x44 && $attr != 0xff && $attr != 0xbb && $attr != 0xf0
            printf "(custom: outer=0x%x inner=0x%x)\n", ($attr >> 4) & 0xf, $attr & 0xf
        end
        set $i = $i + 1
    end
end

# ── Helper: decode TTBR ────────────────────────────────────────────
define decode_ttbr
    set $val = $arg0
    set $name = $arg1
    printf "  %s = 0x%016lx\n", $name, $val
    printf "    BADDR [47:1] = 0x%lx  (page table physical base)\n", $val & 0x0000ffffffffffff
    printf "    ASID  [63:48] = %d\n", ($val >> 48) & 0xffff
end

# ══════════════════════════════════════════════════════════════════
# BREAKPOINTS — Phase 1: Primary Entry
# ══════════════════════════════════════════════════════════════════

# Break at record_mmu_state to see initial SCTLR
break record_mmu_state
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 1: record_mmu_state               ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    printf "  x0 (FDT from bootloader) = 0x%016lx\n", $x0
    printf "  CurrentEL = EL%d\n", ($CurrentEL >> 2) & 3
    printf "  PC = 0x%016lx (physical — MMU is OFF)\n", $pc
    continue
end

# Break after record_mmu_state returns
break preserve_boot_args
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 1: preserve_boot_args             ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    printf "  x19 (MMU state) = 0x%016lx  (%s)\n", $x19, $x19 ? "MMU was ON" : "MMU was OFF"
    printf "  x0  (FDT addr)  = 0x%016lx\n", $x0
    continue
end

# ══════════════════════════════════════════════════════════════════
# BREAKPOINTS — Phase 3: CPU Setup
# ══════════════════════════════════════════════════════════════════

break __cpu_setup
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 3: __cpu_setup ENTRY              ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    printf "  About to configure TCR_EL1, MAIR_EL1\n"
    printf "  x19 (MMU state) = 0x%016lx\n", $x19
    printf "  x20 (boot mode) = 0x%016lx\n", $x20
    printf "  x21 (FDT)       = 0x%016lx\n", $x21
    continue
end

# ══════════════════════════════════════════════════════════════════
# BREAKPOINTS — Phase 4: Enable MMU
# ══════════════════════════════════════════════════════════════════

break __enable_mmu
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 4: __enable_mmu                   ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    printf "  x0 (SCTLR value to write):\n"
    decode_sctlr $x0
    printf "\n  x1 (TTBR1 — kernel page table):\n"
    decode_ttbr $x1 "TTBR1"
    printf "\n  x2 (TTBR0 — identity map):\n"
    decode_ttbr $x2 "TTBR0"
    continue
end

# ══════════════════════════════════════════════════════════════════
# BREAKPOINTS — Phase 6: Primary Switched
# ══════════════════════════════════════════════════════════════════

break __primary_switched
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 6: __primary_switched             ║\n"
    printf "║  (Running at kernel virtual address!)    ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    printf "  PC   = 0x%016lx  (kernel VA)\n", $pc
    printf "  SP   = 0x%016lx\n", $sp
    printf "  x20  = 0x%016lx  (boot mode)\n", $x20
    printf "  x21  = 0x%016lx  (FDT phys)\n", $x21
    continue
end

# ══════════════════════════════════════════════════════════════════
# BREAKPOINTS — Phase 7+: C code
# ══════════════════════════════════════════════════════════════════

break start_kernel
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  start_kernel() — C CODE BEGINS          ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    printf "  SP = 0x%016lx\n", $sp
    printf "  PC = 0x%016lx\n", $pc
    continue
end

break setup_arch
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 7: setup_arch()                   ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    continue
end

break arm64_memblock_init
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 8: arm64_memblock_init()          ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    continue
end

break paging_init
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 9: paging_init()                  ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    continue
end

break bootmem_init
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 10: bootmem_init()                ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    continue
end

break mm_core_init
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 11: mm_core_init()                ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    continue
end

break kmem_cache_init
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 12: kmem_cache_init()             ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    continue
end

break vmalloc_init
commands
    silent
    printf "\n╔══════════════════════════════════════════╗\n"
    printf "║  Phase 13: vmalloc_init()                ║\n"
    printf "╚══════════════════════════════════════════╝\n"
    continue
end

printf "\n=== Breakpoints set for all 13 phases ===\n"
printf "Type 'continue' to start boot\n\n"
