# Phase 1 — Assembly Boot (MMU Off → MMU On)

## Overview

This is the very first kernel code that executes after the bootloader jumps to the kernel image. The CPU is running with:

- **MMU: OFF** — all memory accesses use physical addresses
- **D-cache: OFF** — no data caching
- **No allocator** — memory comes from static BSS sections only
- **x0** = physical address of the Device Tree Blob (DTB)

By the end of this phase, the MMU is enabled, the CPU is running at virtual addresses, and control is handed to C code (`start_kernel()`).

## Call Graph

```
primary_entry()                          ← head.S:82  (Entry point)
│
├── record_mmu_state()                   ← Save SCTLR to x19
├── preserve_boot_args()                 ← Save x0-x3 (DTB in x21)
│
├── __pi_create_init_idmap()             ← map_range.c  (Identity page tables)
│   └── map_range()                      ← Build PGD→PUD→PMD→PTE chain
│
├── init_kernel_el()                     ← Configure exception level (EL2→EL1)
│
├── __cpu_setup()                        ← proc.S  (MMU register config)
│   ├── Set MAIR_EL1                     ← Memory attribute types
│   ├── Set TCR_EL1                      ← Translation control
│   └── Prepare SCTLR_EL1               ← System control (MMU enable bits)
│
└── __primary_switch()                   ← head.S  (MMU enable + VA switch)
    ├── __enable_mmu()                   ← Load TTBR0/TTBR1, set SCTLR.M=1
    ├── __pi_early_map_kernel()          ← Map kernel at link address
    └── br x8 → __primary_switched()    ← Jump to virtual address!
        ├── Set sp = init_task stack
        ├── Set VBAR_EL1                 ← Exception vectors
        ├── kasan_early_init()           ← (if KASAN enabled)
        └── bl start_kernel              ← Transition to C code
```

## Memory State

| Item | Source | Description |
|------|--------|-------------|
| `init_idmap_pg_dir` | BSS | Identity-mapped page tables (phys = virt) |
| `init_pg_dir` | BSS | Initial kernel page tables |
| `early_init_stack` | BSS | Early boot stack (before init_task) |
| `swapper_pg_dir` | BSS | Final kernel page tables (populated later in Phase 2) |

**No dynamic memory allocation occurs in this phase.** All page tables and stacks are statically allocated in the kernel image's BSS section.

## Documents

| # | Document | Covers |
|---|----------|--------|
| 01 | [primary_entry.md](01_primary_entry.md) | Entry point, register setup, DTB preservation |
| 02 | [create_init_idmap.md](02_create_init_idmap.md) | Identity page table creation algorithm |
| 03 | [cpu_setup.md](03_cpu_setup.md) | MAIR, TCR, SCTLR hardware register configuration |
| 04 | [enable_mmu_primary_switch.md](04_enable_mmu_primary_switch.md) | MMU enable, physical→virtual transition |
| 05 | [primary_switched.md](05_primary_switched.md) | Stack setup, exception vectors, start_kernel() |

## Key Registers After This Phase

| Register | Value | Purpose |
|----------|-------|---------|
| `SCTLR_EL1.M` | 1 | MMU enabled |
| `TTBR0_EL1` | `init_idmap_pg_dir` | Identity map (user space region) |
| `TTBR1_EL1` | `init_pg_dir` | Kernel page tables |
| `MAIR_EL1` | `MAIR_EL1_SET` | 8 memory attribute types defined |
| `TCR_EL1` | Configured | Page granule, VA bits, cacheability |
| `VBAR_EL1` | `vectors` | Exception vector table |
| `SP` | `init_task` stack | Kernel stack pointer |
