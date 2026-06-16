# TCR_EL1: T0SZ, T1SZ and VA Range Size Control

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Concept Foundation

`TCR_EL1` (Translation Control Register, EL1) is the central control register for the MMU's address translation at EL1. It determines:
1. **How big the VA space is** — via T0SZ and T1SZ
2. **Which granule size** — 4KB, 16KB, or 64KB
3. **Cacheability and shareability of page table walks** — performance critical
4. **Whether ASID is 8-bit or 16-bit** — affects TLB capacity
5. **Whether TBI (Top Byte Ignore) is on** — used for MTE/PAC
6. **Whether Hardware AF/Dirty bit management is enabled**

`TCR_EL1` is a 64-bit register accessible only from EL1 (and EL2/EL3 by their own equivalents). The Linux kernel sets it at boot and rarely changes it afterward.

---

## 2. TCR_EL1 Full Bit Field Layout

```
Bits [63:60]  — Reserved (FEAT_* extension fields in higher versions)
Bit[59]   DS    — Descriptor Size (ARMv8.7 FEAT_D128: 128-bit descriptors)
Bit[58]   TCMA1 — Tag Checked Memory Attr for TTBR1 region (MTE)
Bit[57]   TCMA0 — Tag Checked Memory Attr for TTBR0 region (MTE)
Bit[56]   E0PD1 — EL0 access to TTBR1 region faults (ARMv8.5)
Bit[55]   E0PD0 — EL0 access to TTBR0 region faults
Bit[54]   NFD1  — Non-Fault translation Disable TTBR1
Bit[53]   NFD0  — Non-Fault translation Disable TTBR0
Bit[52]   TBID1 — TBI for instruction only (TTBR1) (ARMv8.3)
Bit[51]   TBID0 — TBI for instruction only (TTBR0)
Bit[50]   HWU162 — HW Use Bit 62 for TTBR1
Bit[49]   HWU161 — HW Use Bit 61 for TTBR1
Bit[48]   HWU160 — HW Use Bit 60 for TTBR1
Bit[47]   HWU159 — HW Use Bit 59 for TTBR1
Bit[46]   HWU062 — HW Use Bit 62 for TTBR0
Bit[45]   HWU061
Bit[44]   HWU060
Bit[43]   HWU059
Bit[42]   HPD1  — Hierarchical Permission Disable TTBR1 (ARMv8.1)
Bit[41]   HPD0  — Hierarchical Permission Disable TTBR0
Bit[40]   HD    — Hardware Dirty bit management (ARMv8.1)
Bit[39]   HA    — Hardware Access Flag management (ARMv8.1)
Bit[38]   TBI1  — Top Byte Ignore for TTBR1 region
Bit[37]   TBI0  — Top Byte Ignore for TTBR0 region
Bit[36]   AS    — ASID size (0=8-bit ASID, 1=16-bit ASID)
Bit[35]   — Reserved
Bit[34:32] IPS  — Intermediate Physical Address Size (OA bits supported)
                   000 = 32-bit PA
                   001 = 36-bit PA
                   010 = 40-bit PA
                   011 = 42-bit PA
                   100 = 44-bit PA
                   101 = 48-bit PA
                   110 = 52-bit PA (LPA)
Bit[31:30] TG1  — Granule size for TTBR1 (kernel)
                   01 = 16KB
                   10 = 4KB
                   11 = 64KB
Bit[29:28] SH1  — Shareability for TTBR1 table walks
                   00 = Non-shareable
                   10 = Outer Shareable
                   11 = Inner Shareable
Bit[27:26] ORGN1 — Outer cache attributes for TTBR1 walks
                   00 = Non-cacheable
                   01 = Write-Back, Read-Allocate, Write-Allocate Cacheable
                   10 = Write-Through
                   11 = Write-Back, Read-Allocate only
Bit[25:24] IRGN1 — Inner cache attributes for TTBR1 walks
                   (same encoding as ORGN1)
Bit[23]   EPD1  — Walk disable for TTBR1 (1=fault, 0=walk)
Bit[22]   A1    — ASID source (0=TTBR0.ASID, 1=TTBR1.ASID)
Bit[21:16] T1SZ — VA size offset for TTBR1: VA = 64 - T1SZ bits
Bit[15:14] TG0  — Granule size for TTBR0 (user)
                   00 = 4KB
                   01 = 64KB
                   10 = 16KB
Bit[13:12] SH0  — Shareability for TTBR0 table walks
Bit[11:10] ORGN0 — Outer cache attr for TTBR0 walks
Bit[9:8]  IRGN0 — Inner cache attr for TTBR0 walks
Bit[7]    EPD0  — Walk disable for TTBR0 (1=fault all user accesses)
Bit[6]    — Reserved
Bit[5:0]  T0SZ  — VA size offset for TTBR0: VA = 64 - T0SZ bits
```

---

## 3. T0SZ and T1SZ — The Core VA Size Control

### Formula

```
Effective VA bits for TTBR0 region = 64 - T0SZ
Effective VA bits for TTBR1 region = 64 - T1SZ
```

### Typical Linux ARM64 Values

```c
// For 48-bit VA (CONFIG_ARM64_VA_BITS=48), 4KB granule:
T0SZ = 16   →  64 - 16 = 48-bit user VA
T1SZ = 16   →  64 - 16 = 48-bit kernel VA

// For 39-bit VA (some embedded configs):
T0SZ = 25   →  64 - 25 = 39-bit user VA
T1SZ = 25   →  64 - 25 = 39-bit kernel VA

// For 52-bit VA (LPA, 64KB granule):
T0SZ = 12   →  64 - 12 = 52-bit user VA
T1SZ = 12   →  64 - 12 = 52-bit kernel VA
```

### Linux Boot Setup

```c
// arch/arm64/kernel/head.S
// Macro to compute TCR value:
#define TCR_TxSZ(x)      ((UL(64) - (x)) << TCR_T0SZ_SHIFT) | \
                          ((UL(64) - (x)) << TCR_T1SZ_SHIFT)

// arch/arm64/include/asm/pgtable-hwdef.h
#define TCR_T0SZ_OFFSET  0
#define TCR_T1SZ_OFFSET  16
#define TCR_IPS_SHIFT    32
#define TCR_IPS_MASK     (7UL << TCR_IPS_SHIFT)

// arch/arm64/mm/proc.S — tcr_set_el1_t0sz macro:
.macro  tcr_compute_pa_size, tcr, pos, tmp, tmp2
        mrs     \tmp, id_aa64mmfr0_el1
        // Read PA range from ID register, encode in TCR.IPS
        ...
.endm
```

---

## 4. Granule Size Fields — TG0 and TG1

```
TG0 (TTBR0 granule):
  0b00 = 4KB    → Page size = 4096 bytes
  0b01 = 64KB   → Page size = 65536 bytes
  0b10 = 16KB   → Page size = 16384 bytes

TG1 (TTBR1 granule):
  0b01 = 16KB
  0b10 = 4KB
  0b11 = 64KB
```

Note: TG0 and TG1 encodings are different! TG1 encoding does NOT match TG0.

```c
// arch/arm64/include/asm/pgtable-hwdef.h
#if defined(CONFIG_ARM64_4K_PAGES)
#define TCR_TG_FLAGS    TCR_TG0_4K | TCR_TG1_4K
#elif defined(CONFIG_ARM64_16K_PAGES)
#define TCR_TG_FLAGS    TCR_TG0_16K | TCR_TG1_16K
#elif defined(CONFIG_ARM64_64K_PAGES)
#define TCR_TG_FLAGS    TCR_TG0_64K | TCR_TG1_64K
#endif
```

---

## 5. Cache Attributes for Table Walks

Page table walks are memory accesses themselves. The `IRGN`/`ORGN`/`SH` fields control how these walk accesses are cached:

```
Linux sets:
  IRGN = 0b01 (Inner Write-Back, Read-Allocate, Write-Allocate)
  ORGN = 0b01 (Outer Write-Back, Read-Allocate, Write-Allocate)
  SH   = 0b11 (Inner Shareable)

This makes page table walk accesses cacheable in both L1 and L2 caches,
and coherent across all CPUs sharing the inner domain.
```

If these were set to Non-Cacheable (0b00), every page table walk would go to DRAM — catastrophic performance impact. Modern ARM64 systems rely on the table walk cache (TWC, part of the TLB subsystem) and regular data caches to accelerate walks.

---

## 6. EPD0 and EPD1 — Walk Disable

```
EPD0 = 1: All accesses via TTBR0 generate Translation Fault (regardless of page tables)
EPD1 = 1: All accesses via TTBR1 generate Translation Fault
```

**EPD0=1 use case**: When a process has no user-space mappings (kernel thread), the kernel can set EPD0=1 to immediately fault any user-space access without spending cycles walking an empty page table. This is a safety mechanism.

**EPD1=1 use case**: Rarely used. Could be set to disable kernel access for testing, but Linux never does this in production.

---

## 7. IPS — Intermediate Physical Address Size

`TCR_EL1.IPS` tells the MMU how many physical address bits the hardware supports for the Output Address (PA) from translation:

```c
// Linux reads the PA size from ID_AA64MMFR0_EL1.PARange:
// and programs TCR.IPS accordingly:
static u64 get_tcr_ips(void)
{
    u64 mmfr0 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);
    u64 pa_range = cpuid_feature_extract_unsigned_field(mmfr0,
                   ID_AA64MMFR0_EL1_PARANGE_SHIFT);
    // Map ID register value → TCR.IPS value
    return min(pa_range, (u64)ID_AA64MMFR0_EL1_PARANGE_48);
}
```

Setting IPS to a smaller value than the hardware supports is allowed (conservatively limits PA space). Setting it larger than hardware supports causes UNPREDICTABLE behavior.

---

## 8. A1 — ASID Source

```
A1=0: ASID is taken from TTBR0_EL1[63:48] (user process ASID)
A1=1: ASID is taken from TTBR1_EL1[63:48] (kernel ASID)
```

Linux uses `A1=0`, so the ASID stored in `TTBR0_EL1` identifies the current process. When the kernel switches processes, it writes a new TTBR0_EL1 with the new process's ASID and page table base.

---

## 9. Complete Linux TCR_EL1 Value Example

```c
// arch/arm64/mm/proc.S — typical TCR_EL1 value for 48-bit VA, 4KB pages:
#define TCR_EL1_VALUE    \
    TCR_TxSZ(VA_BITS)  | \  // T0SZ=16, T1SZ=16
    TCR_CACHE_FLAGS    | \  // IRGN/ORGN = 0b01, SH = 0b11
    TCR_SMP_FLAGS      | \  // SH = Inner Shareable
    TCR_TG_FLAGS       | \  // TG0=4KB, TG1=4KB
    TCR_KASLR_FLAGS    | \  // ASID related
    TCR_ASID16         | \  // AS=1: 16-bit ASID
    TCR_TBI0           | \  // TBI0=1: Top Byte Ignore for user (if MTE/PAC)
    TCR_A1             | \  // A1=0: ASID from TTBR0
    TCR_IPS_48BIT           // IPS=101 (48-bit PA)
```

---

## 10. Interview Questions & Answers

**Q1: How does TCR_EL1 control the size of the virtual address space?**

`TCR_EL1.T0SZ` defines the user VA size as `64 - T0SZ` bits. `TCR_EL1.T1SZ` defines the kernel VA size similarly. Linux sets both to `64 - VA_BITS` where `VA_BITS` is configured at build time (typically 48). This creates a 48-bit user space (`TTBR0`) and a 48-bit kernel space (`TTBR1`) separated by the canonical address hole. The hardware uses these values to determine which TTBR to use and how many bits to extract from the VA for translation.

**Q2: What happens if T0SZ is increased after the system is running?**

Increasing T0SZ (reducing VA size) would invalidate any user-space pointers that use bits beyond the new range. This is dangerous — it would cause translation faults for existing processes. Linux never changes T0SZ at runtime for this reason. The only dynamic change is to the ASID/TTBR0 (process switch) and TTBR1 (KPTI). T0SZ/T1SZ are set once at boot.

**Q3: Why do IRGN and ORGN matter for performance?**

Page table walks generate memory accesses for each level (4 accesses for a 4-level walk). If IRGN/ORGN are Non-Cacheable, all these accesses go directly to DRAM (100+ ns each). With Write-Back Cacheable (0b01), the table entries are cached in L1/L2. On a TLB miss for a deeply nested structure, the difference between non-cacheable and cacheable walks can be 5-10× in latency. Linux always sets IRGN=ORGN=0b01 for this reason.

**Q4: What is EPD0 used for in practice?**

`EPD0=1` disables all TTBR0 translations. It is used for kernel threads that should never access user space — if a kernel thread accidentally dereferences a user-space pointer (NULL dereference in a 0x0... address), it immediately faults instead of walking potentially stale page tables. Linux can optionally use this as a defense against some kernel information leak attacks. However, most Linux deployments use PAN (`PSTATE.PAN=1`) instead as it's more fine-grained.

---

## 11. Quick Reference Table

| Field | Bits | Linux Value (48-bit, 4KB) | Effect |
|---|---|---|---|
| T0SZ | [5:0] | 16 | 48-bit user VA |
| T1SZ | [21:16] | 16 | 48-bit kernel VA |
| TG0 | [15:14] | 0b00 | 4KB page (user) |
| TG1 | [31:30] | 0b10 | 4KB page (kernel) |
| IRGN0 | [9:8] | 0b01 | Inner WB cacheable walk |
| ORGN0 | [11:10] | 0b01 | Outer WB cacheable walk |
| SH0 | [13:12] | 0b11 | Inner Shareable walk |
| AS | [36] | 1 | 16-bit ASID |
| IPS | [34:32] | 0b101 | 48-bit PA |
| A1 | [22] | 0 | ASID from TTBR0 |
| TBI0 | [37] | 1 | Top Byte Ignore (PAC/MTE) |
| EPD0 | [7] | 0 | Walk enabled |
| EPD1 | [23] | 0 | Walk enabled |
