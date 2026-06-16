# Interview Questions & Answers — `__primary_switched` Overview

## Q1: "What is `__primary_switched` and why does it exist?"

**A (30-second answer):**
> "`__primary_switched` is the last assembly function before `start_kernel()`. It runs
> with the MMU on, in kernel virtual address space. Its job is to set up everything
> C code needs: the task structure, kernel stack, exception vectors, the virtual-to-physical
> offset, KASAN shadow memory, and the exception level. Without it, `start_kernel()`
> would crash on its very first instruction."

**A (deep answer for follow-up):**
The function exists at a specific architectural seam. Before it, everything runs in
physical/identity-mapped space (`.idmap.text`). After it, everything runs in kernel
virtual space (`.text`). The function establishes the nine prerequisites that C code
needs but cannot establish for itself — because establishing them requires assembly
constructs (like `msr sp_el0`, `msr vbar_el1`, `eret`) that the C compiler cannot generate.

---

## Q2: "Why can't this function be written in C?"

**A:**
Five reasons, each blocking C usage:

1. **Stack creation:** C requires a valid stack BEFORE any function frame. `init_cpu_task`
   switches to `init_stack` — this must be done IN assembly before any C frame exists.

2. **`current` pointer:** C uses `current` = `mrs xN, sp_el0`. `sp_el0` is set by
   `msr sp_el0, x4` in `init_cpu_task`. C code before this line would call `current`
   and get garbage.

3. **VBAR_EL1:** Setting `VBAR_EL1` to safe vectors must happen before any C function
   can take an exception. The ordering constraint between stack setup and VBAR setup
   cannot be expressed in C.

4. **`kimage_voffset` computation:** The `adrp x4, _text` instruction is PC-relative
   and yields the VIRTUAL address precisely because PC is in virtual space. The C
   compiler has no way to express "give me the PC-relative virtual address of `_text`
   as a runtime computation minus a passed-in physical address."

5. **`finalise_el2`:** Contains `ERET` to switch exception levels. `ERET` is only
   emittable from assembly.

---

## Q3: "Walk me through the order of operations and explain WHY that order."

**A (dependency-driven explanation):**
```
init_cpu_task       FIRST — provides valid SP and current
  │
  └► msr vbar_el1  SECOND — needs valid SP (exception entry uses SP)
       │
       └► stp/mov  THIRD — needs valid SP + VBAR (any fault during push handled)
            │
            ├► str_l __fdt_pointer  (no order constraint relative to kimage_voffset)
            │
            ├► kimage_voffset      BEFORE kasan — kasan uses phys_to_virt internally
            │       │
            │       └► kasan_early_init  BEFORE start_kernel — first C instruction
            │                            is already KASAN-instrumented
            │
            └► finalise_el2       BEFORE start_kernel — EL must be final
                    │
                    └► bl start_kernel   LAST — all 9 tasks complete
```

The key insight: **removing or reordering any step makes the kernel fail silently** —
because the diagnostic infrastructure you'd need to debug the failure is the thing you
just broke.

---

## Q4: "What happens if an exception fires before `msr vbar_el1`?"

**A:**
The CPU hardware computes the handler address: `VBAR_EL1 + offset`.
At this point, `VBAR_EL1 = 0` (or garbage from firmware).

Scenario: synchronous exception (data abort) fires. CPU adds offset `0x200` (current
EL, SP_ELx group). Handler address = `0x0 + 0x200 = 0x200`.

The MMU looks up virtual address `0x200`:
- TTBR0 (identity map): covers PA range — VA `0x200` maps to PA `0x200` (if that range
  is mapped). But PA `0x200` is likely ROM/BIOS memory, not a valid exception handler.
- TTBR1 (kernel): `0x200` is in the low address range, NOT covered by TTBR1 which handles
  `0xFFFF800000000000+`.

Result: Translation fault → CPU tries to dispatch THAT exception → same VBAR → same
`0x200` → recursive translation fault. The CPU hangs in an infinite loop with no output.
No serial console, no UART, no diagnostic — just a silent hang.

**This is why `init_cpu_task` (which gives us a valid SP) must complete FIRST** — a valid
SP is the prerequisite for safe exception entry, and safe exception entry is the prerequisite
for installing a non-zero VBAR.

---

## Q5: "What is `early_init_stack` and why is it temporary?"

**A:**
`early_init_stack` is a small boot-time-only stack defined in the kernel linker script.

```c
// arch/arm64/kernel/vmlinux.lds.S
PERCPU_SECTION(...)
  early_init_stack = .;      // 4 pages (16KB) of per-CPU boot stack
```

It is used because at boot entry, `init_task` isn't set up yet — we can't use `init_stack`
before setting up `init_task` as the current task.

It is TEMPORARY because:
1. It has no `thread_info` at its base (no task struct)
2. It has no final frame sentinel (unwinder would walk off the end)
3. It is per-CPU but not associated with any task — if an exception handler calls
   `current` while on `early_init_stack`, it gets garbage

`init_cpu_task` in `__primary_switched` switches SP to `init_stack` — the proper
16KB stack for PID 0 with a valid `thread_info` at the base.

---

## Q6: "What is KASLR and how does it interact with `__primary_switched`?"

**A:**
KASLR (Kernel Address Space Layout Randomization) randomizes where the kernel is loaded
in both physical and virtual address space on each boot.

Before `__primary_switched` runs, `__pi_early_map_kernel` has:
1. Chosen a random physical load address (e.g., `0x52A00000` instead of `0x40080000`)
2. Chosen a random virtual base (e.g., `0xFFFFFF9A_B3200000` instead of `0xFFFFFF80_10080000`)
3. Applied ELF relocations to patch all absolute addresses in the kernel image
4. Patched the literal pool entry `ldr x8, =__primary_switched` to the new VA

In `__primary_switched`, the `kimage_voffset` computation:
```asm
adrp  x4, _text    // gives RANDOMIZED virtual address of _text
sub   x4, x4, x0   // x0 = RANDOMIZED physical load address
```
...automatically captures the correct KASLR offset. No hardcoded values anywhere.

The security benefit: `virt_to_phys()` and `phys_to_virt()` work correctly regardless
of where KASLR placed the kernel, AND the offset changes every boot, making exploitation
significantly harder.

---

## Q7: "What does `ASM_BUG()` at the end mean?"

**A:**
`ASM_BUG()` expands to a `BRK #BUG_BRK_IMM` instruction — a software breakpoint.

It exists because `bl start_kernel` must NEVER return. `start_kernel` ends in
`rest_init()` which calls `cpu_startup_entry()` which is an infinite idle loop.

If it somehow DID return (bug in `start_kernel`, stack corruption, etc.), the `BRK`
instruction fires a debug exception, which the vector table handler catches and converts
to a kernel `BUG()` → kernel panic with:
```
BUG: failure at arch/arm64/kernel/head.S:LINE
```

This is NOT defensive programming — it is a design contract encoded in machine code:
**"If execution reaches this instruction, the kernel's core architectural invariant
has been violated, and continuing would cause undefined behavior."**

The ARM64 `BRK` vs `UDF` choice: `BRK` is preferred because it generates a debug
exception (vector 0x200 group, synchronous) which the kernel's debug exception handler
can format into a readable panic message. `UDF` generates an undefined instruction
exception — also caught, but with different panic formatting.

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