# The `isb` Instruction — Why It's Mandatory After `msr vbar_el1`

## What `isb` Does

`ISB` = Instruction Synchronization Barrier. It:
1. Flushes the **instruction pipeline** on the executing PE (Processing Element)
2. Ensures all preceding instructions are complete before the `isb` retires
3. Causes subsequent instructions to be fetched fresh using the new system state

`isb` is the STRONGEST instruction barrier on ARM64. It is more expensive than
`dmb` (data memory barrier) because it affects instruction fetch, not just data.

---

## Why System Register Writes Need `isb`

ARM64 allows speculative execution and out-of-order instruction fetch. After writing
a system register like `VBAR_EL1`:

**Without `isb`:**
```
msr     vbar_el1, x8    // write VBAR: takes effect at some future point
                        // CPU may already have fetched next 10 instructions
                        // if exception occurs HERE → uses OLD VBAR_EL1!
                        // UNPREDICTABLE behavior
stp     x29, x30, ...   // already in the pipeline
```

**With `isb`:**
```
msr     vbar_el1, x8    // write VBAR
isb                     // flush pipeline: VBAR_EL1 write is now COMMITTED
                        // all subsequent instructions see new VBAR_EL1
stp     x29, x30, ...   // fetched AFTER isb; exception handling is safe
```

The ARM Architecture Reference Manual (ARMv8-A) explicitly states:
> "An ISB instruction is required for the effect of a write to a System register to
> be visible to subsequent instruction fetches on the same PE."

---

## The `isb` Latency — Is It Slow?

On ARM Cortex-A78:
- `isb` serializes the pipeline
- Typical latency: 10-20 cycles (pipeline drain + refetch)
- Cost is ONE-TIME at boot — completely irrelevant for performance

Compare: `dmb sy` (data memory barrier):
- Waits for all outstanding memory operations to complete
- Latency: varies by system, can be 100s of cycles

`isb` after `msr vbar_el1` is cheap (pipeline drain only, no memory traffic)
and completely correct. No optimization is possible or needed here.

---

## When `isb` Is Required (ARM Architecture Rules)

ARM Architecture requires `isb` after writing to:

| Register | Reason |
|---|---|
| `VBAR_EL1` | Changes exception vector base |
| `SCTLR_EL1` | Changes MMU/cache enable bits |
| `TTBR0_EL1/TTBR1_EL1` | Changes page table base |
| `TCR_EL1` | Changes translation control |
| `MAIR_EL1` | Changes memory attribute indirection |
| `CPACR_EL1` | Changes FP/SVE/trap configuration |
| `HCR_EL2` | Changes hypervisor configuration |
| `CONTEXTIDR_EL1` | Changes ASID/context |

In `__primary_switched`, there is one `isb` — after `msr vbar_el1, x8`. This is
the minimum required. No other system registers are written in `__primary_switched`
that require `isb` (the `init_cpu_task` macros writing `sp_el0` and `tpidr_el1`
don't require `isb` because their effects are visible to loads/stores, not to
instruction fetch).

---

## What Happens on the Other Side — The Exception Entry Code

When any exception occurs after the `isb`, the CPU vector dispatch correctly
jumps to `vectors + offset`. The first thing the exception handler does:

```asm
// arch/arm64/kernel/entry.S kernel_entry macro (simplified):
sub     sp, sp, #PT_REGS_SIZE      // make room for pt_regs
stp     x0, x1, [sp, #16 * 0]     // save x0, x1
stp     x2, x3, [sp, #16 * 1]     // save x2, x3
...
mrs     x22, elr_el1               // saved PC (where exception was taken)
mrs     x23, spsr_el1              // saved PSTATE
...
```

For this exception save to work correctly:
- `sp` must be valid (set by `init_cpu_task`) ✓
- `sp_el0` must be valid for `current` access ✓
- `tpidr_el1` must be valid for per-CPU access ✓
- `vbar_el1` must point to `vectors` ✓ (the `isb` ensures this)

All prerequisites are satisfied. The ordering in `__primary_switched` is not
arbitrary — it's precisely the minimum sequence for a safe handoff.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The ARMv8-A CPU pipeline is a deeply pipelined, out-of-order superscalar design (e.g., Cortex-A57 has 15+ pipeline stages). At the moment SCTLR_EL1.M is set and the ISB executes, the pipeline is flushed: all instructions in the fetch, decode, and issue queues are discarded. The ISB acts as a serialization point. After the ISB, the first instruction fetch goes through the now-active MMU using TTBR0_EL1 (identity map). The branch predictor BTB (Branch Target Buffer) and the prefetcher are also indirectly reset because the VA space has changed semantics.

### Kernel Perspective (Linux ARM64)
The set_sctlr_el1 macro in arch/arm64/include/asm/assembler.h wraps the MSR and ISB sequence. The ISB is mandatory: without it, the processor is allowed to complete instructions speculatively fetched before the SCTLR write, potentially fetching from the wrong VA space. After the ISB, the RET instruction causes a jump to the return address (still in the identity-mapped region because x30 was set before __enable_mmu). The very next BL or indirect branch takes the CPU to the kernel VA.

### Memory Perspective (ARMv8 Memory Model)
From the memory model perspective, the ISB after setting SCTLR_EL1.M is a context synchronization event (CSE). The ARM ARM (D1.7) defines that a CSE causes all pending system register effects (including SCTLR_EL1, TTBR0_EL1, TTBR1_EL1, TCR_EL1, MAIR_EL1) to be committed before any subsequent instruction can issue. This means: after the ISB, the MMU is guaranteed to be using the latest values of all translation-related registers for every subsequent memory access, with no possibility of using pre-switch values.