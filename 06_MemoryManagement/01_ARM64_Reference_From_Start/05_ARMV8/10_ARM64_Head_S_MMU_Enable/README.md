# ARM64 MMU Enable — `head.S` Boot Flow

## What This Section Covers

This section documents how the **MMU (Memory Management Unit) is enabled in ARM64 Linux**
before `start_kernel()` is ever called.

MMU enable happens entirely in **assembly** (`arch/arm64/kernel/head.S`), before any C code runs.

---

## Why This Matters

Until the MMU is on:
- The CPU fetches and executes instructions at **physical addresses**
- There is no virtual memory, no kernel address space
- All pointers are physical

Once the MMU is on:
- The CPU works with **virtual addresses**
- Kernel lives at `0xFFFF000000000000` and above
- User space will live at `0x0000...` (lower half)

---

## File Map

| File | Topic |
|---|---|
| `01_MMU_Enable_Overview.md` | High-level flow and context |
| `02_Page_Table_Setup.md` | `create_idmap` and `__create_page_tables` |
| `03_System_Registers.md` | `SCTLR_EL1`, `TCR_EL1`, `MAIR_EL1`, `TTBR0/1` |
| `04_Enable_MMU_Assembly.md` | The actual `__enable_mmu` assembly code |
| `05_VA_Split_And_Translation_Regime.md` | VA split, `TCR_EL1.T0SZ/T1SZ`, ASID |
| `06_Final_Page_Tables_Paging_Init.md` | `paging_init()` and `swapper_pg_dir` |
| `07_ARM32_vs_ARM64_MMU.md` | Side-by-side comparison |

---

## Position in Boot Timeline

```
                 ┌──────────────────────────────────────┐
                 │  EL2 / EL3 Firmware (MMU off)         │
                 └──────────────┬───────────────────────┘
                                │
                 ┌──────────────▼───────────────────────┐
                 │  head.S                               │
                 │  ├── create_idmap()                   │
                 │  ├── __create_page_tables()           │
                 │  ├── Set TTBR0/TTBR1/TCR/MAIR        │
                 │  ├── SCTLR_EL1.M = 1  ◄── MMU ON     │
                 │  ├── isb                              │
                 │  └── br __primary_switched            │
                 └──────────────┬───────────────────────┘
                                │
                 ┌──────────────▼───────────────────────┐
                 │  __primary_switched (virtual address) │
                 │  └── start_kernel()                   │
                 │        └── setup_arch()               │
                 │              └── paging_init()        │
                 │                    └── swapper_pg_dir │
                 └──────────────────────────────────────┘
```

---

## Key Source Files

| Kernel File | Purpose |
|---|---|
| `arch/arm64/kernel/head.S` | Page table creation + `__enable_mmu` |
| `arch/arm64/mm/mmu.c` | `paging_init()`, `map_kernel()` |
| `arch/arm64/include/asm/sysreg.h` | `SCTLR_EL1` / `TCR_EL1` bit definitions |
| `arch/arm64/mm/proc.S` | TLB flush, `cpu_do_switch_mm` |
| `arch/arm64/include/asm/pgtable.h` | Page table macros and constants |
