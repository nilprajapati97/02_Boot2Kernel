# What Is `__primary_switched` — Complete Conceptual Overview

## One-Line Definition

`__primary_switched` is the **last assembly function** executed before the Linux kernel's
C runtime takes over. It is the bridge between raw hardware initialization and a fully
functional C execution environment.

---

## The Name Explained

The function name encodes its meaning precisely:

| Part | Meaning |
|---|---|
| `__primary` | Runs on the **primary (boot) CPU** only — not on secondary CPUs |
| `switched` | The MMU has been **switched on** — we are now in virtual address space |
| `__` prefix | Internal/local kernel function, not exported as a symbol |
| `_LOCAL` suffix in macro | Not visible outside this translation unit — `SYM_FUNC_START_LOCAL` |

---

## What It Is NOT

Understanding what this function is NOT helps clarify what it IS:

- It is NOT `primary_entry` — that is where the boot CPU first lands with MMU OFF
- It is NOT `__primary_switch` — that enables the MMU and jumps here
- It is NOT `start_kernel` — that is the first C function (init/main.c)
- It is NOT part of the secondary CPU boot path (`__secondary_switched` does that)

---

## Its Exact Position in the Transition Chain

```
primary_entry()
  ↓ [MMU OFF, physical addresses]
__cpu_setup()           ← configure MMU registers (TCR, MAIR, SCTLR)
  ↓
__primary_switch()
  ↓ bl __enable_mmu    ← MMU TURNS ON HERE. TTBR1 now maps kernel.
  ↓ bl __pi_early_map_kernel  ← KASLR: relocate kernel, fix page tables
  ↓ ldr x8, =__primary_switched  ← load VIRTUAL address of this function
  ↓ br x8             ← PC JUMPS to virtual address space
__primary_switched()   ← YOU ARE HERE
  ↓ bl start_kernel   ← C code begins — never returns
```

The `br x8` in `__primary_switch` is the single most consequential branch in the
entire kernel: it crosses the physical↔virtual boundary permanently.

---

## What It Accomplishes — The Nine Tasks

```
__primary_switched does exactly 9 things:

  Task 1:  init_cpu_task           →  CPU identity: current task, stack, per-CPU
  Task 2:  msr vbar_el1            →  Exception vectors: safe exception handling
  Task 3:  stp x29,x30 / mov x29  →  C stack frame: ABI-compliant call setup
  Task 4:  str_l __fdt_pointer     →  Save FDT: device tree accessible to C
  Task 5:  kimage_voffset          →  VA↔PA translation foundation
  Task 6:  set_cpu_boot_mode_flag  →  EL1/EL2 decision recorded for KVM/hotplug
  Task 7:  kasan_early_init        →  KASAN shadow memory mapped (if enabled)
  Task 8:  finalise_el2            →  Exception level finalized (VHE decision)
  Task 9:  bl start_kernel         →  Hand off to C — never returns
```

Every single task is MANDATORY. Remove any one and the kernel dies with no diagnostic.

---

## Why It Must Be Assembly (Not C)

`__primary_switched` cannot be written in C because:

1. **Stack:** C requires a valid stack. `init_cpu_task` SETS UP the stack — this must
   happen IN assembly before any C function frame can be created.

2. **`current`:** C code uses `current` macro (which reads `sp_el0`). `sp_el0` is set
   by `init_cpu_task`. Before that instruction, C code calling `current` returns garbage.

3. **VBAR:** C code can trigger exceptions (page faults, IRQs). The vector table must
   be set BEFORE entering C. This ordering cannot be expressed in C.

4. **`kimage_voffset`:** The calculation uses `adrp` which is PC-relative. The compiler
   cannot generate this calculation for a C variable assignment — it requires the precise
   PC location in virtual space to compute VA − PA.

5. **`finalise_el2`:** May execute `ERET` — assembly-only. The exception level switch
   is architecturally required to use the `ERET` instruction.

---

## The Contract This Function Upholds

**Precondition (guaranteed ON ENTRY):**
- MMU is ON. TTBR1 maps kernel virtual space.
- x0 = `__pa(KERNEL_START)` — physical load address
- x20 = `cpu_boot_mode` — EL1 or EL2 value
- x21 = FDT physical address — carried since `preserve_boot_args`
- SP = `early_init_stack` — temporary boot stack

**Postcondition (guaranteed ON EXIT to `start_kernel`):**
- Current task: `init_task` (PID 0, swapper)
- Kernel stack: `init_stack` (16KB, valid frame chain)
- Exceptions: safe — `VBAR_EL1 = &vectors`
- `kimage_voffset`: computed — `virt_to_phys()` works
- `__fdt_pointer`: set — device tree accessible
- `__boot_cpu_mode`: recorded — KVM and hotplug can query it
- KASAN shadow: mapped (if configured)
- Exception level: finalized (EL1 or EL2/VHE)

---

## Section Placement (`.text` vs `.idmap.text`)

`__primary_switched` lives in the regular `.text` section — NOT `.idmap.text`.

```
.idmap.text:   primary_entry, __primary_switch, __enable_mmu
               (accessible via TTBR0 identity map — runs before MMU or in low VA)

.text:         __primary_switched, start_kernel, all normal kernel code
               (accessible ONLY via TTBR1 kernel page table — requires MMU ON)
```

This is why `__primary_switch` CANNOT call `__primary_switched` directly with `bl`.
`bl` would use PC-relative addressing pointing into `.idmap.text` space. Instead it
uses `ldr x8, =__primary_switched` (absolute VA from literal pool) + `br x8` to jump
into `.text` in kernel virtual space.

---

## Interview Summary (One Paragraph)

> "`__primary_switched` is the first function in kernel virtual address space. It
> has a strict one-way contract: receive a CPU with the MMU on but no C runtime
> infrastructure, and deliver a CPU ready for `start_kernel()`. Every instruction
> is mandatory and ordered by dependencies — stack before VBAR, kimage_voffset
> before KASAN, everything before start_kernel. It cannot be written in C because
> the very things C requires (stack, current, VBAR) are what this function creates."

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