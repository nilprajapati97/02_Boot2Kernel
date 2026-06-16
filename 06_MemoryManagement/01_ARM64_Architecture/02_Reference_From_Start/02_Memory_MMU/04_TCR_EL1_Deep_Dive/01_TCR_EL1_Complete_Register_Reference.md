# TCR_EL1 Complete Register Reference

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm

---

## 1. Register Overview

`TCR_EL1` (Translation Control Register, EL1) is the master configuration register for the EL0/EL1 virtual memory system. It controls:
- Virtual address range sizes for both user (TTBR0) and kernel (TTBR1)
- Granule sizes for both TTBR0 and TTBR1 page table walks
- Cache and shareability attributes for page table walk memory accesses
- The intermediate physical address size
- ASID width
- Special features: hardware AF/DB, top-byte ignore, hierarchical permissions

---

## 2. Full Bit Field Layout

```
TCR_EL1 (64-bit register):

Bit  Field    Description
─────────────────────────────────────────────────────────────────
[5:0]  T0SZ    VA size for TTBR0: VA = 64 - T0SZ bits
[6]    Reserved (RES0)
[7]    EPD0    TTBR0 page table walk disable (1 = fault on access)
[9:8]  IRGN0   Inner cacheability for TTBR0 walks
[11:10] ORGN0  Outer cacheability for TTBR0 walks
[13:12] SH0    Shareability for TTBR0 walks
[15:14] TG0    Granule size for TTBR0 (00=4KB, 01=64KB, 10=16KB)
[21:16] T1SZ   VA size for TTBR1: VA = 64 - T1SZ bits
[22]   A1      ASID source: 0=TTBR0_EL1[63:48], 1=TTBR1_EL1[63:48]
[23]   EPD1    TTBR1 page table walk disable (1 = fault on access)
[25:24] IRGN1  Inner cacheability for TTBR1 walks
[27:26] ORGN1  Outer cacheability for TTBR1 walks
[29:28] SH1    Shareability for TTBR1 walks
[31:30] TG1    Granule size for TTBR1 (01=16KB, 10=4KB, 11=64KB)
[34:32] IPS    Intermediate Physical Address size
[35]   Reserved (RES0)
[36]   AS     ASID size: 0=8-bit (256), 1=16-bit (65536)
[37]   TBI0   Top Byte Ignore for TTBR0 (1=ignore VA[63:56])
[38]   TBI1   Top Byte Ignore for TTBR1 (1=ignore VA[63:56])
[39]   HA     Hardware Access Flag (ARMv8.1): 1=HW sets AF bit
[40]   HD     Hardware Dirty Bit (ARMv8.1): 1=HW manages dirty bit
[41]   HPD0   Hierarchical Permission Disable, TTBR0
[42]   HPD1   Hierarchical Permission Disable, TTBR1
[43]   HWU059 Hardware Use, TTBR0 descriptor bit 59
[44]   HWU060 Hardware Use, TTBR0 descriptor bit 60
[45]   HWU061 Hardware Use, TTBR0 descriptor bit 61
[46]   HWU062 Hardware Use, TTBR0 descriptor bit 62
[47]   HWU159 Hardware Use, TTBR1 descriptor bit 59
[48]   HWU160 Hardware Use, TTBR1 descriptor bit 60
[49]   HWU161 Hardware Use, TTBR1 descriptor bit 61
[50]   HWU162 Hardware Use, TTBR1 descriptor bit 62
[51]   TBID0  TBI only for Data accesses, TTBR0 (ARMv8.3)
[52]   TBID1  TBI only for Data accesses, TTBR1 (ARMv8.3)
[53]   NFD0   Non-Fault access Disable, TTBR0 (SVE non-faulting loads)
[54]   NFD1   Non-Fault access Disable, TTBR1
[55]   E0PD0  EL0 fault on TTBR0 access at EL0 when EPD0=1
[56]   E0PD1  EL0 fault on TTBR1 access at EL0
[57]   TCMA0  Tag Checking Memory Attribute, TTBR0 (MTE)
[58]   TCMA1  Tag Checking Memory Attribute, TTBR1 (MTE)
[59]   DS     Dirty State Enable for Block and Page descriptors (ARMv8.7)
[63:60] Reserved (RES0)
```

---

## 3. T0SZ / T1SZ — VA Range Size

### Formula

```
Virtual Address width = 64 - TxSZ bits

T0SZ controls TTBR0 (user) VA range:
  T0SZ=16 → VA = 48 bits (standard user space, 256 TB)
  T0SZ=25 → VA = 39 bits (compact, 512 GB)
  T0SZ=22 → VA = 42 bits (used with 64KB granule)
  T0SZ=12 → VA = 52 bits (LPA2, ARMv8.7)
  T0SZ=0  → VA = 64 bits (not practically used)

T1SZ controls TTBR1 (kernel) VA range:
  T1SZ=16 → VA = 48 bits (standard kernel space, upper 256 TB)
  Must be same as T0SZ for symmetric VA split

Canonical address check:
  For T0SZ=16 (48-bit VA):
    User VA: bits[63:48] must all be 0x0000
    Kernel VA: bits[63:48] must all be 0xFFFF
    Any other pattern → Translation Fault
```

### Valid T0SZ / T1SZ Values

```
T0SZ range: 16 to 39 (for 4KB granule, 48-bit to 25-bit VA)
  Minimum T0SZ = 16 (maximum VA width = 48)
  Maximum T0SZ = 39 (minimum VA width = 25 → 512 tables at L3 only)

  For LPA (52-bit VA): T0SZ = 12 (with 64KB granule only, ARMv8.2)
  For LPA2 (52-bit VA): T0SZ = 12 (with any granule, ARMv8.7)

ARM64 constraint: T0SZ and T1SZ do not need to match,
  but Linux uses matching values for symmetric VA split.

Minimum T0SZ (maximum VA): Limited by IPS (output PA size).
  T0SZ < 16 is implementation-defined and may not be supported.
```

---

## 4. TG0 / TG1 — Granule Selection

```
TG0 encoding (for TTBR0):
  0b00 = 4 KB
  0b01 = 64 KB
  0b10 = 16 KB
  0b11 = Reserved (UNPREDICTABLE)

TG1 encoding (for TTBR1) — NOTE: DIFFERENT encoding than TG0!
  0b00 = Reserved (UNPREDICTABLE)
  0b01 = 16 KB
  0b10 = 4 KB    ← note: same value as TG0.4KB is 0b00, but TG1.4KB is 0b10!
  0b11 = 64 KB

Linux constants:
  #define TCR_TG0_4K   (0b00 << 14)   // TG0 = 4KB
  #define TCR_TG0_64K  (0b01 << 14)   // TG0 = 64KB
  #define TCR_TG0_16K  (0b10 << 14)   // TG0 = 16KB
  
  #define TCR_TG1_16K  (0b01 << 30)   // TG1 = 16KB
  #define TCR_TG1_4K   (0b10 << 30)   // TG1 = 4KB  ← different value!
  #define TCR_TG1_64K  (0b11 << 30)   // TG1 = 64KB

The TG0 ≠ TG1 encoding asymmetry is a famous ARM64 gotcha.
TG0 and TG1 can theoretically use different granules,
but Linux always uses the same granule for both.
```

---

## 5. IRGN0/1, ORGN0/1 — Walk Cacheability

```
Inner/Outer cacheability for page table memory accessed during PTW:

IRGN (Inner Region):
  0b00 = Normal memory, Inner Non-Cacheable
  0b01 = Normal memory, Inner WB RA WA (Write-Back, Read-Allocate, Write-Allocate)
  0b10 = Normal memory, Inner WT RA (Write-Through, Read-Allocate, no WA)
  0b11 = Normal memory, Inner WB RA (Write-Back, Read-Allocate, no WA)

ORGN (Outer Region):
  Same encoding as IRGN but applies to outer cache (L2/L3/LLC)

Linux standard configuration:
  IRGN = 0b01 (WB RA WA) — page tables cached with write-back + write-allocate
  ORGN = 0b01 (WB RA WA) — same for outer cache

  #define TCR_IRGN_WBWA  (1 << 8)   // IRGN0 = WB RA WA
  #define TCR_ORGN_WBWA  (1 << 10)  // ORGN0 = WB RA WA
  
Performance: WB RA WA gives best PTW performance:
  - Read-Allocate: cache miss during PTW fills a cache line
  - Write-Allocate: hardware AF/DBM writes fill cache line (not evict)
  - Write-Back: modified cache lines written to DRAM only on eviction
```

---

## 6. SH0/SH1 — Shareability for Walks

```
SH (Shareability for page table walk memory):
  0b00 = Non-Shareable
  0b01 = Reserved (UNPREDICTABLE)
  0b10 = Outer Shareable
  0b11 = Inner Shareable

Linux standard: SH = 0b11 (Inner Shareable)
  Required for SMP correctness (see Category 03 file 09)

  #define TCR_SHARED_INNER  (3 << 12)   // SH0 = Inner Shareable
  
Memory model requirement:
  Page tables must be Inner Shareable so all CPUs in the same cluster
  see coherent page table updates from any CPU.
```

---

## 7. IPS — Intermediate Physical Address Size

```
IPS controls the maximum output PA size from the MMU:

IPS[2:0]:
  0b000 = 32-bit PA (4 GB)
  0b001 = 36-bit PA (64 GB)
  0b010 = 40-bit PA (1 TB)
  0b011 = 42-bit PA (4 TB)
  0b100 = 44-bit PA (16 TB)
  0b101 = 48-bit PA (256 TB)  ← most common
  0b110 = 52-bit PA (4 PB)    ← LPA (ARMv8.2, 64KB granule only)
  0b111 = 56-bit PA (64 PB)   ← Reserved in most versions

Choosing IPS:
  Read ID_AA64MMFR0_EL1.PARange to find maximum supported PA:
    0b0000 = 32-bit PA
    0b0001 = 36-bit PA
    0b0010 = 40-bit PA
    0b0011 = 42-bit PA
    0b0100 = 44-bit PA
    0b0101 = 48-bit PA
    0b0110 = 52-bit PA (LPA)
  
  Set IPS = min(PARange, what the system needs)
  Cannot set IPS > hardware PARange (UNPREDICTABLE)

Linux:
  tcr_el1 |= (id_aa64mmfr0_el1 & 0xf) << TCR_IPS_SHIFT;
  // Directly use hardware PARange as IPS
```

---

## 8. AS — ASID Size

```
AS (bit[36]) selects 8-bit or 16-bit ASID:

AS=0: 8-bit ASID (range 0–255, 256 unique ASIDs)
AS=1: 16-bit ASID (range 0–65535, 65536 unique ASIDs)

Hardware support check:
  ID_AA64MMFR0_EL1.ASIDBits:
    0b0000 = Only 8-bit ASID supported
    0b0010 = Both 8-bit and 16-bit supported

Linux selects 16-bit if supported:
  if (cpuid_feature_extract_unsigned_field(mmfr0,
      ID_AA64MMFR0_ASIDBits_SHIFT) == 0b0010)
      tcr |= TCR_AS;  // Set 16-bit ASID

With 16-bit ASID:
  TTBR0_EL1[63:48] = ASID[15:0]
  TTBR1_EL1[63:48] = ASID[15:0] (if A1=1)
  Upper 8 bits of ASID are significant (16-bit range)
```

---

## 9. HA / HD — Hardware AF and Dirty Bit (ARMv8.1)

```
HA (bit[39]): Hardware Access Flag management
  0 = Software manages AF (faults on AF=0)
  1 = Hardware sets AF=1 atomically on access (no fault)

HD (bit[40]): Hardware Dirty Bit management
  0 = Software manages dirty state
  1 = Hardware updates AP[0] atomically on write (dirty tracking)
  
  Requires HA=1 to be set first (HD implies HA)

Linux enables if supported:
  if (cpu_have_feature(cpu_feature(HAFDBS)))
      tcr |= TCR_HA | TCR_HD;

Verification: ID_AA64MMFR1_EL1.HAFDBS:
  0b0000 = Neither HA nor HD
  0b0001 = HA only
  0b0010 = HA + HD
```

---

## 10. EPD0 / EPD1 — Page Table Walk Disable

```
EPD0 (bit[7]): Disable TTBR0_EL1 page table walks
  EPD0=0: Normal operation (walks TTBR0 for user VA range)
  EPD0=1: Any access to TTBR0 VA range generates Translation Fault
  
  Use case: Thread Local Storage, compat ABI, or during TLB flush
  
EPD1 (bit[23]): Disable TTBR1_EL1 page table walks
  EPD1=0: Normal kernel operation
  EPD1=1: Any kernel VA access generates Translation Fault
  
  CAUTION: Setting EPD1=1 makes kernel itself inaccessible!
  
  Use case: EL2 hypervisor may set EPD1=1 to detect kernel VA escapes,
  or in microkernel contexts where kernel VA must be restricted.

KPTI (Meltdown mitigation):
  User-space stub page table (tramp_pg_dir) sets TTBR0 to minimal stub
  EPD0 not used directly; instead KPTI uses a separate minimal TTBR0
  that only has the kernel entry trampoline mapped.
```

---

## 11. Linux TCR_EL1 Value Construction

```c
// arch/arm64/include/asm/pgtable-hwdef.h + head.S

// Base TCR value for 4KB pages, 48-bit VA:
#define TCR_T0SZ(x)     ((UL(64) - (x)) << TCR_T0SZ_OFFSET)
#define TCR_T1SZ(x)     ((UL(64) - (x)) << TCR_T1SZ_OFFSET)

#define TCR_EL1_VALUE   (TCR_T0SZ(VA_BITS) | TCR_T1SZ(VA_BITS) | \
                         TCR_TG0_4K | TCR_TG1_4K | \
                         TCR_IRGN_WBWA | TCR_ORGN_WBWA | \
                         TCR_SHARED_INNER | \
                         TCR_IPS_48BIT)

// Adding hardware features at runtime (after CPU detection):
if (cpu_have_feature(cpu_feature(HAFDBS)))
    tcr |= TCR_HA | TCR_HD;

if (system_supports_tbi())
    tcr |= TCR_TBI0;  // Top Byte Ignore for user pointers (MTE/PAC)

if (system_supports_cnp())
    ttbr1 |= TTBR_CNP_BIT;

msr(tcr_el1, tcr);
isb();  // Ensure TCR takes effect before page table walks

// Typical TCR_EL1 value on a modern ARM64:
// T0SZ=16, T1SZ=16, TG0=0b00(4KB), TG1=0b10(4KB), IRGN=ORGN=1(WBWA),
// SH=3(IS), IPS=5(48-bit), AS=1(16-bit), HA=1, HD=1
// = 0x00000085B5193519 (approximate)
```

---

## 12. Interview Questions & Answers

**Q1: What is the difference between TG0 and TG1 granule encoding, and why is it an interview trap?**

`TG0` (for TTBR0) and `TG1` (for TTBR1) have **different encodings** for the same granule sizes. For `TG0`: `0b00`=4KB, `0b01`=64KB, `0b10`=16KB. For `TG1`: `0b01`=16KB, `0b10`=4KB, `0b11`=64KB. The value for 4KB in `TG0` is `0b00`, but in `TG1` it is `0b10`. Using the wrong encoding results in the hardware walking page tables with the wrong granule, causing garbage translations and system crashes. This is a real bug that has appeared in bootloaders and early BSP code.

**Q2: What happens if T0SZ > T1SZ?**

A larger TxSZ means a narrower VA range. If T0SZ > T1SZ, user space has a smaller VA range than kernel space. While technically valid (the hardware supports asymmetric VA), Linux ARM64 always uses symmetric T0SZ = T1SZ to provide equal user and kernel VA ranges. Asymmetric values could be used in embedded systems to restrict user space VA (e.g., T0SZ=25 for 39-bit user VA, T1SZ=16 for 48-bit kernel VA). The key constraint is that user VA (bits 0 to 64-T0SZ-1) and kernel VA (bits 64-T1SZ to 63) must not overlap.

**Q3: Why must IPS be set to match the actual hardware PA range?**

Setting `IPS` too high (beyond the hardware's actual PA capability) is UNPREDICTABLE — the hardware may generate incorrect PA outputs or generate faults. Setting `IPS` too low artificially restricts the accessible physical memory. For example, on a system with 40-bit PA hardware (`ID_AA64MMFR0_EL1.PARange=0b0010`), setting `IPS=0b101` (48-bit) would be illegal. Linux reads `ID_AA64MMFR0_EL1.PARange` and directly uses it for IPS, ensuring the maximum physically-supported PA range is used.

---

## 13. Quick Reference

| Field | Bits | Purpose |
|---|---|---|
| T0SZ | [5:0] | TTBR0 VA width = 64 - T0SZ |
| EPD0 | [7] | Disable TTBR0 walk (1=fault) |
| IRGN0 | [9:8] | Inner cache for TTBR0 walk |
| ORGN0 | [11:10] | Outer cache for TTBR0 walk |
| SH0 | [13:12] | Shareability for TTBR0 walk |
| TG0 | [15:14] | TTBR0 granule (00=4KB) |
| T1SZ | [21:16] | TTBR1 VA width = 64 - T1SZ |
| A1 | [22] | ASID source (0=TTBR0) |
| EPD1 | [23] | Disable TTBR1 walk |
| IRGN1 | [25:24] | Inner cache for TTBR1 walk |
| ORGN1 | [27:26] | Outer cache for TTBR1 walk |
| SH1 | [29:28] | Shareability for TTBR1 walk |
| TG1 | [31:30] | TTBR1 granule (10=4KB) |
| IPS | [34:32] | Intermediate PA size |
| AS | [36] | ASID size (0=8-bit, 1=16-bit) |
| TBI0 | [37] | Top Byte Ignore TTBR0 |
| TBI1 | [38] | Top Byte Ignore TTBR1 |
| HA | [39] | Hardware Access Flag |
| HD | [40] | Hardware Dirty Bit |
| HPD0 | [41] | Hierarchical Perm Disable TTBR0 |
| HPD1 | [42] | Hierarchical Perm Disable TTBR1 |
