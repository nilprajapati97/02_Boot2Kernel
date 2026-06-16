# SCTLR_EL1 — MMU Enable, Cache Enable, Alignment Check

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

`SCTLR_EL1` (System Control Register, EL1) is the master control register for the EL1 execution environment. It controls:
- **MMU** enable/disable (virtual-to-physical translation).
- **Data cache** (D-cache) enable/disable.
- **Instruction cache** (I-cache) enable/disable.
- **Alignment checking** for memory accesses.
- **Write-implies-XN** policy.
- **Stack alignment** enforcement.
- Various other micro-architectural controls.

At reset, `SCTLR_EL1` has most features disabled. The kernel boot sequence progressively enables features as the system is initialized. The most critical operation is enabling the MMU (bit M=1) which transitions the processor from running on physical addresses to virtual addresses.

---

## 2. SCTLR_EL1 Bit Field Layout

```
Bits [63:32] — Reserved (FEAT_MTE, FEAT_PAN, etc. extensions live here in newer versions)

Key bits [31:0]:
Bit[31]  DSSBS  — Default SSBS (Speculative Store Bypass Safe) value on EL0 entry
Bit[30]  TWED   — TWE Delay (ARMv8.6) — trap WFE with delay
Bit[29:28] TWEDEL — Delay value for TWED
Bit[27]  ENTR   — Trap reads of ENCODIAS
Bit[26]  UCI    — Trap EL0 cache maintenance instructions
Bit[25]  EE     — Exception Endianness (0=LE, 1=BE for EL1 data)
Bit[24]  E0E    — Endianness for EL0 (0=LE, 1=BE)
Bit[23]  SPAN   — Set PAN (Privileged Access Never) on exception entry
Bit[22]  EIS    — Exception Interrupt behavior (ARMv8.5)
Bit[21]  IESB   — Implicit Error Synchronization Barrier enable
Bit[20]  TSCXT  — Trap EL0 access to SCXTNUM_EL0
Bit[19]  WXN    — Write-implies-XNever (write permission implies Execute Never)
Bit[18]  nTWE   — Trap WFE from EL0 (0=trap to EL1, 1=no trap)
Bit[17]  Reserved
Bit[16]  nTWI   — Trap WFI from EL0 (0=trap to EL1, 1=no trap)
Bit[15]  UCT    — User Cache Type register access (0=trap, 1=allow EL0 access to CTR_EL0)
Bit[14]  DZE    — User DC ZVA access (0=trap DC ZVA from EL0, 1=allow)
Bit[13]  EnDB   — Enable PAC (Pointer Auth, key B, data)
Bit[12]  I      — I-Cache enable (0=I-cache disabled, 1=enabled)
Bit[11]  EOS    — Exception Output Synchronization
Bit[10]  EnRCTX — Enable RCCS register access
Bit[9]   UMA    — User Mask Access (0=trap MSR/MRS DAIF from EL0)
Bit[8]   SED    — SETEND instruction disable (AArch32 only)
Bit[7]   ITD    — IT instruction disable (AArch32 Thumb only)
Bit[6]   nAA    — Non-Aligned Access (0=alignment fault enabled, 1=disabled)
Bit[5]   CP15BEN — CP15 barrier enable (AArch32 compat)
Bit[4]   SA0    — Stack alignment check for EL0 (0=disabled, 1=enabled)
Bit[3]   SA     — Stack alignment check for EL1 (0=disabled, 1=enabled)
Bit[2]   C      — D-Cache enable (0=data cache disabled, 1=enabled)
Bit[1]   A      — Alignment fault check (0=disabled, 1=faults on unaligned access)
Bit[0]   M      — MMU enable (0=MMU off: VA=PA, 1=MMU on: translation active)
```

---

## 3. Critical Bits Deep Dive

### Bit 0 — M (MMU Enable)

This is the most critical bit in the system.

**M=0 (MMU disabled)**:
- VA = PA: Every address issued by the CPU is treated as a physical address.
- No TLB lookups occur.
- Memory access permissions are not checked by the MMU.
- All memory behaves as Device-nGnRnE (strongly ordered) by default.
- Used during: boot, before page tables are set up, and in hypervisor mode before Stage 2 is configured.

**M=1 (MMU enabled)**:
- Full virtual-to-physical translation via `TTBR0_EL1`/`TTBR1_EL1` and page tables.
- TLB lookups active.
- Memory attribute from MAIR_EL1 applied via PTE AttrIdx.
- Access permission faults can be generated.

**Critical sequence for enabling MMU** (from `arch/arm64/kernel/head.S`):
```asm
// 1. Set up page tables in TTBR0_EL1 and TTBR1_EL1
msr TTBR0_EL1, x25       // Identity-mapped page table for current PA
msr TTBR1_EL1, x26       // Kernel page table

// 2. Configure TCR_EL1, MAIR_EL1
msr TCR_EL1, x10
msr MAIR_EL1, x12

// 3. ISB to ensure register writes are visible
isb

// 4. Enable MMU + D-cache + I-cache in SCTLR_EL1
mrs x0, SCTLR_EL1
orr x0, x0, #SCTLR_ELx_M    // Bit 0
orr x0, x0, #SCTLR_ELx_C    // Bit 2
orr x0, x0, #SCTLR_ELx_I    // Bit 12
msr SCTLR_EL1, x0

// 5. MANDATORY ISB — flushes pipeline, next fetch uses MMU
isb

// Now executing with virtual addresses!
```

### Bit 2 — C (Data Cache Enable)

**C=0**: All data memory accesses bypass the D-cache and go directly to the memory subsystem. This is valid for device memory anyway (device memory is always non-cacheable), but for Normal memory, this means every load/store goes to RAM — catastrophically slow.

**C=1**: Normal memory (MAIR attributes with WB/WT) uses the D-cache according to its MAIR_EL1 attributes. Device memory (nGnRnE etc.) is still non-cacheable regardless.

**Linux initial value**: Enabled during `paging_init()`. Before this point, early boot code runs with C=0 to avoid cache coherency issues before page tables map all memory.

### Bit 12 — I (Instruction Cache Enable)

**I=0**: All instruction fetches bypass I-cache. Performance penalty is severe.
**I=1**: Instruction fetches use the I-cache.

**Note**: Even with I=0, the CPU still fetches instructions correctly — they just always come from memory. With I=1 and code in Normal Cacheable memory, instruction fetches are cached.

**Linux**: Enabled simultaneously with M and C during MMU enable.

### Bit 1 — A (Alignment Check)

**A=0**: Unaligned memory accesses are handled silently. The CPU issues multiple aligned accesses internally (hardware alignment fixup). Performance penalty but no fault.

**A=1**: Unaligned access generates an alignment fault (Synchronous exception to EL1 with `ESR_EL1.EC = 0x21` for data abort). The kernel handles alignment faults in `do_alignment_fault()`.

**Linux default**: A=0 (alignment faults disabled for normal accesses) for performance. However, device memory (mapped with strict MAIR attributes) enforces its own alignment requirements independent of this bit.

**Interview trap**: The `nAA` bit (bit 6) controls Natural Alignment Checking for Load/Store Exclusive and Load-Acquire/Store-Release instructions, which is separate from the general `A` bit.

### Bit 19 — WXN (Write-implies-XNever)

**WXN=0**: Writable memory can also be executable (if UXN/PXN not set in PTE). This is the permissive default.

**WXN=1**: Any page with write permission (AP[2]=0 → writable) is **automatically treated as Execute Never**, even if UXN/PXN bits in the PTE are 0. This enforces W^X at the hardware level for the entire address space.

**Linux**: `SCTLR_EL1.WXN=1` is set to enforce W^X for kernel hardening (`CONFIG_ARM64_SW_TTBR0_PAN` or via `ARM64_SCTLR_EL1_INIT`).

### Bit 4 — SA0 (Stack Alignment Check for EL0)
### Bit 3 — SA (Stack Alignment Check for EL1)

**SA=1 / SA0=1**: The SP must be 16-byte aligned when executing a `SP-related` load/store. Specifically, the AArch64 AAPCS requires 16-byte stack alignment. With SA enabled, any load/store pair (`LDP`/`STP`) using SP that is not 16-byte aligned causes an alignment fault.

**Linux**: Both SA and SA0 are enabled in production kernels, enforcing the AArch64 ABI stack alignment requirement.

### Bit 25 — EE (Exception Endianness)
### Bit 24 — E0E (EL0 Endianness)

ARM64 is bi-endian for data accesses. **EE=0** (default) means EL1 operates in little-endian mode. **EE=1** would be big-endian EL1 data (rare, used in some network equipment). Instructions are always little-endian regardless.

**Linux**: EE=0 (little-endian) for ARM64 Linux.

---

## 4. Linux Kernel SCTLR_EL1 Initial Value

The Linux kernel defines the initial SCTLR_EL1 value:

```c
// arch/arm64/include/asm/sysreg.h

#define SCTLR_EL1_SET  (SCTLR_ELx_M    |  /* MMU enable */         \
                         SCTLR_ELx_C    |  /* D-cache enable */     \
                         SCTLR_ELx_SA   |  /* Stack align EL1 */    \
                         SCTLR_EL1_SA0  |  /* Stack align EL0 */    \
                         SCTLR_EL1_SED  |  /* Disable SETEND */     \
                         SCTLR_ELx_I    |  /* I-cache enable */     \
                         SCTLR_EL1_DZE  |  /* Allow DC ZVA at EL0 */\
                         SCTLR_EL1_UCT  |  /* Allow CTR_EL0 read */ \
                         SCTLR_EL1_nTWI |  /* No WFI trap */        \
                         SCTLR_EL1_nTWE |  /* No WFE trap */        \
                         SCTLR_ELx_IESB |  /* Implicit ESB */       \
                         SCTLR_EL1_SPAN |  /* Set PAN on exc entry */\
                         SCTLR_ELx_ITFSB)  /* TAG fault sync */

#define SCTLR_EL1_CLR  (SCTLR_ELx_A    |  /* No alignment faults */ \
                         SCTLR_EL1_CP15BEN |                         \
                         SCTLR_EL1_ITD)
```

The actual value on a typical 64-bit Linux kernel with most features enabled is around `0x34D5D91D`.

---

## 5. SCTLR_EL1 During Kernel Boot (Sequence)

```
Reset:         SCTLR_EL1 = implementation defined (typically 0x00C50078)
               M=0, C=0, I=0 — MMU off, caches off

early boot:    Set up identity mapping in TTBR0_EL1
               Configure TCR_EL1, MAIR_EL1

__enable_mmu:  SCTLR_EL1 |= (M | C | I)
               ISB
               Now running with MMU enabled on virtual addresses

paging_init:   Full page table setup, TTBR1_EL1 = swapper_pg_dir
               TTBR0_EL1 = reserved page (until first process)

init/main.c:   start_kernel() runs, all system registers fully configured
```

---

## 6. SCTLR_EL1 vs SCTLR_EL2 vs SCTLR_EL3

Each EL has its own SCTLR register controlling that EL's operation:

```
SCTLR_EL1: Controls EL1 (Linux kernel) operation and EL0 (user) aspects
SCTLR_EL2: Controls EL2 (hypervisor) MMU, caches (when not VHE)
SCTLR_EL3: Controls EL3 (TF-A) MMU, caches

Key difference: SCTLR_EL2 also controls Stage 2 behavior when VM=0
                SCTLR_EL3 is entirely independent — TF-A manages it
```

---

## 7. Interview Questions & Answers

**Q1: What is the exact sequence to enable the MMU on ARM64 and why must ISB follow?**

Before enabling MMU:
1. Build page tables and write `TTBR0_EL1`, `TTBR1_EL1`.
2. Configure `TCR_EL1` (address space size, granule, shareability).
3. Configure `MAIR_EL1` (memory type attributes).
4. Execute `ISB` to ensure all register writes are committed.
5. Set `SCTLR_EL1.M=1` (and typically C=1, I=1 simultaneously).
6. Execute `ISB` — this flushes the instruction pipeline.

The second `ISB` is mandatory because without it, instructions fetched speculatively after the `MSR SCTLR_EL1` might be fetched using the old (MMU-off) execution state. The `ISB` ensures the pipeline is flushed and subsequent instruction fetches use the MMU.

**Q2: Can the kernel disable the MMU while running?**

Theoretically yes (by clearing `SCTLR_EL1.M`), but it's catastrophically dangerous. Once the MMU is disabled, all accesses use physical addresses. The kernel virtual addresses would be invalid. This is only done in specific scenarios: kexec (boot a new kernel), kdump (panic recovery), or hardware bring-up testing. The code that disables the MMU must be in a physically-mapped region and must immediately jump to physical addresses.

**Q3: What is the effect of WXN on kernel security?**

With `WXN=1`, any page mapped as writable (AP[2]=0) is automatically treated as Execute Never, even if the PTE's PXN/UXN bits say it's executable. This hardware-enforces the W^X policy: attacker code written to writable memory cannot be executed. This closes a class of code injection attacks. Linux sets WXN=1 as part of kernel hardening.

**Q4: Why is alignment checking (bit A) typically disabled in production Linux?**

AArch64 hardware supports efficient unaligned accesses on Normal memory — the CPU internally handles them in a way that's slightly slower but functionally correct. Enabling A=1 would generate alignment faults on every unaligned access (common in network packet processing, string operations). The fault handling overhead would be much worse than the hardware alignment fixup cost. However, for Device memory, unaligned accesses have undefined behavior regardless of the A bit.

**Q5: What does `SCTLR_EL1.SPAN` do in the context of PAN?**

`SPAN` = Set PAN (Privileged Access Never) on exception entry. When `SPAN=1`, every time an exception is taken to EL1, `PSTATE.PAN` is automatically set to 1. This means as soon as the kernel exception handler starts, kernel code cannot access user-space memory directly (without using `uaccess` wrappers). This protects against Spectre-like attacks where kernel handlers inadvertently access user data. Without `SPAN`, the kernel must manually set PAN at every exception entry.

---

## 8. Quick Reference

| Bit | Name | Default | Effect when 1 |
|---|---|---|---|
| 0 | M | 0 | MMU enabled |
| 1 | A | 0 | Alignment faults enabled |
| 2 | C | 0 | D-cache enabled |
| 3 | SA | 0 | EL1 stack alignment enforced |
| 4 | SA0 | 0 | EL0 stack alignment enforced |
| 12 | I | 0 | I-cache enabled |
| 19 | WXN | 0 | Write implies Execute Never |
| 23 | SPAN | 0 | Set PAN on exception entry |
| 24 | E0E | 0 | EL0 big-endian |
| 25 | EE | 0 | EL1 big-endian |
| 26 | UCI | 0 | Trap EL0 cache ops |
