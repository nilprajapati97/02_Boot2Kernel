# Identity Map Continuity and the PA=VA Window

**Context:** Why the kernel must maintain a PA=VA mapping across the MMU enable boundary  
**Critical Invariant:** Instructions at physical address X must also be at virtual address X

---

## 0. The Fundamental Problem

After `msr sctlr_el1` enables the MMU, the CPU uses the program counter (PC)
as a **virtual address** to fetch the next instruction.

The instruction after `msr sctlr_el1` is `isb`. At the moment the MMU becomes
active, the CPU's PC holds a value like `0x4020_1ABC`. With the MMU on, this
is now a **VA**. The CPU looks it up in TTBR0 → identity map.

If the identity map does NOT contain a mapping for `0x4020_1ABC`:
```
Translation Fault, Level 0
Fault Address Register = 0x4020_1ABC
Fault Status Code = 0b100100 (Translation Fault, Level 0)
Exception taken to EL1
ELR_EL1 = 0x4020_1ABC
```

The kernel crashes before executing a single instruction in virtual space.

---

## 1. What the Identity Map Contains

The identity map (`__pi_init_idmap_pg_dir`) maps:
1. The `.idmap.text` section — all code that runs straddling the MMU-enable boundary
2. The kernel's page tables (so they are accessible to the PTW by both PA and VA)

The `.idmap.text` section is explicitly placed by the linker:

```ld
// arch/arm64/kernel/vmlinux.lds.S
.idmap.text : {
    KEEP(*(.idmap.text))
}
```

Code annotated with `__idmap_text` section attribute is placed here:

```c
// arch/arm64/kernel/head.S
SYM_CODE_START(__enable_mmu)
    ...
SYM_CODE_END(__enable_mmu)
// __enable_mmu is in .idmap.text (or linked adjacent to it)
```

Actually in head.S, `__enable_mmu` is within the main `.text` section of
`head.S`, which is the `.head.text` section (very first section in the binary).
The identity map created by `__pi_create_init_idmap` maps the entire kernel
image, but using 2MB block entries, ensuring the PC range is covered.

---

## 2. The 2MB Block Mapping and Coverage Guarantee

`__pi_create_init_idmap` creates 2MB block entries covering the entire kernel
image. For a typical ARM64 kernel:

```
Kernel loaded at PA: 0x4000_0000
Kernel size:         ~10 MB

PMD entries for identity map:
  PA[0x4000_0000 – 0x41FF_FFFF]  →  VA[0x4000_0000 – 0x41FF_FFFF]  (2MB block 1)
  PA[0x4200_0000 – 0x43FF_FFFF]  →  VA[0x4200_0000 – 0x43FF_FFFF]  (2MB block 2)
  PA[0x4400_0000 – 0x45FF_FFFF]  →  VA[0x4400_0000 – 0x45FF_FFFF]  (2MB block 3)
  PA[0x4600_0000 – 0x47FF_FFFF]  →  VA[0x4600_0000 – 0x47FF_FFFF]  (2MB block 4)
  PA[0x4800_0000 – 0x49FF_FFFF]  →  VA[0x4800_0000 – 0x49FF_FFFF]  (2MB block 5)
```

Since `__enable_mmu` is somewhere in the kernel image, it is covered by one
of these 2MB blocks. The PA of `__enable_mmu` maps to the same VA. ✓

---

## 3. The Continuity Window

The identity map needs to be valid during a specific **window of execution**:

```
Window start:  The instruction AFTER "msr sctlr_el1" (ISB, then ret)
Window end:    "br x8" in __primary_switch jumps to __primary_switched VA
```

During this window:
- The CPU executes at identity-mapped VAs (VA = PA)
- `TTBR0_EL1` holds the identity map root
- `TTBR1_EL1` holds `reserved_pg_dir` (valid but empty — no kernel VA mappings yet)

After `br x8`:
- Execution jumps to `__primary_switched` at a kernel VA (0xFFFF_8000_...)
- This VA is in TTBR1's range → uses `swapper_pg_dir` (just built by `__pi_early_map_kernel`)
- TTBR0 identity map is no longer used for instruction fetches (PC in TTBR1 range)

TTBR0 identity map is eventually replaced when:

```c
// arch/arm64/mm/mmu.c — after start_kernel:
cpu_replace_ttbr1(lm_alias(swapper_pg_dir), init_idmap_pg_dir);
// This also clears the identity map from TTBR0 and switches to
// the init_task's mm (null_mm for the idle thread)
```

---

## 4. What `reserved_pg_dir` Does During the Window

During the identity map continuity window, TTBR1 holds `reserved_pg_dir`.

`reserved_pg_dir` is a single page (4KB) of zeros:

```c
// arch/arm64/mm/mmu.c
RESERVE_BRK_SIZE(reserved_pg_dir, PAGE_SIZE);
```

All entries are zero → all are INVALID descriptors.

**Does this matter?** During the window, the CPU executes at identity-mapped VAs
which are in the **TTBR0 range** (VA[63:48] = 0x0000 for 48-bit VA). Since
TTBR0 is used for VA[63:48]=0 (non-negative VAs), and TTBR1 is used for
VA[63:48]=0xFFFF (negative VAs), there is no conflict.

The CPU never tries to use TTBR1 for identity-map VAs because:
```
VA = 0x4020_1ABC = 0x0000_0000_4020_1ABC
VA[63:48] = 0x0000  →  TTBR0 selected  ✓
```

---

## 5. KASLR and Identity Map Correctness

With KASLR enabled, the kernel is loaded at a randomized physical address, e.g.:

```
PA_BASE = 0x4000_0000 + random_offset
```

The identity map is built at **runtime** by `__pi_create_init_idmap` based on
the actual load address:

```c
// arch/arm64/mm/pi/map_kernel.c
__pi_create_init_idmap(pa_base, ...);
// pa_base is the actual physical load address
```

This means the identity map always covers the actual load address, regardless
of KASLR randomization. There is no hardcoded address in the identity map.

---

## 6. What Breaks Without the Identity Map

Scenario: `__pi_create_init_idmap` has a bug and does NOT map `0x4020_1000`
(where `__enable_mmu` happens to be):

```
msr sctlr_el1, x0    ← MMU enabled at this PA
isb                  ← Pipeline flush, re-fetch at VA 0x4020_1001
                        PTW: TTBR0 = identity map
                             Walk PGD[0] → INVALID (bug: not mapped)
                             → Translation Fault, Level 0
                        CPU: takes SError or Synchronous Exception
                        ELR_EL1 = 0x4020_1001
                        ESR_EL1 = 0x9200_0000 (Translation Fault L0)
```

The exception handler itself is at a kernel VA (mapped via `vectors` in
`__primary_switched`). But `__primary_switched` hasn't been called yet.
Exception vector base (`VBAR_EL1`) was set in `__cpu_setup` to a VA. But
`swapper_pg_dir` (TTBR1) is `reserved_pg_dir` — also empty.

So the exception handler also faults. The CPU enters a double-fault or triple-
fault state and hangs with `daif = 0b1111` (interrupts masked), PC pointing
nowhere, machine is dead.

This confirms: **The identity map is a correctness invariant, not a performance
optimization.**

---

## 7. The Precise PA=VA Window Requirements

| Requirement | Reason |
|---|---|
| `__enable_mmu` code at PA X must have identity map entry for VA X | Instruction fetch after MMU enable |
| `ret` in `__enable_mmu` must reach caller via identity map | `ret` pops LR (a VA = PA in `.idmap.text`) |
| Caller (`__primary_switch` + `br x8`) must be in identity map | Final instruction fetches before VA jump |
| `swapper_pg_dir` PA must also be identity-mapped | The hardware PTW for TTBR1 needs to read these entries by PA |
| `__primary_switched` VA does NOT need identity map | It is reached via `br x8` (TTBR1 range VA) |

---

## 8. Summary: Three Moments of PA=VA Dependency

```
Moment 1: isb after msr sctlr_el1
    PC = 0x4020_xxxx (PA-as-VA)
    TTBR0 → identity map must cover this PA

Moment 2: ret in __enable_mmu
    PC = PA of __primary_switch code (link register value)
    TTBR0 → identity map must cover this PA

Moment 3: br x8 in __primary_switch
    x8 = KERNEL_START (VA: 0xFFFF_8000_...)
    PC jumps OUT of identity map range
    First instruction at TTBR1 VA — now uses swapper_pg_dir
    Identity map no longer needed for instruction fetch
    (Still needed by PTW for page table reads briefly)
```

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