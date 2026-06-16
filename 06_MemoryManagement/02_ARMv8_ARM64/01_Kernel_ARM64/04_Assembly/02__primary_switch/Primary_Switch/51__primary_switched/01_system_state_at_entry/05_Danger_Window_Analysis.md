# Danger Window Analysis — What Can Kill the Kernel Before It Starts

## The Three Danger Windows in `__primary_switched`

### Window 1: Entry → `msr vbar_el1` (CRITICAL — Any Exception = HANG)

**Duration:** ~12 instructions (init_cpu_task macro expansion)

**Risk level:** CRITICAL

**What can go wrong:**
Any exception — including DATA ABORT from speculative prefetch, SError from memory
subsystem, or Debug exception — will dispatch to `VBAR_EL1 + offset` where VBAR_EL1 = 0.

```
Exception fires during init_cpu_task
    │
    ▼
CPU: handler = VBAR_EL1 + offset = 0x0 + 0x200 = 0x200
    │
    ▼
TLB lookup for VA 0x200:
  TTBR1 covers 0xFFFF_8000_... and above → 0x200 NOT covered
  TTBR0 identity map covers low PA range
  PA 0x200 → may be ROM, may be unmapped
    │
    ▼
If PA 0x200 is unmapped → translation fault from within exception handler
    │
    ▼
Recursive exception → same VBAR → same fault → CPU spin → HANG
No UART, no serial, no output. Silent death.
```

**Mitigations in place:**
1. `DAIF.I=1` (IRQs masked) — set by ERET from `init_kernel_el` to EL1. Eliminates
   timer interrupts and peripheral IRQs. Does NOT mask SError, Debug, data aborts.
2. Speculative prefetch suppression: caches are on, TLBs are populated. Speculative
   fetches to the kernel `.text` region (mapped TTBR1) succeed silently.
3. Short window: the entire `init_cpu_task` macro is ~12 instructions. At 3GHz+,
   this window is ~4 nanoseconds.

---

### Window 2: `msr vbar_el1` → `isb` (SHORT — Exception Uses Old VBAR)

**Duration:** 1 instruction gap

**Risk level:** LOW but architecturally documented

**The problem:**
ARM64 architecture does NOT guarantee that `msr` to a system register is immediately
visible to the instruction pipeline. An out-of-order processor may have already
fetched and decoded the next instruction(s) before the `msr` completes.

More critically: an exception taken BETWEEN the `msr` and the `isb` may still use
the OLD VBAR value (pre-write) for the exception handler address calculation.

**The `isb` fix:**
`ISB` (Instruction Synchronization Barrier) is a context synchronization event.
The ARMv8 architecture guarantees:
> "All changes to VBAR_EL1 take effect at the next context synchronization event."

`ISB` is the lightest context synchronization event. It flushes the instruction pipeline
and ensures the new VBAR is in effect for all instructions that execute AFTER the ISB.

**Without ISB:** Architecturally permitted race condition. VBAR update is non-deterministic.

---

### Window 3: After `br x8` → Before `kimage_voffset` Stored (VIRT↔PHYS BROKEN)

**Duration:** ~8 instructions

**Risk level:** MEDIUM

**What can go wrong:**
Any code that calls `phys_to_virt()` or `virt_to_phys()` during this window gets:
```c
__phys_to_virt(pa) = pa + kimage_voffset
                   = pa + 0        // kimage_voffset is in .bss = zero-initialized
                   = pa            // returns physical address as if it were virtual!
```

**This window is safe because:**
- No C code runs during this window (still in assembly)
- The only operations are: `init_cpu_task`, `msr vbar_el1`, `isb`, `stp`, `mov x29`
- None of these call `phys_to_virt()` or `virt_to_phys()`
- `str_l __fdt_pointer` stores a physical address — no conversion needed

**`str_l kimage_voffset` closes this window.**

---

### Window 4: After `kimage_voffset` → Before `kasan_early_init` (KASAN TRAP)

**Duration:** ~4 instructions (`set_cpu_boot_mode_flag` call)

**Risk level:** MEDIUM (configuration-dependent)

**Only relevant when:** `CONFIG_KASAN_GENERIC` or `CONFIG_KASAN_SW_TAGS` is set.

**What can go wrong:**
`set_cpu_boot_mode_flag` is an assembly function that does NOT call any KASAN-instrumented
C code. It is safe in this window.

BUT: if the `bl` triggers a branch prediction miss and the branch predictor speculatively
executes into KASAN-instrumented code... actually no — speculative execution cannot
cause architectural side effects (page faults from speculative accesses are suppressed).

The real risk: if `set_cpu_boot_mode_flag` were replaced with any C function call,
the C function's first instrumented memory access would fault.

**This window is intentionally designed to contain only KASAN-clean assembly.**

---

### Window 5: After `finalise_el2` ERET → Before Kernel Properly Starts

**Duration:** After `ldp` restores frame, before `bl start_kernel` and inside it

**Risk level:** LOW after all windows are closed

After `finalise_el2`:
- VBAR_EL1 = `&vectors` ✓
- SP = valid `init_stack` ✓
- `kimage_voffset` = computed ✓
- KASAN shadow = mapped (if enabled) ✓
- Per-CPU = initialized ✓

Any exception from this point is safely handled by the full exception vector.

---

## Danger Window Timeline

```
ENTRY TO __primary_switched
│
├─[DANGER: VBAR=0]─────────────────────────────────────────────────┐
│  init_cpu_task (12 instructions)                                  │
│  ANY EXCEPTION HERE = SILENT HANG                                 │
└───────────────────────────────────────────────────────────────────┘
│  msr vbar_el1, x8
├─[DANGER: VBAR WRITE NOT RETIRED]─────────────────────────────────┐
│  (1 instruction gap)                                              │
└───────────────────────────────────────────────────────────────────┘
│  isb   ← VBAR now safe
│  stp x29,x30,[sp,#-16]!
│  mov x29, sp
├─[SAFE: virt↔phys broken but no phys_to_virt called]──────────────┐
│  str_l x21, __fdt_pointer, x5                                     │
│  adrp x4, _text                                                   │
│  sub x4, x4, x0                                                   │
└───────────────────────────────────────────────────────────────────┘
│  str_l x4, kimage_voffset, x5   ← kimage_voffset now valid
├─[SAFE: KASAN shadow unmapped but no KASAN-instrumented code runs]─┐
│  mov x0, x20                                                       │
│  bl set_cpu_boot_mode_flag  (assembly, no KASAN instrumentation)   │
└───────────────────────────────────────────────────────────────────┘
│  bl kasan_early_init   ← KASAN shadow now mapped (if configured)
│  mov x0, x20
│  bl finalise_el2       ← EL finalized
│  ldp x29, x30, [sp], #16
│
└──► bl start_kernel     ← ALL WINDOWS CLOSED. SAFE.
```

---

## Conclusion: Why the Danger Windows Don't Actually Kill the Kernel

The windows exist but are managed through:
1. **Short duration** — ARM64 pipeline at 3GHz = windows are nanoseconds
2. **IRQ masking** — most hardware interrupts are masked during boot
3. **Careful instruction selection** — only assembly in dangerous windows, no C calls
4. **ARM cache/TLB pre-population** — speculative prefetches to mapped regions succeed
5. **Hardware speculation suppression** — translation faults from speculative accesses
   are suppressed (not reported as precise exceptions) until the instruction actually retires

This is not accident — it is deliberate careful design by the ARM64 kernel team.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The "danger window" or "hazard window" in the MMU-enable sequence is the period between writing SCTLR_EL1.M=1 and the completion of the first ISB. During this window, the ARMv8-A microarchitecture has undefined behavior for instruction fetches and data accesses that were already in flight from the pre-MMU era. The ISB is the architectural guarantee that all such in-flight operations complete using the old settings, and all operations after the ISB use the new settings. There is NO architecturally defined "mixed" or "partial" MMU behavior.

### Kernel Perspective (Linux ARM64)
The kernel minimizes the danger window by:
1. Ensuring the identity map covers the exact physical pages of the __enable_mmu code.
2. Using set_sctlr_el1 which immediately follows the MSR with an ISB.
3. Not placing any data loads or stores between the MSR and ISB.
4. Returning via RET to the identity-mapped continuation address after the ISB.
The danger window is approximately 1 instruction wide (the MSR write) + whatever the microarchitecture's pipeline flush latency is.

### Memory Perspective (ARMv8 Memory Model)
From the ARMv8 formal memory model perspective, the MSR SCTLR_EL1 write is a system register write (non-memory operation). The ISB that follows it is defined as a Context Synchronization Event (CSE). Per the ARM ARM (A1.8.2), a CSE: "ensures that all effects of any instructions preceding the CSE are visible before any instruction following the CSE is executed." This formally defines the end of the danger window. Before the ISB: the memory system state is transitioning. After the ISB: the memory system is fully configured with the new SCTLR, TTBR, TCR, and MAIR values.