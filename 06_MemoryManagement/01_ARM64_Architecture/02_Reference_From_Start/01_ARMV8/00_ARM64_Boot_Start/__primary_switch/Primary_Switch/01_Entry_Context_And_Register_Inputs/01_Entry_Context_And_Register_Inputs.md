# Entry Context and Register Inputs to `__primary_switch`

**Source:** `arch/arm64/kernel/head.S` | `arch/arm64/mm/proc.S`

---

## What Happened Up to `__primary_switch`

The bootloader hands control to the kernel at `primary_entry` with the MMU off and `x0` holding the physical address of the FDT blob.
`record_mmu_state` checks whether the bootloader left the MMU on and saves that state in `x19`, while `preserve_boot_args` saves `x0`–`x3` into `boot_args[]` and keeps the FDT pointer in `x21`.
`__pi_create_init_idmap` then builds a minimal identity-map page table so the CPU can safely execute code at physical addresses once the MMU is turned on.
`init_kernel_el` configures `SCTLR_EL1`, drops from EL2 to EL1 if needed, and returns the boot EL in `x20`.
Finally, `__cpu_setup` programs `MAIR_EL1` and `TCR_EL1` and returns the ready-to-use `SCTLR_EL1_MMU_ON` value in `x0` — at which point a plain `b __primary_switch` transfers control here with everything the MMU-enable sequence needs already in registers.

---

## 1. Overview

`__primary_switch` is reached via a **BRANCH** (not a branch-and-link):

```asm
bl  __cpu_setup        // initialise processor
b   __primary_switch   // <--- plain branch, x30/lr is NOT updated here
```

This means `__primary_switch` does **NOT** return to `primary_entry`.  
It is a one-way transition toward the fully virtual kernel execution world.

At the moment `__primary_switch` begins executing:

| State      | Value                                                          |
|------------|----------------------------------------------------------------|
| MMU        | OFF (will be turned ON inside this function)                   |
| D-Cache    | OFF                                                            |
| I-Cache    | ON or OFF (don't care at this point)                           |
| Running in | `.idmap.text` section (identity mapped, PA == VA)              |
| EL         | EL1 (may have dropped from EL2 earlier in `init_kernel_el`)   |

---

## 2. Register State at Entry

What each register carries and who set it:

| Register   | Value at Entry                                                                                                      | Set By                                                    |
|------------|---------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------|
| `x0`       | `INIT_SCTLR_EL1_MMU_ON` — SCTLR_EL1 value to turn MMU ON                                                          | `__cpu_setup` (proc.S)                                    |
| `x19`      | MMU-was-on flag at kernel boot<br>`0` = MMU OFF at entry<br>non-0 = MMU ON                                         | `record_mmu_state` (head.S)                               |
| `x20`      | CPU boot mode<br>`BOOT_CPU_MODE_EL1` (0x0001)<br>`BOOT_CPU_MODE_EL2` (0x0002)                                      | `primary_entry` → `mov x20, x0` after `init_kernel_el`   |
| `x21`      | Physical address of FDT blob (bootloader passes this in x0 at reset)                                               | `preserve_boot_args` (head.S)                             |
| `sp`       | `early_init_stack` (set in `primary_entry`)                                                                         | `primary_entry`                                           |
| `x29` (fp) | `0` / `xzr` — no real frame chain yet                                                                              | `primary_entry`                                           |

> **Note:** `x19`, `x20`, `x21` are callee-saved registers (AArch64 ABI).  
> `__cpu_setup` only uses `x0`, `x1`, `x5`, `x6`, `x9`, `x15`, `x16`, `x17` — so `x19`/`x20`/`x21` survive the `bl __cpu_setup` call intact.

---

## 3. Register Detail Explanations

### 3.1 `x0` — SCTLR_EL1 Value (MMU-ON Configuration)

- Loaded at the very end of `__cpu_setup` in `proc.S`:

```asm
mov_q   x0, INIT_SCTLR_EL1_MMU_ON
ret
```

- This value has the **M bit (bit 0) = 1**, meaning "enable MMU".
- It also has cache enable bits, alignment bits, etc. pre-configured.
- `__primary_switch` passes this `x0` directly to `__enable_mmu`:

```asm
bl  __enable_mmu       // x0 consumed by set_sctlr_el1 inside
```

- `x0` is the critical handoff value: `__cpu_setup` computes it, `__primary_switch` delivers it to `__enable_mmu` to flip the MMU on.

---

### 3.2 `x19` — MMU State Flag (Entered with MMU On or Off?)

- Set by `record_mmu_state` at the very start of `primary_entry`.
- Logic:
  - Read `SCTLR_EL1` (or `SCTLR_EL2` if booted at EL2).
  - Check the **M bit** (MMU enable) AND **C bit** (D-cache enable).
  - If both set → `x19 = SCTLR_ELx_M` (non-zero) → MMU was ON
  - If either 0 → `x19 = 0` → MMU was OFF
- Used inside `primary_entry` for cache maintenance decisions.
- Preserved across all subsequent calls (callee-saved).
- By the time `__primary_switch` is entered, `x19` is no longer used directly inside `__primary_switch` itself, but it survives to `__primary_switched` and eventually `start_kernel()` scope.

---

### 3.3 `x20` — CPU Boot Mode

- Set by `primary_entry` after `init_kernel_el` returns:

```asm
bl  init_kernel_el     // returns BOOT_CPU_MODE_EL1 or EL2 in w0
mov x20, x0            // save it in callee-saved x20
```

- Possible values:

```
BOOT_CPU_MODE_EL1 = 0x00000001
BOOT_CPU_MODE_EL2 = 0x00000002  (upper 32 bits may hold flags)
```

- Used much later in `__primary_switched`:

```asm
mov x0, x20
bl  set_cpu_boot_mode_flag    // writes to __boot_cpu_mode variable
bl  finalise_el2              // use EL2/VHE if possible
```

---

### 3.4 `x21` — FDT Physical Address

- Set by `preserve_boot_args` at the start of `primary_entry`:

```asm
mov x21, x0      // x0 = FDT passed by bootloader at reset
```

- The FDT (Flattened Device Tree) blob contains board/platform info.
- Passed into `__pi_early_map_kernel` as the second argument:

```asm
mov x1, x21
bl  __pi_early_map_kernel
```

- Also saved to `__fdt_pointer` later in `__primary_switched`:

```asm
str_l x21, __fdt_pointer, x5
```

---

## 4. Code Section Context

`__primary_switch` lives in the `.idmap.text` section:

```asm
.section ".idmap.text","a"
SYM_FUNC_START_LOCAL(__primary_switch)
```

This section is **identity-mapped** (PA == VA) and is part of the minimal mapping that the CPU can execute BEFORE and IMMEDIATELY AFTER the MMU is switched on. This is required because:

- **Before MMU on:** CPU uses physical addresses directly.
- **After MMU on:** The identity map ensures the next instruction fetch after `SCTLR_EL1.M=1` still resolves to the same physical address, avoiding a PC fault.

The function must complete its critical MMU-enable sequence while executing from this identity-mapped region. Only after branching to `__primary_switched` (via `ldr x8` / `br x8`) does execution leave the identity-mapped window and enter the final kernel virtual address space.

---

## 5. Call Chain Summary

How registers flow into `__primary_switch`:

```
Bootloader
  |
  | x0 = FDT physical address
  v
primary_entry
  |
  |---> record_mmu_state       =>  x19 = MMU state flag
  |---> preserve_boot_args     =>  x21 = FDT phys addr
  |---> __pi_create_init_idmap
  |---> [cache maintenance]
  |---> init_kernel_el         =>  x20 = boot mode (EL1 or EL2)
  |---> __cpu_setup            =>  x0  = SCTLR_EL1_MMU_ON value
  |
  b   __primary_switch         <===  ENTRY POINT
        x0  = SCTLR_EL1_MMU_ON
        x19 = MMU-was-on flag
        x20 = boot mode
        x21 = FDT phys addr
        sp  = early_init_stack
        x29 = 0
```
Entry to __primary_switch
  MMU=OFF, PA=VA (idmap), TTBR0=idmap, TTBR1=none
          |
    adrp  x1, reserved_pg_dir
    adrp  x2, __pi_init_idmap_pg_dir
          |
    bl __enable_mmu
          |
  MMU=ON, PA=VA still (idmap running), TTBR0=idmap, TTBR1=reserved(empty)
          |
    sp = early_init_stack
    bl __pi_early_map_kernel   ← builds swapper_pg_dir, updates TTBR1
          |
  MMU=ON, TTBR0=idmap, TTBR1=swapper_pg_dir (real kernel maps live)
          |
    ldr x8, =__primary_switched   ← load high virtual address
    br  x8
          |
  Execution jumps to 0xFFFF_8000_xxxx → __primary_switched
  Now fully in kernel virtual address space → eventually start_kernel()

  
---

*End of Topic: Entry Context and Register Inputs*

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