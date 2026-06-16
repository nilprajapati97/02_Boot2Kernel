# Why the Stack Is Reinitialized After MMU Enable

**Context:** Understanding the `mov sp, x27` instruction in `__primary_switched`  
**Source:** `arch/arm64/kernel/head.S`, line ~528 in `__primary_switched`

---

## 0. The Stack Before MMU Enable

Before `__enable_mmu` is called, `__primary_switch` and all code it calls
(including C functions like `__pi_early_map_kernel`) use a stack.

Where is this stack? The boot stack used before MMU enable is the
**early_init_stack** defined in the linker script:

```asm
// arch/arm64/kernel/head.S — primary_entry:
adr_l   x27, early_init_stack         // x27 = VA of early_init_stack
mov     sp, x27                        // SP = early_init_stack address
```

And in `__primary_switch` itself, there's typically:

```asm
// head.S __primary_switch
...
bl      __enable_mmu             // returns via identity map
bl      __primary_switched       // NOT done as bl — done via ldr/br
```

Actually — in `__primary_switch`, the call to `__primary_switched` is via
`ldr x8, =__primary_switched; br x8` (an indirect jump to a VA), not a `bl`.
This means there's no `ret` from `__primary_switched` to `__primary_switch`.

---

## 1. Two Stacks, Two Worlds

```
World 1: Pre-MMU (physical addresses)
    Stack VA == Stack PA
    Stack location: early_init_stack (a symbol at a PA-as-VA)
    Used by: __cpu_setup callee chain, __primary_switch, __pi_early_map_kernel

World 2: Post-MMU (virtual addresses)
    Stack VA != Stack PA (typically)
    Stack location: per-CPU boot stack in kernel VA space
    Used by: __primary_switched, start_kernel, all kernel threads
```

The **reinitializing** of the stack is the act of switching from World 1's stack
to World 2's stack.

---

## 2. The `early_init_stack` Symbol — World 1 Stack

```ld
// arch/arm64/kernel/vmlinux.lds.S (simplified)
.bss (NOLOAD) : {
    ...
    . = ALIGN(PAGE_SIZE);
    . += SZ_4K;                    // Reserve 4096 bytes
    early_init_stack = .;          // Symbol at the TOP (stack grows down on ARM64)
}
```

Properties:
- Size: 4KB (`SZ_4K`)
- Location: In the BSS section, which is at a physical address = virtual address
  in the **identity map** (VA ≈ PA for kernel image symbols during early boot)
- Type: Raw memory, no guard pages, no canary

Wait — is `early_init_stack` the VA or PA at this point?

Before the MMU is enabled:
- `adr_l x27, early_init_stack` computes the **physical address** of the symbol
  using PC-relative arithmetic (ADR is PC-relative, works with physical PC)
- This PA is used as SP
- Since pre-MMU code runs at PA, `sp` holds a PA value

After the MMU is enabled:
- The kernel's VA space is set up
- `early_init_stack` as a symbol in the linked kernel has a **virtual address**
  (e.g., `0xFFFF_8000_????_????`)
- But the `sp` register still holds the old **physical address** value

---

## 3. Why the Old Stack Is Invalid After MMU Enable

After `br x8` jumps to `__primary_switched`:
- PC is in the TTBR1 range (VA `0xFFFF_8000_...`)
- `sp` register holds a value from the pre-MMU world

**Case 1: SP holds a PA that happens to be identity-mapped**

If the kernel's physical load address is, say, `0x4000_0000`, and
`early_init_stack` PA is `0x4000_2000`, then after MMU enable:
- The identity map (TTBR0) maps VA `0x4000_2000` → PA `0x4000_2000`
- `sp = 0x4000_2000` as a VA would work briefly
- BUT: the identity map is in TTBR0 (user space range, VA[63:48]=0x0000)
- Kernel code using `sp = 0x4000_2000` (a user-space VA) would trigger **PAN**
  (Privileged Access Never, if enabled): accessing user VA from EL1 → fault

**Case 2: SP holds a PA that is NOT in any valid kernel VA mapping**

Most kernel VAs are in the `0xFFFF_0000_0000_0000` range (TTBR1).
A physical address like `0x4000_2000` is NOT in TTBR1's range. Any stack
push/pop would use TTBR0, which won't have a mapping for arbitrary PAs once
the identity map is torn down.

**Conclusion:** The old `sp` value is unusable in the post-MMU world.

---

## 4. The `__primary_switched` Stack Setup

In `__primary_switched`, the very first operations are:

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
    adrp    x4, init_thread_union         // VA of init_task's stack
    add     sp, x4, #THREAD_SIZE          // SP = top of init_task stack
    adrp    x5, init_task                 // VA of init_task
    msr     sp_el0, x5                    // SP_EL0 = init_task (for EL0 access)
    ...
```

This sets `sp` to the **virtual address** of the top of `init_task`'s kernel
stack (which is allocated as part of the `init_thread_union`).

---

## 5. `init_thread_union` — The New Stack

```c
// init/init_task.c
union thread_union init_thread_union __init_task_data = {
    INIT_THREAD_INFO(init_task)
};
```

`thread_union` is defined as:

```c
// include/linux/sched.h
union thread_union {
    struct task_struct task;    // OR thread_info
    unsigned long stack[THREAD_SIZE/sizeof(long)];
};
```

Size: `THREAD_SIZE = 1 << THREAD_SHIFT = 1 << 14 = 16384 bytes = 16 KB`

The stack occupies 16 KB of virtual address space in the kernel's `.data..page_aligned`
section. It is mapped by `swapper_pg_dir` (built by `__pi_early_map_kernel`).

---

## 6. Stack Pointer Convention: Top vs. Bottom

ARM64 stacks grow **downward**: SP starts at the top (high address) and decreases
as items are pushed.

```
VA of init_thread_union:   0xFFFF_8000_AABB_0000   ← Bottom (low address)
                           ....
                           ....
VA top of thread_union:    0xFFFF_8000_AABB_4000   ← Top = THREAD_SIZE above base
                                                     = 0xFFFF_8000_AABB_0000 + 0x4000
```

`add sp, x4, #THREAD_SIZE` sets SP to the top:
```
sp = VA of init_thread_union + THREAD_SIZE
   = VA of init_thread_union + 0x4000
   = top of 16KB stack area
```

The first `push` (or `str` to `[sp, #-8]!`) decrements SP and stores into the
stack area.

---

## 7. Why SP Must Be Reinitialized Before Any C Call

`__primary_switched` calls `start_kernel()`:

```asm
bl      start_kernel
```

`start_kernel` is a C function that:
1. Creates a stack frame (by `sub sp, sp, #frame_size` in the function prologue)
2. Stores callee-saved registers to the stack frame
3. Calls nested functions, each of which creates their own stack frames
4. Calls `smp_setup_processor_id()`, `setup_arch()`, etc.

If `sp` pointed to the old pre-MMU stack (now invalid VA), the very first
`sub sp, sp, #frame_size` in `start_kernel`'s prologue would produce an SP
value in an unmapped or wrong memory region. The subsequent `stp` to `[sp]`
would:
- Either fault immediately (access to unmapped VA)
- Or silently corrupt memory (if the PA from a stale TTBR0 entry happens to
  be valid and writable — a security hole)

---

## 8. Frame Pointer Reset and Stack Unwinding

Alongside the `sp` reset, `__primary_switched` also resets the frame pointer:

```asm
mov     x29, xzr   // x29 (frame pointer) = NULL
```

This terminates the stack backtrace chain. The kernel's stack unwinder
(`arch/arm64/kernel/stacktrace.c`) follows frame pointer chains (`x29` links)
to print backtraces. Setting `x29 = NULL` marks the bottom of the call stack.

Without this reset:
- `x29` would still contain a value from the pre-MMU stack frame chain
- Stack unwinds would follow the old chain into unmapped (pre-MMU) memory
- Kernel panic backtraces during early boot would show garbage or fault

---

## 9. Summary: Why Stack Reinit Is Necessary

| Reason | Detail |
|---|---|
| Pre-MMU SP holds a PA value | PA != kernel VA; unmapped in TTBR1 |
| PAN prevents user-VA stack access | Identity map (TTBR0) is user range; EL1 can't use it with PAN |
| C functions need valid stack | ABI requires SP to point to writeable, mapped memory |
| Stack unwinder correctness | x29 must be reset to terminate backtrace chain |
| Memory safety | Continuing with old SP risks faulting or corrupting random PAs |

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The MMU in ARMv8-A is enabled by writing bit 0 (M) of SCTLR_EL1 to 1 via an MSR instruction followed by an ISB. The ISB is the critical barrier: it flushes the instruction pipeline so that all instructions fetched AFTER the ISB use the new memory system configuration. Before the MMU is enabled, the CPU operates in a flat physical address space. After the bit is set, the TLB, page-table walker, TTBR0/TTBR1, TCR_EL1, and MAIR_EL1 all become active simultaneously. There is no intermediate state.

### Kernel Perspective (Linux ARM64)
Linux enables the MMU in __enable_mmu (arch/arm64/kernel/head.S), called from __primary_switch. The sequence is:
  1. Write TTBR0_EL1 (identity map root).
  2. Write TTBR1_EL1 (kernel map root).
  3. ISB to synchronize TTBR writes.
  4. Write SCTLR_EL1 with M=1 (via set_sctlr_el1 macro).
  5. ISB to flush the pipeline.
  6. RET -- the very next instruction is fetched through the new MMU.
The identity map ensures that the physical address of the code after the RET is also mapped at the same VA (PA==VA), so no instruction-fetch fault occurs.

### Memory Perspective (ARMv8 Memory Model)
The moment SCTLR_EL1.M is written to 1 and the ISB completes, the ARMv8 memory model transitions from "flat PA" to "two-stage VA->PA via page tables". The identity map (stored in __idmap_text_start to __idmap_text_end, mapped in the .idmap.text section) covers the physical pages of the MMU-enable code so the VA==PA invariant holds during the critical window. Without the identity map, the instruction fetch for the RET after set_sctlr_el1 would target a VA that has no valid TLB entry, causing a translation fault with no exception handler installed yet.