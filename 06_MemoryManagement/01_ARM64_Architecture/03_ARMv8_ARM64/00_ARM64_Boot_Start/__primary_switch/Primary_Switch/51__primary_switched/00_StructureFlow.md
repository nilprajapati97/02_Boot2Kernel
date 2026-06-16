

# Deep Dive: `__primary_switched` — Memory Perspective (ARMv8)

---

## Directory Structure

```
51__primary_switched/
├── 00_overview/                         ← full function summary + execution order
├── 01_system_state_at_entry/            ← MMU, registers, PC, cache state on entry
├── 02_init_cpu_task/
│   ├── 00_overview/                     ← macro summary
│   ├── 01_sp_el0_current_task/          ← how sp_el0 becomes "current"
│   ├── 02_kernel_stack_setup/           ← 16KB init_stack, PT_REGS reservation, layout diagram
│   ├── 03_final_frame_marker/           ← fp=0/lr=0 sentinel, unwinder chain
│   ├── 04_shadow_call_stack/            ← x18, SCS, ROP protection
│   └── 05_per_cpu_offset_tpidr_el1/     ← tpidr_el1, per-cpu base, array indexing
├── 03_exception_vector_setup/           ← VBAR_EL1, vector table layout, ISB reason
├── 04_stack_frame_c_calling_convention/ ← stp/mov x29, AAPCS64 prologue
├── 05_fdt_pointer_save/                 ← x21, __fdt_pointer, why physical address
├── 06_kimage_voffset/
│   ├── 00_overview/
│   ├── 01_virtual_minus_physical/       ← adrp vs x0 origin, formula, usage macros
│   └── 02_kaslr_impact/                 ← randomization, security benefit
├── 07_boot_mode_flag/                   ← __boot_cpu_mode, EL1/EL2 slots, KVM use
├── 08_kasan_early_init/                 ← shadow memory, why before C code, overhead
├── 09_finalise_el2_vhe/                 ← VHE, HCR_EL2.E2H, EL negotiation
└── 10_start_kernel_handoff/             ← ldp restore, bl start_kernel, ASM_BUG, final state table
```

---

## Execution Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                  Entry: __primary_switched                      │
│          (MMU ON · Virtual PC · EL1 · Caches clean)             │
└─────────────────────────┬───────────────────────────────────────┘
                          │
                          ▼
          ┌───────────────────────────────┐
          │  [01] System State at Entry   │
          │  MMU on · virtual PC          │
          │  registers · cache state      │
          └───────────────┬───────────────┘
                          │
                          ▼
          ┌───────────────────────────────────────────────────────┐
          │  [02] init_cpu_task  (macro)                          │
          │                                                       │
          │  ├─ [02.01] sp_el0  ←  current task pointer          │
          │  ├─ [02.02] Kernel stack setup                        │
          │  │          16KB init_stack · PT_REGS reservation     │
          │  ├─ [02.03] Final frame marker                        │
          │  │          fp=0, lr=0 sentinel → unwinder chain      │
          │  ├─ [02.04] Shadow Call Stack                         │
          │  │          x18 · SCS · ROP protection                │
          │  └─ [02.05] tpidr_el1  ←  per-cpu base offset        │
          └───────────────┬───────────────────────────────────────┘
                          │
                          ▼
          ┌───────────────────────────────┐
          │  [03] Exception Vector Setup  │
          │  VBAR_EL1 ← vector table base │
          │  ISB (instruction sync)       │
          └───────────────┬───────────────┘
                          │
                          ▼
          ┌───────────────────────────────┐
          │  [04] Stack Frame Setup       │
          │  stp fp, lr  /  mov x29, sp   │
          │  AAPCS64 prologue             │
          └───────────────┬───────────────┘
                          │
                          ▼
          ┌───────────────────────────────┐
          │  [05] FDT Pointer Save        │
          │  x21 → __fdt_pointer          │
          │  (physical addr, pre-remap)   │
          └───────────────┬───────────────┘
                          │
                          ▼
          ┌───────────────────────────────────────────────────────┐
          │  [06] kimage_voffset                                  │
          │                                                       │
          │  ├─ [06.01] Overview                                  │
          │  ├─ [06.02] Virtual − Physical formula                │
          │  │          adrp vs x0 origin · usage macros          │
          │  └─ [06.03] KASLR impact                              │
          │             randomization · security benefit          │
          └───────────────┬───────────────────────────────────────┘
                          │
                          ▼
          ┌───────────────────────────────┐
          │  [07] Boot Mode Flag          │
          │  __boot_cpu_mode              │
          │  EL1/EL2 slots · KVM use      │
          └───────────────┬───────────────┘
                          │
                          ▼
          ┌───────────────────────────────┐
          │  [08] KASAN Early Init        │
          │  Shadow memory map            │
          │  runs before any C code       │
          │  performance overhead noted   │
          └───────────────┬───────────────┘
                          │
                          ▼
          ┌───────────────────────────────┐
          │  [09] Finalise EL2 / VHE      │
          │  HCR_EL2.E2H · EL negotiation │
          │  VHE host/guest unification   │
          └───────────────┬───────────────┘
                          │
                          ▼
          ┌───────────────────────────────┐
          │  [10] start_kernel Handoff    │
          │  ldp restore · bl start_kernel│
          │  ASM_BUG trap (no return)     │
          │  final CPU state table        │
          └───────────────────────────────┘
```

---

## Step Reference Table

| Step | Directory                          | Key Topic                                    |
|------|------------------------------------|----------------------------------------------|
| 01   | `01_system_state_at_entry/`        | MMU, registers, PC, cache state on entry     |
| 02   | `02_init_cpu_task/`                | CPU task init macro — 5 sub-topics           |
| 02.1 | `└─ 01_sp_el0_current_task/`       | sp_el0 → current task pointer                |
| 02.2 | `└─ 02_kernel_stack_setup/`        | 16KB init_stack, PT_REGS, layout diagram     |
| 02.3 | `└─ 03_final_frame_marker/`        | fp=0 / lr=0 sentinel, stack unwinder         |
| 02.4 | `└─ 04_shadow_call_stack/`         | x18, SCS, ROP protection                     |
| 02.5 | `└─ 05_per_cpu_offset_tpidr_el1/`  | tpidr_el1, per-cpu base, array indexing      |
| 03   | `03_exception_vector_setup/`       | VBAR_EL1, vector table layout, ISB           |
| 04   | `04_stack_frame_c_calling_convention/` | stp/mov x29, AAPCS64 prologue            |
| 05   | `05_fdt_pointer_save/`             | x21, __fdt_pointer, physical address         |
| 06   | `06_kimage_voffset/`               | VA−PA offset — 3 sub-topics                  |
| 06.1 | `└─ 01_virtual_minus_physical/`    | adrp vs x0 origin, formula, usage macros     |
| 06.2 | `└─ 02_kaslr_impact/`              | KASLR randomization, security benefit        |
| 07   | `07_boot_mode_flag/`               | __boot_cpu_mode, EL1/EL2 slots, KVM          |
| 08   | `08_kasan_early_init/`             | Shadow memory, early init, overhead          |
| 09   | `09_finalise_el2_vhe/`             | VHE, HCR_EL2.E2H, EL negotiation            |
| 10   | `10_start_kernel_handoff/`         | ldp restore, bl start_kernel, ASM_BUG        |

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
This document describes a stage in the ARMv8-A Linux ARM64 boot path. ARMv8-A is the 64-bit ARM architecture (AArch64 execution state) introduced with the ARM Cortex-A53/A57 generation. Key architectural features relevant to boot:
- Exception levels: EL0 (user), EL1 (OS kernel), EL2 (hypervisor), EL3 (secure monitor).
- Two-stage translation: TTBR0_EL1 (user/low VA) and TTBR1_EL1 (kernel/high VA).
- System registers accessed via MRS/MSR instructions (not memory-mapped).
- PSTATE: condition flags + CPU mode + interrupt mask bits.
- Mandatory ISB after system register writes that affect instruction fetch.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 boot path follows this sequence:
  stext (head.S) -> __primary_switch -> __pi_early_map_kernel -> __enable_mmu
  -> __primary_switched -> start_kernel -> setup_arch -> paging_init
Each stage initializes one more layer of the memory system. Before start_kernel, all memory management is done with physical addresses or the early identity/kernel maps. After paging_init(), the full kernel virtual memory map is active.

### Memory Perspective (ARMv8 Memory Model)
The ARMv8 memory model (based on the ARM ARM's "Arm Memory Model" chapter) defines:
- Normal memory: cacheable, reorderable, speculatable. Used for DRAM (kernel code, data, stack, heap).
- Device memory: non-cacheable, strictly ordered. Used for MMIO (UART, GIC, etc.).
- Barriers: DSB (Data Synchronization Barrier), DMB (Data Memory Barrier), ISB (Instruction Synchronization Barrier) enforce ordering guarantees.
At boot, the kernel transitions from a world where every address is physical (pre-MMU) to the full ARMv8 virtual memory model where TTBR0 and TTBR1 map the user and kernel address spaces respectively.