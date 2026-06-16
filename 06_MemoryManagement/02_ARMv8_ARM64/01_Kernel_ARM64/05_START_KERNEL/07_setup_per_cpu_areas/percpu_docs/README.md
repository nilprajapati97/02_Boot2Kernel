# setup_per_cpu_areas() — Complete Interview Reference
## Linux Kernel Per-CPU Subsystem: ARM32 & ARM64 Deep Dive

---

## Purpose

This documentation tree is a **complete, interview-ready reference** for the Linux kernel
`setup_per_cpu_areas()` call chain. Every function, every data structure, every hardware
register, and every architectural difference between ARM32 and ARM64 is documented here
with source file and line number citations from the Linux kernel.

---

## How to Navigate This Tree

```
percpu_docs/
├── README.md                              ← YOU ARE HERE — master index
│
├── 01_system_overview/
│   └── overview.md                        ← START HERE: foundational concepts
│
├── 02_data_structures/
│   ├── pcpu_alloc_info.md                 ← Core descriptor struct
│   ├── pcpu_chunk.md                      ← Chunk allocator structure
│   ├── pcpu_group_info.md                 ← NUMA group layout
│   └── per_cpu_offset_table.md            ← __per_cpu_offset[NR_CPUS] array
│
├── 03_call_flow/
│   ├── master_call_flow.md                ← FULL call tree with file:line annotations
│   ├── arm32_call_flow.md                 ← ARM32 specific path
│   └── arm64_call_flow.md                 ← ARM64 specific path (VHE included)
│
├── 04_boot_entry__start_kernel/
│   └── start_kernel.md                    ← Position of setup_per_cpu_areas in boot
│
├── 05_setup_per_cpu_areas/
│   ├── generic_mm_percpu.md               ← mm/percpu.c:3383 — the generic function
│   ├── arm32_no_arch_override.md          ← ARM32 uses generic path
│   └── arm64_no_arch_override.md          ← ARM64 uses generic path
│
├── 06_pcpu_embed_first_chunk/
│   └── pcpu_embed_first_chunk.md          ← mm/percpu.c:3075 — primary allocator
│
├── 07_pcpu_build_alloc_info/
│   └── pcpu_build_alloc_info.md           ← mm/percpu.c:2864 — NUMA grouping
│
├── 08_memblock_alloc/
│   └── memblock_alloc_try_nid.md          ← Bootmem allocation
│
├── 09_template_copy/
│   └── memcpy_per_cpu_load.md             ← Per-CPU data initialization
│
├── 10_pcpu_setup_first_chunk/
│   ├── pcpu_setup_first_chunk.md          ← mm/percpu.c:2608 — bookkeeping
│   ├── pcpu_alloc_first_chunk.md          ← mm/percpu.c:1345 — chunk creation
│   └── pcpu_chunk_relocate.md             ← mm/percpu.c:555 — slot management
│
├── 11_offset_table_computation/
│   └── per_cpu_offset_array.md            ← Delta formula + numeric example
│
├── 12_set_my_cpu_offset/
│   ├── arm32_TPIDRPRW.md                  ← mcr p15,0,Rn,c13,c0,4
│   └── arm64_tpidr_el1.md                 ← msr tpidr_el1 + ALTERNATIVE()
│
├── 13_secondary_cpu_bringup/
│   ├── arm32_secondary_start_kernel.md    ← arch/arm/kernel/smp.c:410
│   └── arm64_secondary_start_kernel.md    ← arch/arm64/kernel/smp.c:203
│
├── 14_runtime_access/
│   ├── per_cpu_macros.md                  ← DEFINE_PER_CPU, per_cpu(), SHIFT_PERCPU_PTR
│   ├── this_cpu_read_write.md             ← 3-instruction hot path assembly
│   └── raw_cpu_vs_this_cpu.md             ← Preemption safety distinctions
│
├── 15_hardware_registers/
│   ├── arm32_cp15_tpidrprw.md             ← CP15 c13 register map
│   └── arm64_tpidr_el1_el2.md             ← tpidr_* register family
│
├── 16_memory_layout/
│   └── percpu_memory_layout.md            ← Before/after diagrams
│
└── 17_interview_qa/
    └── interview_questions_and_answers.md ← 20 Q&A for kernel engineer interview
│
└── 18_powerpc64/
    ├── setup_per_cpu_areas_ppc64.md       ← PPC64 arch-specific setup_64.c flow
    ├── paca_and_percpu_access.md           ← PACA (`r13`) and `data_offset` model
    ├── ppc64_interview_qa.md               ← PPC64 interview quick Q&A
    ├── 01_call_flow/
    │   └── master_call_flow_ppc64.md       ← Full PPC64 function call path
    ├── 02_allocator_strategy/
    │   └── atom_size_and_fallbacks.md      ← MMU-based atom size + fallback policy
    ├── 03_paca_offset_wiring/
    │   └── paca_data_offset_wiring.md      ← `__per_cpu_offset[]` and PACA sync
    └── 04_arch_comparison/
        └── ppc64_vs_arm.md                 ← Interview-ready architecture comparison
```

---

## Recommended Reading Order

### For a 30-minute interview prep
1. `03_call_flow/master_call_flow.md` — memorize the tree
2. `12_set_my_cpu_offset/arm32_TPIDRPRW.md` + `arm64_tpidr_el1.md`
3. `17_interview_qa/interview_questions_and_answers.md`

### For deep understanding (2 hours)
1. `01_system_overview/overview.md`
2. `02_data_structures/` — all 4 files
3. `05_setup_per_cpu_areas/generic_mm_percpu.md`
4. `06_pcpu_embed_first_chunk/` through `11_offset_table_computation/`
5. `12_set_my_cpu_offset/` + `13_secondary_cpu_bringup/`
6. `14_runtime_access/` + `16_memory_layout/`
7. `17_interview_qa/`
8. `18_powerpc64/setup_per_cpu_areas_ppc64.md`
9. `18_powerpc64/01_call_flow/master_call_flow_ppc64.md`
10. `18_powerpc64/04_arch_comparison/ppc64_vs_arm.md`

---

## Quick Reference Card

| Topic | ARM32 | ARM64 |
|---|---|---|
| HW register | TPIDRPRW (CP15 c13,c0,4) | tpidr_el1 (or tpidr_el2 VHE) |
| Write instruction | `mcr p15, 0, Rn, c13, c0, 4` | `msr tpidr_el1, Xn` |
| Read instruction | `mrc p15, 0, Rd, c13, c0, 4` | `mrs Xd, tpidr_el1` |
| Write function | `set_my_cpu_offset()` in percpu.h | `set_my_cpu_offset()` in percpu.h |
| Runtime read macro | `__my_cpu_offset` | `__kern_my_cpu_offset()` |
| UP fallback | `.alt.smp.init` patches ldr | Not needed |
| VHE handling | N/A | `ALTERNATIVE()` patches to tpidr_el2 |
| Boot CPU write | `smp_prepare_boot_cpu()` smp.c:500 | `smp_prepare_boot_cpu()` smp.c:456 |
| Secondary write | `secondary_start_kernel()` smp.c:410 | `secondary_start_kernel()` smp.c:203 |
| arch override? | **No** — uses mm/percpu.c:3383 | **No** — uses mm/percpu.c:3383 |

---

## Source File Reference

| Source File | Key Content |
|---|---|
| `init/main.c:901` | `setup_per_cpu_areas()` call site |
| `mm/percpu.c:3383` | Generic SMP `setup_per_cpu_areas()` |
| `mm/percpu.c:3413` | Generic UP `setup_per_cpu_areas()` |
| `mm/percpu.c:2864` | `pcpu_build_alloc_info()` |
| `mm/percpu.c:3075` | `pcpu_embed_first_chunk()` |
| `mm/percpu.c:2608` | `pcpu_setup_first_chunk()` |
| `mm/percpu.c:1345` | `pcpu_alloc_first_chunk()` |
| `mm/percpu.c:555` | `pcpu_chunk_relocate()` |
| `arch/arm/include/asm/percpu.h:17` | `set_my_cpu_offset()` ARM32 |
| `arch/arm/include/asm/percpu.h:27` | `__my_cpu_offset()` ARM32 |
| `arch/arm/kernel/smp.c:410` | `secondary_start_kernel()` ARM32 |
| `arch/arm/kernel/smp.c:500` | `smp_prepare_boot_cpu()` ARM32 |
| `arch/arm64/include/asm/percpu.h:15` | `set_my_cpu_offset()` ARM64 |
| `arch/arm64/include/asm/percpu.h:32` | `__kern_my_cpu_offset()` ARM64 |
| `arch/arm64/kernel/smp.c:203` | `secondary_start_kernel()` ARM64 |
| `arch/arm64/kernel/smp.c:456` | `smp_prepare_boot_cpu()` ARM64 |
| `include/linux/percpu-defs.h:114` | `DEFINE_PER_CPU` macro |
| `include/linux/percpu-defs.h:230` | `SHIFT_PERCPU_PTR` macro |
| `include/asm-generic/percpu.h:11` | `__per_cpu_offset[]` declaration |
