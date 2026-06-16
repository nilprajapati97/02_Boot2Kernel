<div align="center">

# ⚙️ ARM64 Boot Flow

**Deep-dive documentation of the Linux ARM64 kernel boot path —**<br>
**from firmware handoff through MMU enable to `start_kernel()`**

</div>

[![Platform](https://img.shields.io/badge/Platform-ARM64%20%2F%20AArch64-64FFDA?style=flat-square&logo=arm&logoColor=white)]()
[![Kernel](https://img.shields.io/badge/Linux%20Kernel-6.x-orange?style=flat-square&logo=linux&logoColor=white)]()
[![Language](https://img.shields.io/badge/Language-C%20%7C%20ARM64%20Assembly-blue?style=flat-square)]()
[![Sections](https://img.shields.io/badge/Sections-4-6A5ACD?style=flat-square)]()

---

## 📖 What This Repository Covers

This repository is a structured, deeply annotated reference for engineers studying how the **Linux kernel boots on ARM64 (AArch64)**. It traces every significant step — from the first instruction executed after firmware hands off control, through page table creation and MMU enable, all the way to `start_kernel()` and every subsystem it initializes.

Each section follows a consistent format:
- **Purpose** — what problem the code solves
- **System state** at entry and exit
- **Assembly / C source references** with line-level explanations
- **Register-level and memory-level** walk-throughs
- **ASCII diagrams** for call flows and state transitions
- **Interview-ready** summaries for every key concept

---

## 🗺️ Repository Layout

```
06_ARM64_Boot_Flow/
│
├── 00_ARM64_Boot_Start/          # head.S entry → __primary_switched
├── 01_Kernel_ARM64/              # Assembly ↔ C bridge + start_kernel() phases
├── 10_ARM64_Head_S_MMU_Enable/   # Page table creation + __enable_mmu
└── 11_ARMV8/                     # Complete ARMv8 architecture reference
```

---

## 🚀 Boot Flow at a Glance

```
                 ┌───────────────────────────────────────────┐
                 │  EL3 / EL2  Firmware (TF-A / U-Boot)      │
                 │  MMU OFF · Physical addresses              │
                 └──────────────────┬────────────────────────┘
                                    │ handoff to kernel image
                 ┌──────────────────▼────────────────────────┐
                 │  head.S  ·  _head / primary_entry          │  ← 00_ARM64_Boot_Start
                 │  ├── el2_setup() / init_kernel_el()        │
                 │  ├── create_idmap()                        │
                 │  ├── __create_page_tables()                │  ← 10_ARM64_Head_S_MMU_Enable
                 │  ├── __cpu_setup()  (TCR, MAIR)            │
                 │  ├── __enable_mmu() → SCTLR_EL1.M = 1     │  ← MMU ON ✅
                 │  └── br __primary_switch                   │
                 └──────────────────┬────────────────────────┘
                                    │
                 ┌──────────────────▼────────────────────────┐
                 │  __primary_switched (virtual address)      │  ← 00_ARM64_Boot_Start
                 │  ├── set_cpu_boot_mode_flag()              │
                 │  ├── kasan_early_init()                    │
                 │  ├── finalise_el2_vhe()                    │
                 │  └── start_kernel()                        │
                 └──────────────────┬────────────────────────┘
                                    │
                 ┌──────────────────▼────────────────────────┐
                 │  start_kernel()  (init/main.c)             │  ← 01_Kernel_ARM64
                 │  ├── setup_arch() → paging_init()          │
                 │  ├── mm_core_init()  (buddy allocator)     │
                 │  ├── sched_init()                          │
                 │  ├── init_IRQ() / timekeeping_init()       │
                 │  ├── console_init()                        │
                 │  └── arch_call_rest_init() → idle loop     │
                 └───────────────────────────────────────────┘
```

---

## 📂 Section Reference

### [`00_ARM64_Boot_Start`](00_ARM64_Boot_Start/) — head.S Entry & Early Setup

Everything that happens **before** and **immediately after** the MMU is enabled.

| Directory | Topic |
|:---|:---|
| [`_head/`](00_ARM64_Boot_Start/_head/) | `_head` / `primary_entry` — the very first instruction |
| [`__primary_switch/`](00_ARM64_Boot_Start/__primary_switch/) | `__primary_switch`, `cpu_setup` |
| [`00_primary_switched/`](00_ARM64_Boot_Start/00_primary_switched/) | `__primary_switched` — post-MMU setup (KASAN, EL2-VHE, boot flags) |
| [`Memory_Management/`](00_ARM64_Boot_Start/Memory_Management/) | ARMv8 memory initialization in head.S |
| [`Code_Walkthrough/`](00_ARM64_Boot_Start/Code_Walkthrough/) | `__primary_switched` + `start_kernel` walkthrough |

**Key concepts:** exception level setup, identity mapping, boot mode recording (`__boot_cpu_mode`), KASAN early init, VHE finalization.

---

### [`01_Kernel_ARM64`](01_Kernel_ARM64/) — Kernel Code Walk-Through

The Assembly-to-C bridge and the full `start_kernel()` initialization sequence.

| Directory | Topic |
|:---|:---|
| [`04_Assembly/`](01_Kernel_ARM64/04_Assembly/) | `_head`, `__primary_switch`, `__primary_switched`, `start_kernel` — assembly view |
| [`05_START_KERNEL/`](01_Kernel_ARM64/05_START_KERNEL/) | Phase-by-phase `start_kernel()` call sequence |
| [`06_start_kernel/`](01_Kernel_ARM64/06_start_kernel/) | Deep-dive: every `start_kernel()` phase with state diagrams |
| [`Memory_CodeWalkthrough.md`](01_Kernel_ARM64/Memory_CodeWalkthrough.md) | Memory subsystem bring-up walk-through |

**`start_kernel()` phase map:**

```
Phase 1  — Early CPU & task setup     (set_task_stack_end_magic, boot_cpu_init)
Phase 2  — Architecture setup         (setup_arch → paging_init)
Phase 3  — Parameter parsing          (parse_early_param, cmdline)
Phase 4  — Core memory & exceptions   (mm_core_init: memblock → buddy allocator)
Phase 5  — Tracing                    (ftrace, early_trace_init)
Phase 6  — Scheduler                  (sched_init, CFS)
Phase 7  — Data structures            (radix tree, maple tree, workqueues)
Phase 8  — RCU                        (rcu_init)
Phase 9  — Interrupts & timers        (init_IRQ, hrtimers, softirq, timekeeping)
Phase 10 — Randomness & safety        (CRNG, KFENCE, stack canary)
Phase 11 — Perf & profiling           (perf_event, PMU)
          ── local_irq_enable() ──
Phase 12 — Console & locking          (console_init, lockdep)
Phase 13 — NUMA, ACPI, clocks         (numa_policy_init, calibrate_delay)
Phase 14 — Process & security         (fork_init, cred_init, security_init)
Phase 15 — Filesystems & namespaces   (vfs_caches_init, proc_root_init)
Phase 16 — Control groups             (cgroup_init, cpuset_init)
Phase 17 — Platform finalization      (acpi_subsystem_init)
          ── arch_call_rest_init() → idle loop ──
```

---

### [`10_ARM64_Head_S_MMU_Enable`](10_ARM64_Head_S_MMU_Enable/) — MMU Enable Deep-Dive

The most critical transition in ARM64 boot: turning the MMU on.

| File | Topic |
|:---|:---|
| [`01_MMU_Enable_Overview.md`](10_ARM64_Head_S_MMU_Enable/01_MMU_Enable_Overview.md) | High-level flow, identity mapping problem, two-stage MMU |
| [`02_Page_Table_Setup.md`](10_ARM64_Head_S_MMU_Enable/02_Page_Table_Setup.md) | `create_idmap` and `__create_page_tables` |
| [`03_System_Registers.md`](10_ARM64_Head_S_MMU_Enable/03_System_Registers.md) | `SCTLR_EL1`, `TCR_EL1`, `MAIR_EL1`, `TTBR0/1_EL1` |
| [`04_Enable_MMU_Assembly.md`](10_ARM64_Head_S_MMU_Enable/04_Enable_MMU_Assembly.md) | `__enable_mmu` — the assembly code that flips M-bit |
| [`05_VA_Split_And_Translation_Regime.md`](10_ARM64_Head_S_MMU_Enable/05_VA_Split_And_Translation_Regime.md) | VA split, `TCR_EL1.T0SZ/T1SZ`, ASID |
| [`06_Final_Page_Tables_Paging_Init.md`](10_ARM64_Head_S_MMU_Enable/06_Final_Page_Tables_Paging_Init.md) | `paging_init()` and `swapper_pg_dir` |
| [`07_ARM32_vs_ARM64_MMU.md`](10_ARM64_Head_S_MMU_Enable/07_ARM32_vs_ARM64_MMU.md) | Side-by-side ARM32 vs ARM64 MMU comparison |

**Key concepts:** identity map, two-stage MMU enable (`init_pg_dir` → `swapper_pg_dir`), why D-cache and I-cache are enabled simultaneously with the MMU.

---

### [`11_ARMV8`](11_ARMV8/) — ARMv8 Architecture Reference

A complete ARMv8/AArch64 reference covering every major subsystem.

| Subsystem | Directory | Description |
|:---:|:---|:---|
| 🏛️ | [`docs/00_ARMv8_Overview/`](11_ARMV8/docs/00_ARMv8_Overview/) | AArch64, exception levels, ISA overview |
| 🖥️ | [`docs/01_CPU_Subsystem/`](11_ARMV8/docs/01_CPU_Subsystem/) | Registers, instruction set, execution model |
| 💾 | [`docs/02_Memory_Subsystem/`](11_ARMV8/docs/02_Memory_Subsystem/) | MMU, page tables, TLB |
| ⚡ | [`docs/03_Cache_Subsystem/`](11_ARMV8/docs/03_Cache_Subsystem/) | Cache hierarchy, coherency, maintenance |
| 🔔 | [`docs/04_Interrupt_Subsystem/`](11_ARMV8/docs/04_Interrupt_Subsystem/) | GIC, IRQ handling, interrupt routing |
| 🛡️ | [`docs/05_Security_Subsystem/`](11_ARMV8/docs/05_Security_Subsystem/) | TrustZone, secure world, EL3 |
| ☁️ | [`docs/06_Virtualization_Subsystem/`](11_ARMV8/docs/06_Virtualization_Subsystem/) | Hypervisor, EL2, stage-2 translation |
| 🐛 | [`docs/07_Debug_Trace_Subsystem/`](11_ARMV8/docs/07_Debug_Trace_Subsystem/) | CoreSight, breakpoints, debug registers |
| 🔀 | [`docs/08_Interconnect_Subsystem/`](11_ARMV8/docs/08_Interconnect_Subsystem/) | AMBA, AXI, ACE, CHI protocols |
| 🔋 | [`docs/09_Power_Management/`](11_ARMV8/docs/09_Power_Management/) | PSCI, CPU hotplug, DVFS |
| 📐 | [`docs/10_SIMD_FloatingPoint/`](11_ARMV8/docs/10_SIMD_FloatingPoint/) | NEON SIMD, FP extensions |
| ❓ | [`Questions/`](11_ARMV8/Questions/) | Architecture practice Q&A |

**Additional topics in [`Questions/`](11_ARMV8/Questions/):**
Cache coherence · TLB miss handling · MMU enable in Linux · Cache invalidation before MMU · MESI protocol · Memory barriers · SMP initialization · Spin Table mechanism · Cache line impact · NUMA

---

## 🧠 Key Concepts Covered

| Concept | Where |
|:---|:---|
| Exception level setup (EL1/EL2/EL3) | `00_ARM64_Boot_Start`, `11_ARMV8` |
| Identity mapping and the MMU-enable window | `10_ARM64_Head_S_MMU_Enable` |
| `TTBR0_EL1` / `TTBR1_EL1` and VA split | `10_ARM64_Head_S_MMU_Enable/05` |
| `__boot_cpu_mode` and CPU boot mode recording | `00_ARM64_Boot_Start/00_primary_switched` |
| KASAN early initialization | `00_ARM64_Boot_Start/00_primary_switched` |
| VHE (Virtualization Host Extensions) | `00_ARM64_Boot_Start/00_primary_switched` |
| `memblock` → buddy allocator transition | `01_Kernel_ARM64/06_start_kernel` |
| `setup_arch()` and `paging_init()` | `01_Kernel_ARM64/06_start_kernel` |
| `swapper_pg_dir` and permanent page tables | `10_ARM64_Head_S_MMU_Enable/06` |
| SMP bring-up and spin table | `11_ARMV8/Questions` |
| ARM64 calling convention (AAPCS64) | `00_ARM64_Boot_Start/00_primary_switched` |

---

## 🔑 Key Kernel Source Files Referenced

| Kernel File | Role |
|:---|:---|
| `arch/arm64/kernel/head.S` | Boot entry, page table creation, `__enable_mmu` |
| `arch/arm64/mm/mmu.c` | `paging_init()`, `map_kernel()`, `__boot_cpu_mode` |
| `arch/arm64/mm/proc.S` | `__cpu_setup()` — configures TCR, MAIR |
| `arch/arm64/include/asm/sysreg.h` | System register bit definitions |
| `arch/arm64/include/asm/pgtable.h` | Page table macros and constants |
| `arch/arm64/include/asm/virt.h` | `BOOT_CPU_MODE_EL1/EL2`, VHE helpers |
| `init/main.c` | `start_kernel()` — first arch-independent C function |
| `arch/arm64/kernel/setup.c` | `setup_arch()` |
| `arch/arm64/mm/init.c` | `paging_init()`, memory layout |

---

## 📚 How to Read This Documentation

1. **Start with the boot flow diagram** above to get the big picture.
2. **Follow `00_ARM64_Boot_Start`** to understand the very first instructions.
3. **Read `10_ARM64_Head_S_MMU_Enable`** for the critical MMU-on transition.
4. **Study `01_Kernel_ARM64`** for the Assembly → C handoff and `start_kernel()` phases.
5. **Use `11_ARMV8`** as a reference for architecture fundamentals and interview prep.

Within each section, documents are numbered for reading order and include cross-references to the Linux kernel source tree.

---

<div align="center">

<sub>ARM64 Boot Flow Documentation &nbsp;·&nbsp; Linux Kernel Internals &nbsp;·&nbsp; AArch64 Architecture</sub>

</div>
