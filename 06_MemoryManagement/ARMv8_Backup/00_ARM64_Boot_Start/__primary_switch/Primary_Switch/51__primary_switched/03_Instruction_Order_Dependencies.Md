# Instruction Ordering — WHY This Exact Sequence

## The Most Important Interview Question

> "Could you reorder any instructions in `__primary_switched` without breaking the kernel?"

The answer is NO — and explaining WHY requires understanding the dependency graph
between each operation. This document maps every dependency edge.

---

## Dependency Graph

```
                        ENTRY
                          │
                          │  x0 = __pa(KERNEL_START)
                          │  x20 = cpu_boot_mode
                          │  x21 = FDT physical address
                          │  SP  = early_init_stack (temporary)
                          │  VBAR_EL1 = 0 (GARBAGE)
                          ▼
              ┌───────────────────────┐
              │   init_cpu_task       │  ◄── MUST BE FIRST
              │  (sp_el0, SP, frame,  │
              │   SCS, tpidr_el1)     │
              └──────────┬────────────┘
                         │  Provides: valid SP, valid current, per-CPU base
                         │
                         ▼
              ┌───────────────────────┐
              │   msr vbar_el1, x8    │  ◄── Needs valid SP (exception entry uses SP)
              │   isb                 │
              └──────────┬────────────┘
                         │  Provides: safe exception handling
                         │
                         ▼
              ┌───────────────────────┐
              │  stp x29,x30,[sp]!    │  ◄── Needs valid SP (writes to stack)
              │  mov x29, sp          │       Needs VBAR (any fault during stp handled)
              └──────────┬────────────┘
                         │  Provides: valid C stack frame for bl calls
                         │
                         ├──────────────────────────────────┐
                         │                                  │
                         ▼                                  ▼
              ┌────────────────────┐          ┌─────────────────────────┐
              │ str_l __fdt_pointer│          │ kimage_voffset          │
              │ (x21 → global)     │          │ (adrp _text - x0)       │
              └────────────────────┘          └──────────┬──────────────┘
                         │                               │  Provides: virt↔phys works
                         │                               │
                         │                               ▼
                         │                  ┌────────────────────────────┐
                         │                  │  kasan_early_init          │  ◄── Needs kimage_voffset
                         │                  │  (map shadow region)       │       (shadow mapping uses
                         │                  └──────────┬─────────────────┘        phys_to_virt internally)
                         │                             │
                         │    ┌────────────────────────┘
                         │    │  Provides: KASAN shadow mapped
                         ▼    ▼
              ┌───────────────────────┐
              │  set_cpu_boot_mode_flag│  ◄── Needs valid SP (bl corrupts x30)
              │  (bl — uses stack)     │       Needs VBAR (bl can trigger faults)
              └──────────┬────────────┘
                         │
                         ▼
              ┌───────────────────────┐
              │  finalise_el2         │  ◄── Needs VBAR, SP, stack frame
              │  (bl — may ERET)      │       Must be BEFORE start_kernel
              └──────────┬────────────┘
                         │  Provides: Exception level finalized
                         │
                         ▼
              ┌───────────────────────┐
              │  ldp x29,x30,[sp],#16 │
              │  bl start_kernel      │  ◄── Needs EVERYTHING above complete
              │  ASM_BUG()            │
              └───────────────────────┘
```

---

## Edge-by-Edge Dependency Table

| Operation | Depends On | What Breaks If Moved Earlier |
|---|---|---|
| `msr vbar_el1` | `init_cpu_task` (valid SP) | Exception entry writes to SP. If SP = garbage, first exception corrupts random memory |
| `stp x29,x30` | `msr vbar_el1` (safe exceptions) | A fault during `stp` (misaligned SP) would dispatch to VBAR=0 → crash |
| `str_l __fdt_pointer` | `stp x29,x30` (C stack frame exists) | Can be anywhere after valid SP, but logically grouped with other global stores |
| `kimage_voffset` store | `adrp x4, _text` (virtual PC needed) | Must be computed while PC is in virtual space (after `br x8`). Trivially satisfied |
| `kasan_early_init` | `kimage_voffset` stored | `kasan_early_init` calls `phys_to_virt()` internally → needs `kimage_voffset` |
| `kasan_early_init` | `msr vbar_el1` | It is a C function call (`bl`). Any page fault during mapping needs a handler |
| `set_cpu_boot_mode_flag` | `stp` frame exists | `bl` overwrites x30. Stack frame must be set up so the pushed LR is valid |
| `finalise_el2` | `msr vbar_el1` | `finalise_el2` may execute `ERET` to drop to EL1 — the EL1 state needs VBAR set |
| `finalise_el2` | KASAN shadow mapped | After `finalise_el2`, execution resumes (at EL1 or EL2). The very next instruction must have KASAN mapped |
| `bl start_kernel` | ALL of the above | `start_kernel()`'s first C instruction is KASAN-instrumented, uses `current`, uses per-CPU, may take an interrupt |

---

## The Three Critical "What If" Questions

### Q1: What if `msr vbar_el1` happens BEFORE `init_cpu_task`?

```
Sequence:
  msr vbar_el1, x8   ← VBAR now valid
  init_cpu_task ...  ← stack switch happening here
```

**Scenario:** A Data Abort fires during the stack switch (e.g., speculative prefetch
hits an unmapped address in init_stack).

**ARM64 exception entry behavior:**
The CPU pushes exception state (ELR, SPSR) and switches to SP_ELx (EL1 stack pointer).
SP_ELx IS the current SP — which is `early_init_stack + some_offset` — NOT `init_stack`.

The exception handler in `entry.S` immediately does:
```asm
sub   sp, sp, #PT_REGS_SIZE    // make room for pt_regs on stack
stp   x0, x1, [sp, #16*0]     // save registers
...
```

This stack frame is written to `early_init_stack`. That's fine in isolation, but
`current` (sp_el0) still points to the old value (or 0). The handler calls C code
which calls `current` → garbage task pointer → panic or corruption.

**Verdict:** VBAR before `init_cpu_task` = valid exception dispatch to corrupted context.

---

### Q2: What if `kasan_early_init` happens BEFORE `kimage_voffset`?

`kasan_early_init` (in `mm/kasan/init.c`) calls `kasan_map_populate()` which calls
`pfn_to_kaddr(pfn)` which expands to:

```c
static inline void *pfn_to_kaddr(unsigned long pfn)
{
    return __va(pfn << PAGE_SHIFT);
}
#define __va(x)  ((void *)__phys_to_virt((phys_addr_t)(x)))
#define __phys_to_virt(x)  ((unsigned long)((x) + kimage_voffset))
```

If `kimage_voffset` = 0 (not yet computed), then `__phys_to_virt(PA)` = PA.
The KASAN shadow is mapped at the WRONG virtual address.
Every subsequent KASAN shadow access goes to a wrong page → page fault → hang.

**Verdict:** `kasan_early_init` before `kimage_voffset` = KASAN maps shadow at wrong VA.

---

### Q3: What if `finalise_el2` happens BEFORE `kasan_early_init`?

`finalise_el2` may execute an `ERET` instruction (when dropping from EL2 to EL1).
After ERET, the CPU continues executing at EL1.

The instruction AFTER `bl finalise_el2` is `ldp x29, x30, [sp], #16`.
This instruction is KASAN-instrumented. The compiler has inserted a shadow check:
```c
shadow_byte = *(char *)((sp_value >> 3) + KASAN_SHADOW_OFFSET);
```
If KASAN shadow is not mapped → translation fault at `KASAN_SHADOW_OFFSET + sp_value/8`
→ no handler → hang.

Actually `finalise_el2` BEFORE `kasan_early_init` is OK for THAT reason — `ldp` itself
may not be instrumented (it's asm). But any C function called immediately after would
be instrumented. More concretely: if an interrupt fires after ERET and before
`kasan_early_init`, the IRQ handler is C code = KASAN-instrumented = shadow access
= page fault.

**Verdict:** `finalise_el2` before `kasan_early_init` creates a window where interrupts
can kill the kernel. The current order eliminates this window.

---

## The Minimal Required Partial Order (Formalized)

Given operations A, B, C, D, E, F, G, H, I:

```
A = init_cpu_task
B = msr vbar_el1 + isb
C = stp x29,x30 / mov x29
D = str_l __fdt_pointer
E = kimage_voffset computation + store
F = kasan_early_init (conditional)
G = set_cpu_boot_mode_flag
H = finalise_el2
I = bl start_kernel
```

**Required partial order:**

```
A → B          (valid SP before VBAR)
A → C          (valid SP before stack frame push)
B → C          (safe exceptions before any bl)
E → F          (kimage_voffset before kasan)
B → F          (VBAR before kasan bl call)
C → G          (stack frame before bl set_cpu_boot_mode_flag)
C → H          (stack frame before bl finalise_el2)
F → I          (KASAN mapped before start_kernel)
G → I          (boot mode flag set before start_kernel)
H → I          (EL finalized before start_kernel)
D → I          (fdt_pointer set before start_kernel calls setup_arch)
```

**Operations with no ordering constraint between them:**
- D and E can be swapped (they are independent stores to different globals)
- G and F can be swapped (G = `bl set_cpu_boot_mode_flag`, F = `bl kasan_early_init` — independent)
- G and H can be swapped (both need C, neither depends on each other)

The actual source code places D before E, E before F, F before G, G before H.
D before E is a style/convention choice. The others follow from the analysis above.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the current task (process) is identified at EL0 via TPIDR_EL0 (user thread ID) and at EL1 via SP_EL0. Linux uses SP_EL0 to store the pointer to the current task_struct. SP_EL0 is a dedicated register (not the EL0 stack pointer when running in EL1 -- at EL1, SP_ELx selects either SP_EL0 or SP_EL1 as the active stack, controlled by PSTATE.SP). When the kernel uses SP_EL0 to store the current task pointer, it is using SP_EL0 as a general-purpose register (reading/writing it with MRS/MSR SP_EL0).

### Kernel Perspective (Linux ARM64)
init_cpu_task is a per-CPU variable (or boot-time initialization) that sets up the idle task (init_task / swapper) as the current task. In __primary_switched:
  msr  sp_el0, x23        // x23 holds init_task VA, set SP_EL0 = &init_task
  ldr  x8, [x23, #TSK_TI_CPU]  // verify .cpu field
The current macro in Linux ARM64 expands to:
  mrs x0, sp_el0          // read SP_EL0 as current task_struct pointer
SP_EL0 is never spilled to the stack (it is a system register), making current() essentially a zero-cost operation.

### Memory Perspective (ARMv8 Memory Model)
task_struct for init_task lives in the .data section of the kernel image (statically allocated). Its VA is in the kernel text/data mapping (TTBR1_EL1). When SP_EL0 is set to &init_task, the memory region is already mapped and accessible. The task's stack (thread_union) is in the .init.data section and is also already mapped. After start_kernel -> sched_init(), all subsequent tasks have their task_struct allocated from slab memory in the kernel heap (also in the TTBR1_EL1 region).