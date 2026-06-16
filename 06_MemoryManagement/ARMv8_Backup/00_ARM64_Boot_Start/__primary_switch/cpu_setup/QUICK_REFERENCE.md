# __cpu_setup Quick Reference Guide

## One-Page Overview

```
PURPOSE: Initialize Virtual Memory System Architecture on each ARM64 CPU

LOCATION: arch/arm64/kernel/head.S (assembly)
SECTION: .idmap.text (identity-mapped, 1:1 VA=PA)
SIZE: ~600 lines of assembly code

CALLED FROM:
- primary_entry: Primary CPU boot
- secondary_startup: Secondary CPU boot
- cpu_resume: Resume from suspend
- Hotplug: CPU online/offline

RUNS: Once per CPU per boot/resume cycle
```

---

## Register Quick Reference

### Input State
```
x0-x30:       May be clobbered (caller-saved)
SCTLR_EL1:    Unknown (about to be reconfigured)
TCR_EL1:      Unknown (about to be reconfigured)
MAIR_EL1:     Unknown (will be overwritten)
TTBR0_EL1:    Unknown (caller sets later)
TTBR1_EL1:    Unknown (caller sets later)
TLB:          May contain bootloader entries (will be cleared)
```

### Output State (x0 Return Value Only)
```
x0:           INIT_SCTLR_EL1_MMU_ON (~0x3c5f83b1)
              Returned, not written to SCTLR_EL1
              Caller writes this value to SCTLR_EL1 after page tables ready
```

### Modified Registers
```
MAIR_EL1:     Programmed with 8 memory attribute slots
TCR_EL1:      Programmed with address space configuration
TCR2_EL1:     Programmed (if CPU supports ARMv8.2+)
CPACR_EL1:    Reset to disable coprocessor access
MDSCR_EL1:    Reset to disable debug
PMU/AMU:      Reset/disabled
TLB:          Fully invalidated (tlbi vmalle1)
```

### NOT Modified (Caller Responsibility)
```
TTBR0_EL1:    Caller sets after page tables ready
TTBR1_EL1:    Caller sets after page tables ready
SCTLR_EL1:    Caller writes returned value from x0
```

---

## Memory Attribute (MAIR) Values

```
Index  Value   Name                  Properties
-----  -----   ----                  ----------
0      0x00    Device_nGnRnE         No cache, no reorder, no early ack
1      0xFF    Normal_cached         Write-back, read/write allocate
2      0x44    Normal_noncached      Write-back, no allocate
3      (varies) Custom1              Platform-specific
4      (varies) Custom2              Platform-specific
5      (varies) Custom3              Platform-specific
6      (varies) Custom4              Platform-specific
7      (varies) Custom5              Platform-specific
```

**Typical INIT_MAIR_EL1**: `0xff443c0400`
- Indexes 0-2: Standard (Device, Normal_cached, Normal_noncached)
- Indexes 3-7: Reserved or custom

---

## TCR_EL1 Key Fields

```
Field   Bits    Typical Value   Meaning
-----   ----    ---------------  -------
T1SZ    [38:37] + [15:0]         Kernel VA space (TCR_EL1[21:16] + [5:0])
                 16-20           16 = 48-bit VA range
T0SZ    [5:0]   16               User VA space
                                 16 = 48-bit VA range
TG1     [31:30] 2                Kernel page granule (0=4KB, 1=16KB, 2=64KB)
TG0     [15:14] 0                User page granule (0=4KB, 1=16KB, 2=64KB)
IPS     [34:32] 5                Physical address width (5=48-bit PA)
AS      [36]    1 or 0           ASID width (1=16-bit, 0=8-bit)
ORGN1   [11:10] 1                Outer cacheable (kernel)
IRGN1   [9:8]   1                Inner cacheable (kernel)
ORGN0   [3:2]   1                Outer cacheable (user)
IRGN0   [1:0]   1                Inner cacheable (user)
SH0,SH1 [13:12, 25:24]  2        Shareability (2=inner-shareable)
```

---

## Critical Sequences

### 1. TLB Clear + Synchronization
```asm
tlbi vmalle1            // Invalidate all TLB entries
dsb nsh                 // Data synchronization (non-shareable)
isb                     // Instruction synchronization
```
**Cost**: ~200-300 cycles

### 2. Register Write + Synchronization
```asm
mov_q x0, VALUE
msr   REGISTER_EL1, x0  // Write register
isb                     // Instruction synchronization
```
**Cost**: ~10-20 cycles per register

### 3. Break-Before-Make (BBM) for TCR Changes
```asm
// 1. TLB clear
tlbi vmalle1
dsb nsh

// 2. Register write
msr tcr_el1, x0
isb

// 3. TLB clear again
tlbi vmalle1
dsb nsh
isb
```
**Total Cost**: ~400-500 cycles

---

## CPU Errata Patterns

### Detection Pattern
```asm
// Read CPU ID
mrs   x0, midr_el1      // Model ID + revision
// Extract fields
ubfx  x1, x0, #24, #8   // Part number
ubfx  x2, x0, #0, #4    // Variant
ubfx  x3, x0, #4, #4    // Revision

// Compare against known values
cmp   x1, #0x47         // Cortex-A72?
b.eq  apply_errata_a72
```

### Common Errata
```
Cortex-A72:    #853709   - TLB corruption (extra sync)
Cortex-A55:    #1530923  - Cache maintenance (serialization)
Neoverse-N1:   #1542419  - Prefetch (BTB flush)
Cortex-A76:    #1472981  - Speculative loads
```

---

## Address Space Layout (Typical 48-bit VA)

```
User Space (TTBR0):
0x0000_0000_0000_0000  ─┐
       ▼ (248 TB)       │ User VA space (256 TB)
0x0000_ffff_ffff_ffff  ─┘

Reserved:
0x0001_0000_0000_0000  ─┐
       ▼ (ignored)      │ Unused (2 EB)
0xffff_0000_0000_0000  ─┘

Kernel Space (TTBR1):
0xffff_0000_0000_0000  ─┐
       ▼ (248 TB)       │ Kernel VA space (256 TB)
0xffff_ffff_ffff_ffff  ─┘
```

**Key**: T0SZ=16 (user), T1SZ=16 (kernel) = 48-bit effective VA

---

## Performance Values

```
Operation                   Cycles    Impact
---------                   ------    ------
tlbi vmalle1 + dsb          200-300   TLB coherency
msr register_el1 + isb      5-10      Register setup
TCR change (BBM)            400-500   Critical path
Entire __cpu_setup          500-1000  Boot-time
```

**Impact on boot**: <0.1% (dominated by page table init, I/O)

---

## Debugging Checklist

| Issue | Check |
|-------|-------|
| Hangs during boot | Is TLB cleared before TCR? |
| Address faults | Is TCR T0SZ/T1SZ correct? |
| Stale data visible | Is ISB after system register write? |
| CPU-specific crash | Is errata applied for this CPU? |
| Memory corruption | Is MAIR correct? |
| Slow boot | Are MAIR/TCR caching enabled? |

---

## Code References in Linux

| File | Line | Purpose |
|------|------|---------|
| head.S | ~129 | Call __cpu_setup (primary) |
| head.S | ~600 | Call __cpu_setup (secondary) |
| sleep.S | ~150 | Call __cpu_setup (resume) |
| cpu_errata.c | ~50+ | Errata detection |
| sysreg.h | ~100+ | Register definitions |
| memory.h | ~50+ | Memory macros |

---

## Quick Facts

- **Function size**: ~600 lines assembly
- **Execution time**: 1-2 microseconds (CPU clocks)
- **Called per boot**: 1 (primary) + N-1 (secondary) where N = CPU count
- **Called per resume**: N times (once per CPU)
- **Return value**: x0 only (SCTLR_EL1 config value)
- **CPU count**: Supports 1-10,000+ CPUs
- **Architecture versions**: Handles ARMv8.0 through ARMv8.8+
- **Known CPU variants**: 50+ different models supported
- **Errata handled**: 100+ known CPU bugs

---

## For More Information

**Quick topics** (5-10 min):
- See VAULT_SUMMARY.md and INDEX.md

**Specific topics** (30 min):
- See relevant numbered sections (07-19)

**Deep understanding** (2-3 hours):
- Read complete sequence (00-24)

**Debugging guide** (variable):
- See section 24 (FAQ/debugging)

**Register details** (reference):
- See section 21 (register atlas)

---

*This quick reference covers the most important concepts. See full vault for complete details.*
