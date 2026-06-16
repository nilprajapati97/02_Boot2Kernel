# IPS: Intermediate Physical Address Size

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept: What is IPS?

`TCR_EL1.IPS` (bits[34:32]) — Intermediate Physical Address Size — controls the maximum **output** physical address size from the EL1/EL0 MMU translation.

```
"Intermediate" because in virtualization, this is the intermediate PA (IPA)
that becomes the guest PA input to Stage 2 translation.
At EL1 without virtualization, IPS = the final PA size.

IPS limits the PA bits in the output address of every page table walk.
Setting IPS too large for the hardware → UNPREDICTABLE.
Setting IPS too small → PA above the IPS limit cannot be mapped.
```

---

## 2. IPS Encoding

```
IPS (bits[34:32]):
  0b000 = 32-bit PA (4 GB)
  0b001 = 36-bit PA (64 GB)
  0b010 = 40-bit PA (1 TB)
  0b011 = 42-bit PA (4 TB)
  0b100 = 44-bit PA (16 TB)
  0b101 = 48-bit PA (256 TB)  ← most common in production SoCs
  0b110 = 52-bit PA (4 PB)    ← requires LPA (ARMv8.2, 64KB granule)
  0b111 = 56-bit PA           ← LPA2 (ARMv8.7, more restricted)

Memory aid:
  IPS value: 0=32-bit, 1=36-bit, 2=40-bit, 3=42-bit, 4=44-bit, 5=48-bit, 6=52-bit
  PA size: 32 + (IPS × 4) for IPS=0–4, then 48 and 52 for IPS=5,6
  (The spacing is not uniform — 42 and 44 break the pattern)
```

---

## 3. Reading Hardware PA Capability: ID_AA64MMFR0_EL1.PARange

```
The hardware reports its maximum supported PA via:
ID_AA64MMFR0_EL1.PARange (bits[3:0]):

  0b0000 = 32-bit PA (4 GB) max
  0b0001 = 36-bit PA (64 GB) max
  0b0010 = 40-bit PA (1 TB) max
  0b0011 = 42-bit PA (4 TB) max
  0b0100 = 44-bit PA (16 TB) max
  0b0101 = 48-bit PA (256 TB) max  ← standard modern ARM64 SoC
  0b0110 = 52-bit PA (4 PB) max    ← LPA support

PARange encoding = IPS encoding (they use the same values)
→ Linux directly copies PARange to IPS:

// arch/arm64/kernel/head.S (simplified):
mrs  x0, ID_AA64MMFR0_EL1
ubfx x0, x0, #0, #4          // Extract PARange (bits[3:0])
bfi  tcr, x0, #32, #3         // Insert into IPS (bits[34:32])
// PARange value 0b0110 directly encodes to IPS=52-bit PA
```

---

## 4. LPA: Large Physical Address Extension (52-bit PA)

```
LPA (ARMv8.2 extension) allows 52-bit PA with 64KB granule only:

Normal IPS=0b101 gives 48-bit PA with any granule.
IPS=0b110 gives 52-bit PA BUT requires:
  - 64KB translation granule (TG0/TG1 = 64KB)
  - ID_AA64MMFR0_EL1.PARange >= 0b0110

With LPA + 52-bit PA:
  PTE output address (OA) bits[51:48] are stored in TTBR register bits[5:2]
  (TTBR0_EL1[5:2] = OA[51:48])
  Block/Page descriptor bits[15:12] hold OA[51:48] (for 64KB granule)
  → Unusual bit packing that must be handled by OS software

LPA2 (ARMv8.7):
  52-bit PA with ANY granule (4KB, 16KB, 64KB)
  More straightforward — OA[51:48] stored in normal descriptor fields
  T0SZ = 12 for 52-bit VA with 4KB granule
  ID_AA64MMFR0_EL1.TGRAN4_2 indicates LPA2 support

Linux LPA2 support added in kernel 5.17+
```

---

## 5. IPS vs Physical RAM

```
IPS does NOT need to match the amount of installed DRAM.
IPS sets the maximum addressable PA range.

Examples:
  Server with 256 TB DRAM → IPS = 0b101 (48-bit = 256 TB range)
  Embedded SoC with 4 GB DRAM → IPS could still be 0b010 (40-bit)
    because the SoC's memory controller only decodes 40-bit PA
  Phone with 16 GB DRAM → IPS = 0b001 (36-bit PA handles up to 64 GB)

The critical match is: IPS ≤ hardware PARange (from ID_AA64MMFR0_EL1)
IPS can be less than PARange (restricts addressable PA artificially)
IPS cannot exceed PARange (UNPREDICTABLE)

Linux always sets IPS = PARange (max hardware capability).
This allows the OS to map the entire installed DRAM regardless of size.
```

---

## 6. IPS in Stage 2 (Virtualization)

```
In KVM/ARM64 (virtualization):

Stage 1 (Guest OS, controlled by VTTBR_EL2 + VTCR_EL2):
  TCR_EL1 (in guest) → controls guest VA → guest PA (IPA) translation
  IPS in guest TCR_EL1 → sets max guest IPA size

Stage 2 (Hypervisor, controlled by VTTBR_EL2):
  VTCR_EL2.PS → controls IPA → Host PA size
  Must be >= guest IPA size

The name "Intermediate Physical Address Size" makes sense here:
  IPS in the guest's TCR_EL1 limits the IPA (Intermediate PA)
  VTCR_EL2.PS limits the final Host PA

On a non-virtualized system (EL1 only):
  "Intermediate" is a misnomer — IPS directly limits the final PA.
  There is no Stage 2. IPS = final PA limit.
```

---

## 7. Consequences of Wrong IPS

```
IPS set too LOW:
  PA above the IPS limit cannot be mapped
  Example: 4 GB DRAM at 0x80000000–0xFFFFFFFF = needs 32-bit PA → OK with IPS=0
  But if DRAM extends above 4 GB (e.g., 0x100000000), IPS=0 prevents mapping it
  Linux maps all DRAM in the linear map → if IPS is too small, physmem_init panics

IPS set too HIGH:
  Behavior is CONSTRAINED UNPREDICTABLE
  Hardware may:
  - Ignore the invalid IPS bits → use max supported PA
  - Generate fault on any PA above hardware limit
  - Silently truncate PA to hardware maximum

Correct detection (Linux arch/arm64/kernel/head.S):
  // Read PARange from ID register
  mrs  x5, ID_AA64MMFR0_EL1
  // Extract bits[2:0] for IPS (same encoding as PARange)
  ubfx x5, x5, #ID_AA64MMFR0_PARANGE_SHIFT, #3
  // Set IPS in TCR_EL1
  orr  x10, x10, x5, lsl #TCR_IPS_SHIFT
```

---

## 8. Interview Questions & Answers

**Q1: What is IPS in TCR_EL1, and how does Linux choose its value?**

`IPS` (bits[34:32]) controls the maximum output physical address size from the EL1 MMU. Linux reads `ID_AA64MMFR0_EL1.PARange` (which uses the same encoding: 0b101=48-bit PA, etc.) and directly copies it to `IPS`. This ensures Linux configures the maximum PA the hardware supports. Setting IPS below the hardware PARange would artificially restrict addressable physical memory; setting it above would be UNPREDICTABLE. On a typical modern ARM64 server, PARange=0b101 → IPS=0b101 → 48-bit PA (256 TB addressable physical memory).

**Q2: Why is IPS called "Intermediate" Physical Address Size even on a non-virtualized system?**

In a virtualized environment, the Stage 1 MMU translates guest VA → guest Physical Address (IPA = Intermediate Physical Address), and Stage 2 translates IPA → Host Physical Address. The `IPS` field limits the guest's IPA output size. In a non-virtualized EL1 system, there is no Stage 2, so the "Intermediate" PA from Stage 1 is directly the final PA — but ARM retained the name "IPS" for consistency. The value is still used to limit PA bits in the page table descriptor output addresses.

**Q3: What happens if you try to map DRAM at 0x100000000 (above 4 GB) with IPS=0b000 (32-bit)?**

The PTE stores OA (Output Address) with at most 32 significant bits (IPS=0). Address 0x100000000 requires bit 32 to be set — above the 32-bit limit. The hardware will use only OA[31:12] (the low 32 bits of the PA), truncating or misinterpreting the address. This causes the mapping to point to the wrong physical memory (PA[31:12] = 0 for address 0x100000000 since bit 32 is masked). Memory accesses through this mapping would hit PA=0x00000000 instead of 0x100000000, causing catastrophic memory corruption.

---

## 9. Quick Reference

| IPS[2:0] | PA bits | PA range | PARange code |
|---|---|---|---|
| 0b000 | 32 | 4 GB | 0b0000 |
| 0b001 | 36 | 64 GB | 0b0001 |
| 0b010 | 40 | 1 TB | 0b0010 |
| 0b011 | 42 | 4 TB | 0b0011 |
| 0b100 | 44 | 16 TB | 0b0100 |
| 0b101 | 48 | 256 TB | 0b0101 |
| 0b110 | 52 | 4 PB (LPA) | 0b0110 |

| Rule | Value |
|---|---|
| IPS must be ≤ | `ID_AA64MMFR0_EL1.PARange` |
| Linux sets IPS = | hardware PARange |
| Wrong IPS (high) | CONSTRAINED UNPREDICTABLE |
| Wrong IPS (low) | Physical memory above IPS limit unmappable |
