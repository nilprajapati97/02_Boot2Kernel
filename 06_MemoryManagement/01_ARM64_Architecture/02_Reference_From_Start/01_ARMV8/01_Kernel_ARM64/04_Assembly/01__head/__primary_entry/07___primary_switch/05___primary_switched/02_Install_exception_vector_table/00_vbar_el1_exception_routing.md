# Install Exception Vector Table — VBAR_EL1, Vectors, and the ISB Barrier

**File**: `arch/arm64/kernel/head.S` — inside `__primary_switched`
**Instructions**:
```asm
adr_l   x8, vectors     // Load virtual address of the vector table
msr     vbar_el1, x8    // Install into VBAR_EL1
isb                     // Instruction Synchronization Barrier
```
**Perspective**: CPU Architecture / ARM64 Exception Model
**Style**: NVIDIA TRM / ARM Architecture Reference Manual

---

## 1. What Is VBAR_EL1?

**VBAR_EL1** (Vector Base Address Register, EL1) is the ARM64 system register
that holds the base address of the **exception vector table** for EL1 software.

When any synchronous or asynchronous exception is taken to EL1, the CPU
automatically fetches the handler from:

```
PC ← VBAR_EL1 + offset

Offset table (each entry is 128 bytes = 32 instructions):
┌─────────────────────────────────────────────────────────────────────────────┐
│  Offset   │  Source                        │  Type                         │
├───────────┼────────────────────────────────┼───────────────────────────────┤
│  0x000    │  Current EL, SP_EL0            │  Synchronous                  │
│  0x080    │  Current EL, SP_EL0            │  IRQ / vIRQ                   │
│  0x100    │  Current EL, SP_EL0            │  FIQ / vFIQ                   │
│  0x180    │  Current EL, SP_EL0            │  SError / vSError             │
│  0x200    │  Current EL, SP_ELx (EL1h)     │  Synchronous  ← kernel faults │
│  0x280    │  Current EL, SP_ELx (EL1h)     │  IRQ          ← kernel IRQs   │
│  0x300    │  Current EL, SP_ELx (EL1h)     │  FIQ                          │
│  0x380    │  Current EL, SP_ELx (EL1h)     │  SError                       │
│  0x400    │  Lower EL, AArch64             │  Synchronous  ← syscalls      │
│  0x480    │  Lower EL, AArch64             │  IRQ          ← user IRQs     │
│  0x500    │  Lower EL, AArch64             │  FIQ                          │
│  0x580    │  Lower EL, AArch64             │  SError                       │
│  0x600    │  Lower EL, AArch32             │  Synchronous                  │
│  0x680    │  Lower EL, AArch32             │  IRQ                          │
│  0x700    │  Lower EL, AArch32             │  FIQ                          │
│  0x780    │  Lower EL, AArch32             │  SError                       │
└───────────┴────────────────────────────────┴───────────────────────────────┘

Total table size = 0x800 bytes = 2KB   (ARM64_VECTOR_TABLE_LEN = SZ_2K)
```

The `vectors` symbol in `arch/arm64/kernel/entry.S` is **2KB-aligned**
(required by hardware — VBAR_EL1 must be aligned to at least `0x800`).

---

## 2. Why This MUST Happen Before Any C Code

Before `msr vbar_el1, x8`, the CPU uses whatever address was left in
VBAR_EL1 by `__cpu_setup`. For the EL2 boot path, this is the hyp-stub
vector table (`__hyp_stub_vectors`). For EL1 boot, it is the EL1 reset
value — **architecturally UNKNOWN**.

Any exception in that window dispatches to the wrong handler:
```
Before vbar_el1 install → any fault → CPU jumps to garbage address
                                    → immediate hang or memory corruption
                                    → no backtrace, no console output
```

The sequence in `__primary_switched` is ordered so that:
```
1. init_cpu_task  → valid stack (needed if exception handler runs)
2. msr vbar_el1   → valid exception routing  ← THIS STEP
3. stp x29,x30    → valid frame (needed for backtrace in exception handler)
4. C-level calls  → can now safely fault and recover
```

---

## 3. The `isb` After `msr vbar_el1`

`isb` (Instruction Synchronization Barrier) is **mandatory** after writing
VBAR_EL1.

Without `isb`:
- The CPU pipeline may have speculatively fetched instructions **past** the `msr`
- Those instructions were fetched using the **old** VBAR_EL1 value for address
  prediction
- If any of those prefetched instructions trigger an exception, the CPU might
  use stale exception routing

The `isb` flushes the instruction pipeline, forcing the CPU to re-fetch
all subsequent instructions with the new VBAR_EL1 in effect.

**ARM Architectural Rule**: Any write to a system register that affects
instruction fetch behavior (VBAR, SCTLR) must be followed by an `isb`
before relying on the new value.

---

## 4. NVIDIA Engineering Note: Alignment and SMMU Interaction

On NVIDIA Tegra/Orin platforms, the SMMU (System MMU) translates DMA
addresses. SMMU faults (unsolicited DMA from a rogue peripheral) are
delivered as SError exceptions (Asynchronous Aborts).

```
SMMU fault flow (after vbar_el1 is installed):
  SMMU detects DMA translation fault
  → CPU receives SError
  → PC jumps to VBAR_EL1 + 0x380 (SError from current EL, EL1h)
  → entry.S: kernel_entry → el1_error handler
  → arm64_serror_panic() prints the fault address
  → System panics cleanly with full backtrace

If vbar_el1 was NOT installed:
  → SError dispatches to arbitrary address
  → Silent memory corruption or CPU reset
  → No diagnostic information available
```

This is why VBAR_EL1 installation is one of the **first acts** of
`__primary_switched`, before any hardware interaction that could generate
an asynchronous exception.

---

## 5. `adr_l` vs `adrp` for Loading `vectors`

```asm
adr_l   x8, vectors     // NOT: adrp x8, vectors
```

`adr_l` is a macro that expands to `adrp` + `add` with the full page-aligned
+ low-12-bit offset:

```asm
adrp    x8, vectors          // x8 = page-aligned virtual address
add     x8, x8, :lo12:vectors // x8 += low 12 bits of vectors offset
```

This is needed because `vectors` has a specific 2KB alignment but is not
necessarily at the start of a 4KB page. `adrp` alone would give the 4KB-page
base, missing the offset within the page.

**Constraint**: VBAR_EL1 must be aligned to `0x800` (2KB). The linker
ensures this via `.align 11` before `vectors` in `entry.S`:
```asm
// arch/arm64/kernel/entry.S
    .align  11
SYM_CODE_START(vectors)
    kernel_ventry   1, t, 64, sync   // Synchronous EL1t
    ...
```

Alignment 11 = 2^11 = 2048 bytes = 2KB. ✓

---

## 6. Summary: What This Step Accomplishes

```
Before:  VBAR_EL1 = hyp-stub vectors or unknown reset value
         Any exception = wrong handler = hang or corruption

After:   VBAR_EL1 = &vectors  (kernel's EL1 exception table)
         Any exception = correct kernel handler = recoverable fault,
                         printk message, backtrace, optional panic

Impact:  The kernel is now "exception-safe" — it can handle:
         • Page faults  (data abort, instruction abort)
         • IRQs         (timer, GIC, device interrupts)
         • SError       (SMMU faults, bus errors, ECC errors)
         • SVC          (system calls from user space — not relevant yet)
         • Debug        (breakpoints, watchpoints, BRK instructions)
```
