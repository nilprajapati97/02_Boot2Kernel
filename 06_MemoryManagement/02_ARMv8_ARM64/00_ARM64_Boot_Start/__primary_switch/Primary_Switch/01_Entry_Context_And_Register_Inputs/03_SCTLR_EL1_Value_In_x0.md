# `INIT_SCTLR_EL1_MMU_ON` — The Value in `x0` When `__primary_switch` Runs

**Source:** `arch/arm64/include/asm/sysreg.h` line 863  
**Produced by:** `__cpu_setup` in `arch/arm64/mm/proc.S`  
**Lives in:** Register `x0` for the entire duration of `__primary_switch` until `bl __enable_mmu`

---

## 0. Why This Value Is Pre-Computed

Enabling the MMU is an atomic, point-of-no-return operation. The instant
`SCTLR_EL1.M` is set, every subsequent instruction fetch and data access goes
through the TLB. If the wrong SCTLR value is written — for example, caches
disabled, or endianness wrong — the kernel will malfunction immediately with
no easy recovery.

The Linux kernel therefore **pre-computes** the exact SCTLR value in `__cpu_setup`
and returns it in `x0`. `__primary_switch` and `__enable_mmu` act only as
delivery vehicles. No bit manipulation of SCTLR happens at the critical moment —
the value is simply written as-is.

---

## 1. The Macro Definition

```c
// arch/arm64/include/asm/sysreg.h

#define INIT_SCTLR_EL1_MMU_ON \
    (SCTLR_ELx_M      | SCTLR_ELx_C      | SCTLR_ELx_SA    | \
     SCTLR_EL1_SA0    | SCTLR_EL1_SED    | SCTLR_ELx_I     | \
     SCTLR_EL1_DZE    | SCTLR_EL1_UCT    | SCTLR_EL1_nTWE  | \
     SCTLR_ELx_IESB   | SCTLR_EL1_SPAN   | SCTLR_ELx_ITFSB | \
     ENDIAN_SET_EL1   | SCTLR_EL1_UCI    | SCTLR_EL1_EPAN  | \
     SCTLR_EL1_LSMAOE | SCTLR_EL1_nTLSMD | SCTLR_EL1_EIS   | \
     SCTLR_EL1_TSCXT  | SCTLR_EL1_EOS)
```

---

## 2. Bit-by-Bit Analysis

### SCTLR_EL1 Register Layout (64-bit, significant bits shown)

```
Bit  63    .... 30  29  28  27  26  25  24  23  22  21  20  19  18  17  16
     [RES] .... EOS NTLSMD LSMAOE UCI  EE  E0E SPAN [R] TSCXT [R] nTWE  [R]
                                                          ↑                 ↑
                                                       SCTLR_EL1_SPAN    SCTLR_EL1_nTWE

Bit  15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
     ITFSB IESB  [R]  I  [R] EPAN UCT DZE  [R] [R] SED SA0  SA  C [R]  M
                      ↑                ↑    ↑        ↑   ↑   ↑   ↑     ↑
                      I-cache        EPAN  DZE      SA0  SA  C       M (MMU)
```

---

### 2.1 `SCTLR_ELx_M` — Bit 0 — **MMU Enable** ★ Critical

```
Bit 0 = 1  → MMU ENABLED
```

This is **the** bit. Setting it makes the CPU route all memory accesses through
the TLB and page table walker. Before this bit is set, VA == PA. After this
bit is set, VA is translated using TTBR0_EL1 (low VA) or TTBR1_EL1 (high VA).

The ARM Architecture Reference Manual (ARM ARM) calls this `M` in the
`SCTLR_EL1` definition under section D17.2.118.

**What happens the instruction AFTER this bit is written:**

```asm
set_sctlr_el1 x0     // writes SCTLR_EL1, includes ISB
ret                   // fetches next instruction from VA — goes through TTBR0 identity map
```

The identity map (TTBR0) ensures VA == PA for the `.idmap.text` region, so
the `ret` succeeds without a fault.

---

### 2.2 `SCTLR_ELx_C` — Bit 2 — **Data Cache Enable**

```
Bit 2 = 1  → D-cache ENABLED for EL1 and EL0 data accesses
```

Enables the L1 D-cache. Without this bit, all data loads/stores go directly
to the memory system (L2 cache or DRAM). Enabling it simultaneously with the
MMU is correct because:

1. The page table descriptors that `__pi_early_map_kernel` will write include
   `AttrIdx` fields pointing to `MT_NORMAL` (inner write-back cacheable).
2. With the MMU and D-cache enabled together, the kernel immediately gets
   cache-coherent, write-back cached normal memory.

> **Note:** Even with `C=1`, if a cache line is not present, it is filled from
> memory on first access. The ARM ARM guarantees cache coherency is maintained
> from this point through hardware mechanisms.

---

### 2.3 `SCTLR_ELx_SA` — Bit 3 — **Stack Alignment Check Enable (EL1)**

```
Bit 3 = 1  → SP must be 16-byte aligned at all EL1 data accesses using SP
```

If `SP_EL1` is not 16-byte aligned when a load/store using SP is performed,
the CPU takes an alignment fault exception to the `VBAR_EL1` vector (SP
alignment fault). This catches stack misalignment bugs immediately.

The AAPCS64 ABI requires 16-byte alignment at function call boundaries, so
correct code will never trigger this. It is a safety net.

---

### 2.4 `SCTLR_EL1_SA0` — Bit 4 — **Stack Alignment Check Enable (EL0)**

Same as SA but for EL0 (user space) stack accesses. Enabled at boot so user
programs get the alignment check from the start.

---

### 2.5 `SCTLR_EL1_SED` — Bit 8 — **SETEND Instruction Disable**

```
Bit 8 = 1  → SETEND instruction is UNDEFINED (UDF fault)
```

`SETEND` is an AArch32 instruction that changes data endianness. In AArch64
mode it has no meaning. Disabling it prevents any accidental execution from
causing silent endianness changes.

---

### 2.6 `SCTLR_ELx_I` — Bit 12 — **Instruction Cache Enable**

```
Bit 12 = 1  → I-cache ENABLED
```

Enables the instruction cache for EL1 and EL0. After the MMU is on, I-cache
lookups use VAs (virtual addresses). Cache fills are done with the physical
address obtained from the TLB.

Without I-cache, every instruction fetch goes to L2/DRAM. On a modern ARM64
SoC, an instruction fetch to DRAM takes ~100 ns vs ~1 ns from L1 I-cache —
a 100× penalty. Enabling it here makes the early kernel boot approximately
100× faster for instruction execution.

---

### 2.7 `SCTLR_EL1_DZE` — Bit 14 — **DC ZVA Enable at EL0**

```
Bit 14 = 1  → `DC ZVA` instruction allowed at EL0
```

`DC ZVA` (Data Cache Zero by Virtual Address) zeroes a cache line without
reading it from memory. Used by `memset`-type operations in glibc and kernel.
Enabling at boot means user space can use it immediately.

---

### 2.8 `SCTLR_EL1_UCT` — Bit 15 — **EL0 Access to `CTR_EL0`**

```
Bit 15 = 1  → User space can read Cache Type Register (CTR_EL0)
```

`CTR_EL0` reports cache line sizes. libc uses it to implement efficient
`memcpy`, `memset`, and cache flush functions. Without this bit, accessing
`CTR_EL0` from EL0 traps to the kernel.

---

### 2.9 `SCTLR_EL1_nTWE` — Bit 18 — **No Trap WFE at EL0** (active-low naming)

```
Bit 18 = 1  → WFE at EL0 does NOT trap to EL1
```

`WFE` (Wait For Event) is used in spin-wait loops. Without this bit, every
`WFE` in user space traps to the kernel. Setting `nTWE=1` lets user space
use `WFE` directly, important for `futex`-based locking.

---

### 2.10 `SCTLR_ELx_IESB` — Bit 21 — **Implicit Error Synchronization Barrier**

```
Bit 21 = 1  → An implicit ESB is inserted at every synchronous exception entry
```

This ensures SError interrupts (asynchronous external aborts) are synchronized
at exception boundaries. Without this, an SError might be reported at the
wrong exception level. This bit was introduced in ARMv8.2 (FEAT_IESB).

---

### 2.11 `SCTLR_EL1_SPAN` — Bit 23 — **Set Privileged Access Never**

```
Bit 23 = 1  → PAN (Privileged Access Never) is SET at exception entry
```

When an exception is taken to EL1, `PSTATE.PAN` is automatically set to 1.
This prevents EL1 (kernel) code from directly accessing user-space mapped
memory via the `AT` (Address Translate) instruction or load/store to user VA.
Combined with `SMAP`/`STAC` equivalent, it implements privileged access control.

Introduced in ARMv8.1 (FEAT_PAN). If `PAN` is not supported, this bit is
`RES0` (read-as-zero/write-ignored).

---

### 2.12 `SCTLR_EL1_EPAN` — Bit 57 — **Enhanced Privileged Access Never**

```
Bit 57 = 1  → Execute-only pages (EL0 executable, EL1 non-readable) are protected
```

ARMv8.7 (FEAT_PAN3). With EPAN, if a PTE has `AP=0b01` (EL0 read-only) and
the kernel tries to read it while PAN is active, it takes a permission fault.
This closes a SMAP bypass for execute-only user mappings.

---

### 2.13 `SCTLR_ELx_ITFSB` — Bit 37 — **Tag Check Fault Synchronization**

```
Bit 37 = 1  → MTE tag check faults are synchronous
```

Required for ARM Memory Tagging Extension (FEAT_MTE2). Ensures that memory
tagging faults are reported precisely (synchronous) rather than asynchronously,
enabling correct kernel fault handling and KASAN integration.

---

### 2.14 `SCTLR_EL1_UCI` — Bit 26 — **EL0 Cache Instructions**

```
Bit 26 = 1  → DC CVAU, DC CIVAC, DC CVAC, IC IVAU usable from EL0
```

Allows user space to perform cache maintenance operations needed for JIT
compilers and self-modifying code scenarios (e.g., flush I-cache after writing
JIT-compiled instructions to memory).

---

### 2.15 Endianness Bits — `ENDIAN_SET_EL1`

```c
// arch/arm64/include/asm/sysreg.h
#ifdef CONFIG_CPU_BIG_ENDIAN
#define ENDIAN_SET_EL1    SCTLR_ELx_EE    // bit 25: Big-endian data
#else
#define ENDIAN_SET_EL1    0               // Little-endian (default)
#endif
```

For little-endian kernels (the vast majority), `ENDIAN_SET_EL1 = 0`.
`SCTLR_EL1.EE = 0` means all kernel data accesses are little-endian.

---

### 2.16 `SCTLR_EL1_LSMAOE` — Bit 29 — Load/Store Multiple Atomicity

```
Bit 29 = 1  → STM/LDM instructions across page boundaries are atomic
```

Prevents data corruption if an interrupt fires in the middle of a multi-register
load/store that crosses a page boundary.

---

### 2.17 `SCTLR_EL1_nTLSMD` — Bit 28 — **No Trap LdSt Multiple Device**

```
Bit 28 = 1  → LDM/STM to device memory does NOT trap
```

Without this bit, executing `ldm`/`stm` to device memory would trap to EL2 or
EL3. Setting it prevents spurious traps on kernels that use multi-register
stores to device regions.

---

### 2.18 `SCTLR_EL1_EIS` — Bit 22 — **Exception Entry Is Context Synchronizing**

```
Bit 22 = 1  → Exception entry acts as a context synchronization barrier
```

ARMv8.5 (FEAT_ExS). Ensures that on exception entry, all preceding instructions
have completed before the exception handler runs. This is the safe default.

---

### 2.19 `SCTLR_EL1_TSCXT` — Bit 20 — **Trap EL0 Access to SCXTNUM_EL0**

```
Bit 20 = 1  → EL0 access to SCXTNUM_EL0 (FEAT_CSV2_2) traps to EL1
```

`SCXTNUM_EL0` is used for Spectre-v2 (CSV2) software mitigations. Trapping
EL0 access allows the kernel to control the speculation context switch.

---

### 2.20 `SCTLR_EL1_EOS` — Bit 11 — **Exception Return Is Context Synchronizing**

```
Bit 11 = 1  → ERET acts as a context synchronization barrier
```

Ensures all EL1 state changes are visible before returning to EL0. The
ARM ARM notes that without `EOS=1`, system register writes made just before
`eret` might not be visible to the resumed EL0 code.

---

## 3. Bits Intentionally NOT Set

| Bit | Name   | Not set because |
|-----|--------|-----------------|
| 1   | `A`    | Alignment fault for all accesses — too aggressive; Linux uses `__unaligned` annotations |
| 19  | `WXN`  | Write implies Execute Never — not set until memory regions are fully mapped |
| 25  | `EE`   | Big-endian EL1 — only set for BE kernels |

---

## 4. How `__cpu_setup` Computes This Value

In `arch/arm64/mm/proc.S`, the last few instructions of `__cpu_setup`:

```asm
// proc.S — simplified
mov_q   x0, INIT_SCTLR_EL1_MMU_ON   // Load compile-time constant
// ... (may add CPU-specific alternative bits via cpu_enable_*)
ret                                   // Returns to __primary_switch with x0 = SCTLR value
```

`mov_q` is an assembler macro that emits a `movz` + `movk` sequence to load a
64-bit immediate into a register without memory access:

```asm
.macro mov_q, reg, val
    .if ((val) >> 31) == 0 || ((val) >> 31) == 0x1ffffffff
        mov  \reg, :abs_g0_nc:\val
    .else
        movz \reg, :abs_g3:\val
        movk \reg, :abs_g2_nc:\val
        movk \reg, :abs_g1_nc:\val
        movk \reg, :abs_g0_nc:\val
    .endif
.endm
```

This is a pure register operation — no memory load, no PC-relative access.
The value is embedded directly in the instruction stream as immediates.

---

## 5. Interview-Level Insight: What Would Break If Each Bit Were Wrong

| Bit wrong | Consequence |
|-----------|-------------|
| `M=0` | MMU never enabled — kernel crashes immediately on first kernel VA reference |
| `C=0` | D-cache off — kernel boots but runs ~100× slower; DMA coherence also affected |
| `I=0` | I-cache off — kernel boots very slowly; typically 10-50× instruction fetch penalty |
| `SA=0` | Stack misalignment silently ignored — security vulnerability in stack layout attacks |
| `SPAN=0` | Kernel code can accidentally read/write user memory without `STAC`/`LDAC` — Meltdown-style |
| `IESB=0` | SError might be attributed to wrong context — corrupts kernel oops messages |
| `EE=wrong` | Endian mismatch — all kernel data reads/writes produce garbage immediately |

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
SCTLR_EL1 is the master hardware control register for the EL1 memory system in ARMv8-A. It is a 64-bit system register written via MSR and read via MRS. On cold reset all bits are zero: MMU off, caches off, alignment checks off. The three most critical bits for the boot path are:
- Bit 0 (M): MMU enable. When set, every VA is translated through TTBR0_EL1 or TTBR1_EL1.
- Bit 2 (C): Data/unified cache enable. L1 D-cache becomes active.
- Bit 12 (I): Instruction cache enable. L1 I-cache becomes active.
The CPU pipeline treats the SCTLR write followed by an ISB as a full memory-system reconfiguration fence: all subsequent instruction fetches and data accesses use the new settings.

### Kernel Perspective (Linux ARM64)
Linux pre-computes the full SCTLR value as the compile-time constant INIT_SCTLR_EL1_MMU_ON in arch/arm64/include/asm/sysreg.h. The value is prepared in __cpu_setup (arch/arm64/mm/proc.S) and passed in x0 to __primary_switch. The kernel never modifies SCTLR bit-by-bit; the full value is written once by set_sctlr_el1 to avoid any intermediate inconsistent state. After start_kernel the register is stable for the lifetime of the CPU.

### Memory Perspective (ARMv8 Memory Model)
With SCTLR_EL1.M=0 the CPU uses a flat physical address space: every load/store address IS the physical address. With M=1 the CPU performs a two-level VA->PA lookup via the page-table walker using TTBR0_EL1 (low VA, user/identity map) and TTBR1_EL1 (high VA, kernel). The transition is instantaneous at the instruction boundary after the ISB that follows the MSR write. The identity-map page tables (mapped at __idmap_text_start) guarantee PA==VA for the code executing at the switch point so the pipeline does not fault.