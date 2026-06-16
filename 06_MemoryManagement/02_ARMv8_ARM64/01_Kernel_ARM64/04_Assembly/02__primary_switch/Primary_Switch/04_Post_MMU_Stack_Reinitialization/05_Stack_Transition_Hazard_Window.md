# The Stack Transition Hazard Window

**Context:** The brief period between MMU enable and stack reinit where both stacks are problematic  
**Source:** `arch/arm64/kernel/head.S`, `__enable_mmu` through `__primary_switched`

---

## 0. Defining the Hazard Window

There is a brief period in boot where the CPU is executing with the MMU enabled
but the stack pointer still holds a pre-MMU value:

```
Timeline:
                                      │
msr sctlr_el1 (MMU ON)                │ ← Hazard window START
isb                                   │
ret (return from __enable_mmu)        │   SP still = early_init_stack PA
... (execution in __primary_switch)   │
ldr x8, =__primary_switched           │
br x8  (jump to VA)                   │
  __primary_switched:                 │
  add sp, x4, #THREAD_SIZE            │ ← Hazard window END
                                      │
```

During this window, the SP holds a **physical address** that was used as a stack
address before the MMU was enabled.

---

## 1. Why This Window Exists

The design choice is: enable the MMU first, then set up the new stack.

**Alternative**: Set up the new stack before enabling the MMU.

Why the alternative doesn't work:
1. The new stack (`init_thread_union`) has a **virtual address** (e.g., `0xFFFF_8000_...`)
2. Before the MMU is enabled, writing `sp = VA` would make `sp` point to a
   virtual address that doesn't map to any valid physical memory
3. Any function call in that state would try to push/pop to a VA, causing
   unpredictable behavior (the hardware sees the VA as a PA and may write to
   wrong physical memory)

Therefore: MMU must be enabled first (to make kernel VAs valid), then SP is
switched to a kernel VA.

---

## 2. What Code Runs in the Hazard Window

After `ret` from `__enable_mmu` and before `add sp` in `__primary_switched`:

```asm
// __primary_switch, after bl __enable_mmu returns:
ldr     x8, =__primary_switched   // Load VA of __primary_switched into x8
br      x8                         // Jump to VA (crosses into TTBR1 range)

// __primary_switched:
adrp    x4, init_thread_union     // Compute VA of new stack
add     sp, x4, #THREAD_SIZE      // SP = new stack top ← hazard ends
```

**During `ldr x8, =__primary_switched` and `br x8`:**
- These instructions do NOT use the stack at all
- They operate purely on registers (x8, PC)
- No push/pop, no function calls, no compiler-generated stack access

So the hazard window is **instruction-safe**: the code in the window was
carefully hand-written to avoid any stack operations.

---

## 3. Why No Stack Use Is Safe Here

The key property: **no function calls, no exceptions, no IRQs** in the hazard window.

1. **No function calls:** `ldr x8, =...` and `br x8` don't use the stack.
   The `bl` instruction (which would push a return address) is NOT used for
   the final jump. `br x8` is used (unconditional branch through register —
   no link).

2. **No exceptions:** DAIF = 0b1111 (all exceptions masked) from `primary_entry`.
   No IRQ, FIQ, SError, or debug exceptions can fire.

3. **No synchronous exceptions during this window:** The code touches no memory
   (only registers and instruction fetch), so no data aborts. Instruction fetches
   use the identity map (TTBR0) → guaranteed valid for `.idmap.text`.

---

## 4. The PAN (Privileged Access Never) Concern

With `SCTLR_EL1.SPAN = 0` (configured in `INIT_SCTLR_EL1_MMU_ON`), PAN is
enabled by default after the `msr sctlr_el1` instruction.

Wait — what does `SPAN = 0` mean?

```
SCTLR_EL1.SPAN = 0:  PAN is not disabled by SCTLR_EL1 (PAN state is preserved/set)
```

PAN was introduced in ARMv8.1. When PAN is enabled (`PSTATE.PAN = 1`):
- EL1 code cannot access user-space VAs (VA[63:48] = 0x0000)
- Attempting to do so causes a Permission Fault

With PAN enabled, the old `sp` value (a physical address like `0x4000_8000`
which has VA[63:48] = 0x0000) becomes a **forbidden address** from EL1.

**Does this cause a fault in the hazard window?**

Only if the code in the hazard window USES `sp`. As established above, it doesn't.
- `ldr x8, =__primary_switched` loads from the literal pool (PC-relative, in kernel image)
- `br x8` is a register branch (no memory access)

So PAN is not a problem in the hazard window.

---

## 5. What If an Exception Did Fire in the Hazard Window

Suppose, hypothetically, an SError (asynchronous external abort) fires during
the hazard window:

```
Hardware:
1. Save PSTATE → SPSR_EL1
2. Save PC → ELR_EL1
3. Jump to VBAR_EL1 + 0x180 (SError from EL1 with SP_EL1, kernel vector)

Exception handler:
1. Exception entry code pushes registers to [SP_EL1 - ...]:
   SP_EL1 = early_init_stack PA (e.g., 0x4000_8000)
   Push to VA 0x4000_7FE0...
   
   Is VA 0x4000_7FE0 mapped?
   VA[63:48] = 0x0000 → TTBR0 range → identity map
   Identity map: 0x4000_7FE0 → PA 0x4000_7FE0 (mapped)
   Store succeeds!

   BUT: PAN is active (EL1, user-range VA) → Permission Fault

→ Exception-within-exception: double fault
→ CPU uses SP_EL1h (same SP) → triple fault
→ Machine halts
```

This is exactly why `DAIF = 0b1111` (all masked) is maintained throughout the
hazard window. The kernel cannot handle exceptions during this period.

---

## 6. The `VBAR_EL1` Question

When was `VBAR_EL1` (vector base address register) set?

```asm
// arch/arm64/kernel/head.S — __primary_switched:
adr_l   x5, vectors             // VA of exception vector table
msr     vbar_el1, x5            // Set VBAR_EL1
```

`VBAR_EL1` is set in `__primary_switched`, **after** the stack reinit. This
is correct: before `__primary_switched`, `VBAR_EL1` is either:
- 0 (reset value) — exceptions would vector to VA 0x0000_0000_0000_0000
- A value set by the bootloader (not trusted)

During the hazard window, `VBAR_EL1` is not yet set to the Linux vectors table.
But since DAIF masks all exceptions, this doesn't matter — no exceptions will
fire to expose the bad VBAR.

---

## 7. Duration and Reliability of the Hazard Window

**Number of instructions in the hazard window:** ~4-6 instructions:
- `isb` (end of set_sctlr_el1)
- `ret` (in __enable_mmu)
- Any instructions in __primary_switch after the `ret` before `br x8`
- `ldr x8, =__primary_switched`
- `br x8`
- `adrp x4, init_thread_union` (first instruction in __primary_switched)
- `add sp, x4, #THREAD_SIZE` ← window ends

**Clock cycles:** ~10-20 cycles at typical CPU frequencies.

**Reliability:** Because the window is:
1. Exception-masked (DAIF=F)
2. Stack-access-free (no push/pop)
3. Short (10-20 cycles)
4. Not interruptible by hardware

It is completely safe.

---

## 8. The Window Does Not Exist on Kexec-Loaded Kernels

On a kexec-loaded kernel, the kexec code in the outgoing kernel parks secondary
CPUs and jumps to the new kernel's `primary_entry` with a clean state. The
same boot path is followed, and the same hazard window exists. The analysis
above applies equally.

The key invariant: the hazard window relies on DAIF masking, which is set
by the bootloader or kexec code before jumping to `primary_entry`. Linux
documents this requirement in `Documentation/arm64/booting.rst`:

> The boot loader must: ... disable all hardware interrupts.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the stack pointer is a dedicated register (SP_EL1 at EL1, SP_EL0 at EL0). SP_EL1 is the stack pointer used by the kernel during normal execution. The AAPCS64 ABI requires the stack to be 16-byte aligned at any instruction that may cause an exception. SCTLR_EL1.SA (bit 3) enables hardware enforcement of this alignment: if SP_EL1 is not 16-byte aligned when a load/store using SP is executed, an SP alignment fault is raised. The frame pointer (x29) is a general-purpose register used by convention to hold the base of the current stack frame. Writing x29 is the first act of any C function that wishes to be unwound.

### Kernel Perspective (Linux ARM64)
After the MMU is enabled, __primary_switch reinitializes the stack pointer to a virtual address. The early boot stack is defined as:
  __INIT_DATA: init_thread_union (size THREAD_SIZE, typically 16 KB)
The LDR instruction loads the VA of init_thread_union + THREAD_SIZE into x0, then MOV sp, x0 sets SP_EL1. This is necessary because the old stack pointer was set to a physical address (before the MMU) and that PA is no longer the correct address for the kernel VA layout. x29 is set to zero (zero frame pointer) to terminate the unwind chain at the first kernel stack frame.

### Memory Perspective (ARMv8 Memory Model)
The kernel stack resides in Normal Inner-Shareable Write-Back Cacheable memory (MT_NORMAL). Once the MMU and D-cache are enabled, all stack accesses (PUSH/POP equivalents: STP/LDP) go through the L1 D-cache. The L1 D-cache write-back policy means that the stack contents are not immediately visible to physical memory until a cache clean or eviction. This is safe for the stack because the kernel does not use DMA to read stack memory. The stack pointer reinitalization at VA is a hard cut: all future kernel stack frames exist in the high VA kernel mapping.