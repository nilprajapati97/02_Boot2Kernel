# ARM64 MMU Enable — High-Level Overview

## 1. The Problem MMU Enable Solves

At power-on, the ARM64 CPU runs at **physical addresses** with no memory protection,
no virtual address translation, and no kernel address space layout.

Linux must transition from this raw physical state to a fully virtual memory model
before C code can safely execute. This transition is **MMU enable**.

---

## 2. What "MMU Enable" Means

"Enabling the MMU" means setting **bit 0 (M-bit)** of the `SCTLR_EL1` register.

After this bit is set:
- Every memory access the CPU makes goes through the **Translation Lookaside Buffer (TLB)** and **page tables**
- Physical Address → Virtual Address mapping is active
- Memory attributes (cacheable, shareable, device) are enforced by `MAIR_EL1`
- Access permission checks are enforced by page table descriptor bits

---

## 3. The Critical Constraint — Identity Map

**At the exact cycle the M-bit is set, the next instruction must be reachable
through the new page tables.**

If the page tables do not map the current Physical Address (PC), the CPU
immediately takes a **Translation Fault** and the system halts.

The solution is an **identity map**:

```
Physical Address 0x40200000  ==mapped to==>  Virtual Address 0x40200000
```

This means `PA == VA` for the small region of code that enables the MMU.
Once the MMU is on and the code jumps to the high kernel VA (`0xFFFF...`),
the identity map is no longer needed.

---

## 4. High-Level Steps

```
Step 1: Build identity page table (idmap_pg_dir)
        PA == VA for the __enable_mmu code region

Step 2: Build initial kernel page table (init_pg_dir)
        Kernel image mapped at 0xFFFF000000000000

Step 3: Configure system registers
        TTBR0_EL1  ← idmap_pg_dir (physical address)
        TTBR1_EL1  ← init_pg_dir  (physical address)
        TCR_EL1    ← VA size, granule, shareability config
        MAIR_EL1   ← memory attribute index table
        isb        ← instruction sync barrier

Step 4: Enable MMU
        SCTLR_EL1 |= (M | C | I)   // MMU + D-cache + I-cache
        isb                         // mandatory pipeline flush

Step 5: Branch to virtual address
        br  __primary_switched      // jump to 0xFFFF...

Step 6: Eventually replace temporary tables
        paging_init() in setup_arch() installs swapper_pg_dir
```

---

## 5. Exception Level at Boot

ARM64 Linux boots at **EL2** (if a hypervisor is present) or **EL1**.

- `EL1` uses `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`
- If booting at EL2, `head.S` drops to EL1 first via `el2_setup()`

The MMU enable described here is for **EL1** — the kernel's exception level.

---

## 6. Two-Stage MMU Enable

ARM64 actually has two rounds of page table activity:

| Stage | Where | What happens |
|---|---|---|
| **Stage 1 (early)** | `head.S` | Temporary `init_pg_dir` + identity map, MMU turned on |
| **Stage 2 (final)** | `paging_init()` in `setup_arch()` | Permanent `swapper_pg_dir` installed, full kernel VA mapped |

After stage 2, `init_pg_dir` is freed and `swapper_pg_dir` is the kernel's
permanent page table for its entire lifetime.

---

## 7. Boot Flow Diagram

```
Power ON
   │
   ▼
EL3 / EL2 Firmware (TF-A / U-Boot)
   │   MMU OFF, physical addresses
   ▼
head.S: _start / primary_entry
   │
   ├──► el2_setup()           — drop to EL1 if needed
   │
   ├──► create_idmap()        — build idmap_pg_dir
   │
   ├──► __create_page_tables() — build init_pg_dir
   │
   ├──► __cpu_setup()         — set TCR_EL1, MAIR_EL1
   │
   ├──► __enable_mmu()        — SCTLR_EL1.M = 1  ◄── MMU ON
   │        └── isb
   │
   ├──► br __primary_switched — jump to 0xFFFF...
   │
   ▼
__primary_switched()           — now at virtual address
   │
   └──► start_kernel()
              └──► setup_arch()
                       └──► paging_init()  — install swapper_pg_dir
```

---

## 8. Why Cache Is Enabled Together with MMU

Notice the `__enable_mmu` code also sets:
- `SCTLR_ELx_C` — Data cache enable
- `SCTLR_ELx_I` — Instruction cache enable

These are typically enabled **at the same time** as the MMU because:
1. Cache coherency requires knowing memory attributes (Normal vs Device)
2. Memory attributes come from page table descriptors
3. Without the MMU, the CPU cannot determine the correct memory attributes

Enabling them separately can cause **cache coherency issues** with other CPUs.

---

## 9. Key Files Summary

| File | Location | Role |
|---|---|---|
| `head.S` | `arch/arm64/kernel/` | Assembly boot entry, identity map, MMU enable |
| `mmu.c` | `arch/arm64/mm/` | `paging_init()`, `map_kernel()`, `create_mapping()` |
| `proc.S` | `arch/arm64/mm/` | `__cpu_setup()` — configures TCR, MAIR |
| `sysreg.h` | `arch/arm64/include/asm/` | All system register bit definitions |
| `pgtable.h` | `arch/arm64/include/asm/` | Page table macros, `pgd_offset`, etc. |
| `memory.h` | `arch/arm64/include/asm/` | `PAGE_OFFSET`, `KIMAGE_VADDR`, VA layout |
