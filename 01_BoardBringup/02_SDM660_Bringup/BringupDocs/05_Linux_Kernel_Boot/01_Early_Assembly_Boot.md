# Early Assembly Boot — head.S

## Overview

When ABL jumps to the kernel entry point, execution begins in architecture-specific assembly code: `arch/arm64/kernel/head.S`. This code runs with the MMU disabled, in EL2 or EL1, on the primary CPU (CPU0) only. It sets up the minimal environment needed to jump to C code.

---

## CPU State at Kernel Entry

```
State when ABL jumps to kernel:
─────────────────────────────────
  CPU:     Core 0 (Silver cluster, Cortex-A53)
  EL:      EL2 (if HYP available) or EL1
  MMU:     OFF
  Caches:  OFF (data), ON (instruction may be)
  x0:      DTB physical address (e.g., 0x82000000)
  x1-x3:   0 (reserved)
  UART:    Configured by ABL (115200 baud, 8N1)
  DDR:     Fully trained and available
  Clocks:  XBL-configured (boot frequencies)
  PMIC:    All regulators at boot voltages
```

---

## head.S Execution Flow

```
Entry point: _head (arch/arm64/kernel/head.S)
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│              head.S Execution Flow                        │
│                                                          │
│  1. Preserve DTB pointer (x0 → x21)                     │
│     └── Save for later use during DT processing          │
│                                                          │
│  2. EL2 → EL1 transition (if entered at EL2)            │
│     ├── Configure EL2: HCR_EL2 (Hypervisor Config Reg)  │
│     ├── Set up VBAR_EL2 (exception vectors)              │
│     ├── Configure virtual timer offset                   │
│     ├── Set SCTLR_EL1 (System Control Register)         │
│     │   └── Clear bits: MMU off, caches off              │
│     └── ERET to EL1 (exception return)                   │
│                                                          │
│  3. CPU initialization                                   │
│     ├── Set up temporary stack                           │
│     ├── Clear BSS section                                │
│     ├── Configure MAIR_EL1 (Memory Attribute Indirection)│
│     │   └── Define memory types: Normal, Device, etc.    │
│     └── Read CPU ID (MIDR_EL1) for errata workarounds    │
│                                                          │
│  4. Create initial page tables                           │
│     ├── Identity map for kernel code (VA == PA)          │
│     │   └── So code keeps working when MMU turns on      │
│     ├── Map kernel image to PAGE_OFFSET                  │
│     │   └── 0xFFFF_FFC0_0000_0000 + PA offset (typical)  │
│     ├── Map FDT (Flattened Device Tree)                  │
│     └── Set TTBR0_EL1 and TTBR1_EL1 (page table base)   │
│                                                          │
│  5. Enable MMU                                           │
│     ├── Configure TCR_EL1 (Translation Control Register) │
│     │   ├── T0SZ, T1SZ (VA size: 48-bit)               │
│     │   ├── Granule size: 4 KB pages                    │
│     │   └── Inner/outer cacheability                     │
│     ├── Set SCTLR_EL1.M = 1 (enable MMU)                │
│     ├── Set SCTLR_EL1.C = 1 (enable data cache)         │
│     ├── Set SCTLR_EL1.I = 1 (enable instruction cache)  │
│     ├── ISB (Instruction Synchronization Barrier)        │
│     └── *** MMU IS NOW ON — virtual addresses active *** │
│                                                          │
│  6. Jump to start_kernel() (C code)                      │
│     ├── Adjust stack pointer to virtual address          │
│     ├── Set up function arguments                        │
│     └── BL start_kernel (init/main.c)                    │
│         └── *** NEVER RETURNS ***                        │
└──────────────────────────────────────────────────────────┘
```

---

## Page Table Setup Details

```
ARM64 4-level page tables (48-bit VA, 4 KB granule):
─────────────────────────────────────────────────────
Level 0 (PGD): 512 entries, each covers 512 GB
Level 1 (PUD): 512 entries, each covers 1 GB
Level 2 (PMD): 512 entries, each covers 2 MB (block mapping used here)
Level 3 (PTE): 512 entries, each covers 4 KB

Initial mapping (head.S creates minimal tables):
─────────────────────────────────────────────────
TTBR0_EL1 (user-space / identity):
  Maps kernel physical range 1:1
  PA 0x80080000 → VA 0x80080000 (identity)
  
TTBR1_EL1 (kernel-space):
  Maps kernel to high virtual address
  PA 0x80080000 → VA 0xFFFFFF8008080000 (kernel linear map)
  
Both use 2 MB block mappings for efficiency (PMD-level)
```

---

## EL2 to EL1 Drop

```
Reason: Kernel normally runs at EL1
─────────────────────────────────────
ABL/TZ may leave CPU at EL2 (hypervisor level)
head.S configures EL2 and drops to EL1:

1. HCR_EL2 |= HCR_RW     → EL1 runs in AArch64 mode
2. SPSR_EL2 = EL1h        → Target is EL1 with SP_EL1
3. ELR_EL2 = el1_entry    → Return address in EL1
4. ERET                    → Drop to EL1 at el1_entry

Note: If KVM hypervisor is enabled:
  EL2 is retained for KVM
  head.S configures EL2 for KVM use
  Kernel still runs at EL1 as guest
```

---

## Early Console (earlycon)

Before the full UART driver is loaded, the kernel can output messages using **earlycon**:

```
Kernel cmdline (set by ABL):
  earlycon=msm_serial_dm,0x78af000

head.S flow:
  1. start_kernel() → setup_arch() → setup_early_printk()
  2. Maps earlycon MMIO address (0x78af000) via fixmap
  3. Earliest printk output available (UART register writes)
  4. Later replaced by full serial driver when it probes
```

---

## SDM660-Specific CPU Errata

```c
/* arch/arm64/kernel/cpu_errata.c */

/* Cortex-A53 errata applied during head.S */
static const struct arm64_cpu_capabilities arm64_errata[] = {
    {
        /* Cortex-A53: 835769 - possible data corruption */
        .desc = "ARM erratum 835769",
        .capability = ARM64_WORKAROUND_835769,
        ERRATA_MIDR_ALL_VERSIONS(MIDR_CORTEX_A53),
    },
    {
        /* Cortex-A53: 843419 - ADRP instruction issue */
        .desc = "ARM erratum 843419",
        .capability = ARM64_WORKAROUND_843419,
        ERRATA_MIDR_ALL_VERSIONS(MIDR_CORTEX_A53),
    },
};

/* These are checked during head.S using MIDR_EL1 register */
```

---

## Key Source Files

| File | Purpose |
|------|---------|
| `arch/arm64/kernel/head.S` | Entry point, page table setup, MMU enable |
| `arch/arm64/kernel/head.S::__primary_switch` | Switch to virtual addresses |
| `arch/arm64/mm/proc.S` | CPU-specific MMU setup |
| `arch/arm64/kernel/setup.c` | setup_arch() — early platform init |
| `init/main.c::start_kernel()` | First C function called |

---

## Related Documents

- [../04_ABL_Android_Bootloader/03_Kernel_DTB_Loading.md](../04_ABL_Android_Bootloader/03_Kernel_DTB_Loading.md) — ABL loads kernel
- [02_Device_Tree_Processing.md](02_Device_Tree_Processing.md) — DTB processing after head.S
- [08_UART_Console.md](08_UART_Console.md) — Early UART console setup
