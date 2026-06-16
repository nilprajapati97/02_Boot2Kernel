# CPU Hardware State at Entry to `__primary_switch`

**Source:** `arch/arm64/kernel/head.S`  
**Predecessor:** `bl __cpu_setup` → `b __primary_switch`  
**MMU State when file begins:** OFF  
**D-Cache:** OFF  
**I-Cache:** ON or OFF (don't care at this stage)

---

## 0. Why Hardware State Matters Here

`__primary_switch` is not a normal function call. It is entered via a bare
**`b` branch** (not `bl`), meaning the link register `x30` is NOT updated.
The CPU arrives here having traversed `primary_entry` → `record_mmu_state` →
`preserve_boot_args` → `__pi_create_init_idmap` → `init_kernel_el` →
`__cpu_setup`. Every one of those functions has made deliberate choices about
CPU state, and `__primary_switch` inherits the cumulative result.

This document enumerates exactly what the hardware registers look like at the
first instruction of `__primary_switch`, from the CPU's perspective.

---

## 1. PSTATE — The Process State Register

`PSTATE` is not a single physical register. It is the logical union of fields
that the CPU maintains internally and exposes through special-purpose registers.
At EL1 it manifests as `SPSR_EL1` (when taking exceptions) and individual MSRs.

### 1.1 Exception Level Field — `M[4:0]` and `nRW`

At entry to `__primary_switch`, the CPU is running at **EL1** in **AArch64
state**.

```
PSTATE.M[4:0]  = 0b00101   (EL1h — using SP_EL1)
PSTATE.nRW     = 0          (AArch64 mode, not AArch32)
```

`init_kernel_el` arranges the EL transition via `eret` so that execution
resumes at EL1. The `h` suffix means `SPSel = 1` — the stack pointer is
`SP_EL1`, not the shared `SP_EL0`. This matters immediately when `__primary_switch`
sets up a new stack: any `str`/`ldr` using `sp` touches `SP_EL1`.

**Bit encoding for reference:**

| `M[4:0]` | Meaning               |
|----------|-----------------------|
| `0b00100` | EL1t (uses SP_EL0)   |
| `0b00101` | EL1h (uses SP_EL1) ← |
| `0b01000` | EL2t                 |
| `0b01001` | EL2h                 |

---

### 1.2 Interrupt Masking — `DAIF`

```
PSTATE.D = 1   // Debug exceptions masked
PSTATE.A = 1   // SError (Asynchronous abort) masked
PSTATE.I = 1   // IRQ masked
PSTATE.F = 1   // FIQ masked
```

All four interrupt types are masked (`DAIF = 0b1111`). This was established by
`init_kernel_el` when it wrote `INIT_PSTATE_EL1` into `SPSR_EL1` before the
`eret` that dropped to EL1:

```c
// arch/arm64/include/asm/ptrace.h
#define PSR_D_BIT   0x00000200
#define PSR_A_BIT   0x00000100
#define PSR_I_BIT   0x00000080
#define PSR_F_BIT   0x00000040
#define INIT_PSTATE_EL1 (PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT | PSR_MODE_EL1h)
```

**Why all interrupts masked?**  
The IRQ and FIQ handlers (`VBAR_EL1`) are not yet set up. `VBAR_EL1` is
configured in `__primary_switched` (the first instruction after the virtual
jump). If an interrupt were to fire now, the CPU would vector to an undefined
or boot-ROM address. Masking prevents this.

**Critical implication for `__primary_switch`:**  
Between `bl __enable_mmu` returning and `mov sp, x1` establishing the correct
virtual stack, the CPU is in a window with the wrong SP but cannot be interrupted.
This is not an accident — it is the only correct design.

---

### 1.3 Condition Flags — `NZCV`

```
PSTATE.N = ?   // Not meaningful (result of last comparison in __cpu_setup)
PSTATE.Z = ?
PSTATE.C = ?
PSTATE.V = ?
```

The condition flags carry over from `__cpu_setup`. `__primary_switch` does not
inspect them and the first instruction (`adrp x1, reserved_pg_dir`) does not
set them. They are irrelevant.

---

### 1.4 Stack Pointer Select — `SPSel`

```
PSTATE.SPSel = 1    // Using SP_EL1 (not SP_EL0)
```

Set by `INIT_PSTATE_EL1`. The SP will be overwritten inside `__primary_switch`
to point at `early_init_stack`. Until then, `SP_EL1` still holds the value
established in `primary_entry`:

```asm
// From primary_entry:
adrp    x1, early_init_stack
mov     sp, x1
```

So at the moment `__primary_switch` begins, `SP_EL1` = physical address of
`early_init_stack`. This is still valid because the identity map (TTBR0) covers
it — but it is a *physical* address acting as a virtual address in a PA=VA world.

---

## 2. System Registers Written by `__cpu_setup`

`__cpu_setup` in `arch/arm64/mm/proc.S` is the last callee before
`__primary_switch`. It leaves the following system registers fully configured:

### 2.1 `MAIR_EL1` — Memory Attribute Indirection Register

```asm
// From proc.S:
mov_q   x5, MAIR_EL1_SET
msr     mair_el1, x5
```

`MAIR_EL1` holds 8 × 8-bit memory attribute encodings indexed by the
`AttrIdx[2:0]` field in page table descriptors. At boot they are set to:

| Index | Attribute Name         | Encoding | Meaning                                              |
|-------|------------------------|----------|------------------------------------------------------|
| 0     | `MT_DEVICE_nGnRnE`     | `0x00`   | Device, non-Gathering, non-Reordering, no Early Write Ack |
| 1     | `MT_DEVICE_nGnRE`      | `0x04`   | Device, non-Gathering, non-Reordering, Early Write Ack |
| 2     | `MT_DEVICE_GRE`        | `0x0c`   | Device, Gathering, Reordering, Early Write Ack       |
| 3     | `MT_NORMAL_NC`         | `0x44`   | Normal, Non-Cacheable                                |
| 4     | `MT_NORMAL`            | `0xff`   | Normal, Write-Back Cacheable, Inner+Outer             |
| 5     | `MT_NORMAL_TAGGED`     | `0xf0`   | Normal, Write-Back, Tagged (for MTE)                 |
| 6     | *(unused)*             | `0x00`   | —                                                    |
| 7     | *(unused)*             | `0x00`   | —                                                    |

These encodings are referenced by every page table entry that will be written
by `__pi_early_map_kernel`. The MMU cannot function correctly without `MAIR_EL1`
being programmed *before* it is enabled — which is why `__cpu_setup` does it.

---

### 2.2 `TCR_EL1` — Translation Control Register

`TCR_EL1` was written by `__cpu_setup`. Its key fields at this moment:

| Field    | Value     | Meaning                                                    |
|----------|-----------|------------------------------------------------------------|
| `T0SZ`   | `64 - VA_BITS` | VA bits for TTBR0 (e.g., `16` for 48-bit VA)         |
| `T1SZ`   | `64 - VA_BITS` | VA bits for TTBR1                                     |
| `TG0`    | `0b00`    | 4KB granule for TTBR0                                      |
| `TG1`    | `0b10`    | 4KB granule for TTBR1                                      |
| `IRGN0`  | `0b01`    | Inner Write-Back RW-Allocate cacheable (TTBR0)             |
| `ORGN0`  | `0b01`    | Outer Write-Back RW-Allocate cacheable (TTBR0)             |
| `IRGN1`  | `0b01`    | Inner Write-Back RW-Allocate cacheable (TTBR1)             |
| `ORGN1`  | `0b01`    | Outer Write-Back RW-Allocate cacheable (TTBR1)             |
| `SH0`    | `0b11`    | Inner Shareable (TTBR0)                                    |
| `SH1`    | `0b11`    | Inner Shareable (TTBR1)                                    |
| `IPS`    | CPU-dependent | Intermediate Physical Address Size (typically 40 or 48 bits) |
| `AS`     | `1`       | 16-bit ASID                                                |
| `EPD0`   | `0`       | TTBR0 walk enabled                                         |
| `EPD1`   | `0`       | TTBR1 walk enabled                                         |

`TCR_EL1.IPS` is determined by reading `ID_AA64MMFR0_EL1.PARange` in
`__cpu_setup` and encoding the largest supported PA size. This prevents the
MMU from being asked to translate PA bits the CPU cannot actually address.

---

### 2.3 `SCTLR_EL1` — System Control Register (Pre-MMU state)

At entry to `__primary_switch`, `SCTLR_EL1` contains `INIT_SCTLR_EL1_MMU_OFF`
(set by `init_kernel_el`):

```c
// arch/arm64/include/asm/sysreg.h
#define INIT_SCTLR_EL1_MMU_OFF \
    (ENDIAN_SET_EL1 | SCTLR_EL1_LSMAOE | SCTLR_EL1_nTLSMD | \
     SCTLR_EL1_EIS  | SCTLR_EL1_TSCXT  | SCTLR_EL1_EOS)
```

Key fields that are **OFF** at this point:
- `M` (bit 0) = **0** — MMU disabled  
- `C` (bit 2) = **0** — D-cache disabled  
- `I` (bit 12) = **0** — I-cache disabled (or on — `b __primary_switch` doesn't care)

`__cpu_setup` returns the *ready-to-use* `INIT_SCTLR_EL1_MMU_ON` value in
`x0` — it does NOT write it to `SCTLR_EL1` yet. The actual write happens
inside `__enable_mmu` via `set_sctlr_el1 x0`.

---

## 3. Physical Memory Bus State

The CPU issues all memory transactions as physical addresses at this point.

### 3.1 Instruction Fetch Bus Transactions

```
Current PC ≈ PA of __primary_switch in .idmap.text section
Bus:  PA → L1 I-cache lookup → (miss) → L2 → DRAM
```

Each 4-byte instruction is fetched using a physical address. There is no TLB
lookup, no page table walk, no virtual-to-physical translation. The address
presented to the cache/bus fabric is the raw physical RAM address where the
kernel image was loaded by the bootloader.

### 3.2 Data Access Bus Transactions (e.g., `adrp` result use)

`adrp` itself does no memory access. But when the result in `x1` is written to
`TTBR1_EL1` inside `__enable_mmu`, the write goes through the system register
write port — no memory bus transaction at all (system registers are accessed
via `msr`, not via the load-store unit).

### 3.3 Cache State

| Cache     | State at Entry |
|-----------|----------------|
| L1 I-cache | ON or OFF (bootloader choice) |
| L1 D-cache | **OFF** — `SCTLR_EL1.C = 0` |
| L2 cache  | depends on CPU — typically OFF for data |

With D-cache off, every data store goes directly to DRAM. This was essential
earlier when `preserve_boot_args` wrote `boot_args[]` — those writes needed
to be visible to all agents (e.g., secondary CPUs reading them with D-cache on).

With D-cache off during `__pi_create_init_idmap`'s page table writes, the
`dmb sy` + `dcache_inval_poc` sequence in `primary_entry` was needed to ensure
the hardware page table walker sees the freshly written entries in DRAM, not
in any stale cached state.

---

## 4. TLB State

```
All TLB entries:   UNPREDICTABLE / empty
ASID 0:            In use (set by __cpu_setup: TTBR0/TTBR1 ASID field = 0)
```

`__cpu_setup` issues `tlbi vmalle1` to invalidate all EL1 TLB entries before
returning. This guarantees no stale translations from a previous boot attempt
(e.g., kexec) interfere when the MMU is turned on.

---

## 5. The `x0` Handoff — Most Important Register

```
x0 = INIT_SCTLR_EL1_MMU_ON   (returned by __cpu_setup in proc.S)
```

This is the pre-computed `SCTLR_EL1` value that, when written, enables:
- MMU (`M = 1`)
- D-cache (`C = 1`)
- I-cache (`I = 1`)
- Stack alignment check (`SA = 1`, `SA0 = 1`)
- And ~10 other control bits

`__primary_switch` must pass `x0` unmodified into `__enable_mmu` as its first
argument. The three instructions before `bl __enable_mmu` only touch `x1` and
`x2`, carefully leaving `x0` intact.

---

## 6. Summary Table — Hardware State at `__primary_switch` Entry

| Component | State | Detail |
|---|---|---|
| Exception Level | EL1h | SPSel=1, using SP_EL1 |
| MMU | **OFF** | SCTLR_EL1.M = 0 |
| D-Cache | **OFF** | SCTLR_EL1.C = 0 |
| I-Cache | Don't care | SCTLR_EL1.I = 0 or 1 |
| IRQ/FIQ/SError/Debug | **Masked** | DAIF = 0b1111 |
| MAIR_EL1 | Programmed | 8 attribute slots configured |
| TCR_EL1 | Programmed | T0SZ, T1SZ, TG0, TG1, IPS set |
| SCTLR_EL1 | MMU_OFF value | M=0, C=0, I=0 |
| x0 | INIT_SCTLR_EL1_MMU_ON | Ready to flip MMU on |
| x20 | BOOT_CPU_MODE_EL1/EL2 | CPU boot EL |
| x21 | FDT physical address | Bootloader-provided |
| TLBs | Invalidated | `tlbi vmalle1` done in __cpu_setup |
| SP_EL1 | early_init_stack (PA) | Temporary, will be reset |

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
PSTATE (Process State) in ARMv8-A is a collection of condition flags and control bits maintained internally by the CPU. Key fields:
- NZCV (bits 31:28): condition flags (Negative, Zero, Carry, oVerflow) set by ALU instructions.
- DAIF (bits 9:6): interrupt mask bits (Debug, SError, IRQ, FIQ). All set at EL1 on exception entry.
- EL (bits 3:2): current exception level (00=EL0, 01=EL1, 10=EL2, 11=EL3).
- SP (bit 0): stack pointer selection (0=SP_EL0, 1=SP_ELx).
- PAN (bit 22): Privileged Access Never (ARMv8.1).
- UAO (bit 23): User Access Override (ARMv8.2).
- SSBS (bit 12): Speculative Store Bypass Safe (ARMv8.5).
PSTATE is not a memory-mapped register; it is read/written via PSTATE pseudo-register in the assembler.

### Kernel Perspective (Linux ARM64)
Linux uses PSTATE extensively:
- DAIF.I=1 (IRQ masked) during critical sections to prevent preemption.
- DAIF.A=0 (SError unmasked) to allow SError exceptions in the kernel.
- EL field is checked at boot to determine if the kernel entered at EL1 or EL2.
- PAN is set on exception entry (SCTLR_EL1.SPAN=1) to protect against accidental kernel access to user memory.
local_irq_disable() / local_irq_enable() modify PSTATE.DAIF.I via MSR DAIFSET/DAIFCLR.

### Memory Perspective (ARMv8 Memory Model)
PSTATE affects memory access behavior in two ways: (1) PAN (PSTATE.PAN=1) prevents the kernel (EL1) from directly accessing user-space mapped memory -- any such access raises a Permission Fault, protecting the kernel from Meltdown-style attacks. (2) DAIF masking controls whether asynchronous memory errors (SError) are delivered immediately or held pending. From a memory ordering standpoint, MSR DAIFSET/DAIFCLR are context-synchronizing events that drain the pipeline before the new interrupt masking takes effect.