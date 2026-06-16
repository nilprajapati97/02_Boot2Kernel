# Boot Flow Context — Where `__primary_switched` Sits

## Complete ARM64 Linux Boot Flow (From Power-On to Shell)

```
POWER ON
  │
  ▼
[SoC ROM / TrustZone Secure Monitor]
  │  BL1, BL2, BL31 (ATF — ARM Trusted Firmware)
  │  Sets up secure world, PSCI, SMC handlers
  │
  ▼
[Bootloader — U-Boot / EDK2 / Qualcomm XBL]
  │  Loads kernel Image to RAM at configured offset
  │  Loads DTB (Device Tree Blob) to another RAM address
  │  Sets x0 = PA(DTB), x1=x2=x3=0
  │  Jumps to kernel Image start address
  │
  ▼
[_head / primary_entry — .idmap.text]        ← MMU OFF
  │  bl record_mmu_state        (x19 = was MMU on at entry?)
  │  bl preserve_boot_args      (x21 = FDT PA; save x0-x3 to boot_args)
  │  adrp x1, early_init_stack
  │  mov sp, x1                 (temporary boot stack)
  │  bl __pi_create_init_idmap  (build identity page table for current code)
  │  bl init_kernel_el          (x20 = cpu_boot_mode: EL1 or EL2)
  │  bl __cpu_setup             (TCR, MAIR, SCTLR — MMU ready to enable)
  │  b  __primary_switch
  │
  ▼
[__primary_switch — .idmap.text]              ← MMU OFF → ON
  │  adrp x1, reserved_pg_dir
  │  adrp x2, __pi_init_idmap_pg_dir
  │  bl __enable_mmu            ← *** MMU TURNS ON ***
  │                               TTBR0 = identity map
  │                               TTBR1 = kernel page tables
  │  adrp x1, early_init_stack
  │  mov sp, x1                 (reset stack — now virtual address)
  │  mov x0, x20; mov x1, x21
  │  bl __pi_early_map_kernel   ← KASLR: relocate kernel, build final page tables
  │  ldr x8, =__primary_switched  ← absolute VIRTUAL address
  │  adrp x0, KERNEL_START        ← x0 = __pa(KERNEL_START)
  │  br x8                      ← *** CROSS INTO VIRTUAL ADDRESS SPACE ***
  │
  ▼
[__primary_switched — .text]                  ← MMU ON, VIRTUAL SPACE ← YOU ARE HERE
  │  init_cpu_task              (task, stack, frame, SCS, per-CPU)
  │  msr vbar_el1               (exception vectors)
  │  stp / mov                  (C stack frame)
  │  str_l __fdt_pointer        (save FDT address)
  │  kimage_voffset             (VA-PA offset)
  │  set_cpu_boot_mode_flag     (record EL)
  │  kasan_early_init           (KASAN shadow — if enabled)
  │  finalise_el2               (VHE decision)
  │  bl start_kernel            ← *** ENTER C CODE ***
  │
  ▼
[start_kernel — init/main.c]                  ← FIRST C FUNCTION
  │  setup_arch()               ← parse FDT, memory model
  │  mm_init()                  ← buddy allocator, slab
  │  sched_init()               ← scheduler
  │  irq_init()                 ← GIC, interrupt controllers
  │  console_init()             ← serial console
  │  rest_init()
  │    ├── kernel_thread(kernel_init)   ← PID 1
  │    └── cpu_startup_entry()         ← CPU0 idle loop
  │
  ▼
[kernel_init — PID 1]
  │  Mounts rootfs
  │  Executes /sbin/init (systemd / busybox init)
  │
  ▼
[Userspace — Shell, Services, Applications]
```

---

## The Four Critical Boundaries in ARM64 Boot

### Boundary 1: Bootloader → Kernel (power-on reset or jump)
- CPU in undefined state (could be EL3/EL2/EL1)
- ATF handles EL3 → hands off at EL2 (most SoCs) or EL1
- **Action:** `primary_entry` is the first kernel instruction

### Boundary 2: MMU OFF → MMU ON (in `__enable_mmu`)
- PC is in `.idmap.text` (identity mapped)
- `set_sctlr_el1 x0` sets `SCTLR_EL1.M = 1`
- The very NEXT instruction after SCTLR write executes with MMU ON
- **Action:** TTBR0 (identity) + TTBR1 (kernel) both active

### Boundary 3: Physical Space → Virtual Space (the `br x8` jump)
- After `br x8`, PC = 0xFFFFFF80_xxxxxxxx (kernel virtual)
- `.idmap.text` code (low addresses) no longer accessible via PC
- **Action:** `__primary_switched` begins executing in `.text`

### Boundary 4: Assembly → C Runtime (the `bl start_kernel`)
- `__primary_switched` has established ALL C runtime prerequisites
- **Action:** `start_kernel()` first instruction executes

---

## Why `__primary_switched` Exists as a Separate Function

Before the `br x8` boundary, the code runs from physical/identity-mapped addresses.
After, it runs from kernel virtual addresses.

These two regions have different constraints:
- `.idmap.text` code: must be position-independent, accessible at low PA, short
- `.text` code: can be full-featured, uses kernel VA, can call any C-visible symbol

Splitting into `__primary_switch` (low-level, `.idmap.text`) and `__primary_switched`
(setup, `.text`) is an architectural necessity, not a style choice.

---

## Comparison: Primary vs Secondary CPU Boot

| | Primary CPU | Secondary CPU |
|---|---|---|
| Entry function | `primary_entry` | `secondary_holding_pen` / `secondary_entry` |
| Builds page tables | YES (`__pi_create_init_idmap`) | NO (uses what primary built) |
| "Switched" function | `__primary_switched` | `__secondary_switched` |
| Sets `kimage_voffset` | YES | NO (already set by primary) |
| Sets `__fdt_pointer` | YES | NO |
| Calls `start_kernel` | YES | NO (`secondary_start_kernel`) |
| Calls `kasan_early_init` | YES (if enabled) | NO |

`__secondary_switched` is the secondary CPU equivalent of `__primary_switched`.
It runs a subset of the same steps (set_cpu_boot_mode_flag, finalise_el2,
msr vbar_el1, init_cpu_task) because kimage_voffset and KASAN are already initialized.

---

## Timeline of `x21` (FDT Pointer) — A Thread Through Boot

```
preserve_boot_args:    mov x21, x0     ← bootloader put FDT PA in x0
  │
  │ [x21 survives as callee-saved through 10+ functions]
  │
__primary_switched:    str_l x21, __fdt_pointer, x5  ← stored here
  │
start_kernel → setup_arch:
               setup_machine_fdt(__fdt_pointer)  ← consumed here
```

`x21` is an **architectural callee-saved register** (x19-x28 are callee-saved per
AAPCS64). The compiler and assembler MUST preserve these across function calls.
This is how the FDT address survives 10+ function calls without being explicitly
passed as an argument through each one.

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