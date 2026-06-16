# HWU Bits and PBHA: Hardware Use Descriptor Fields

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. What are HWU Bits?

`TCR_EL1` contains 8 HWU (Hardware Use) bits that indicate which descriptor upper attribute bits are available for hardware use (as opposed to being software-defined bits). This enables the **PBHA (Page-Based Hardware Attributes)** extension.

```
TCR_EL1 HWU fields:
  HWU059 (bit[43]): TTBR0 descriptor bit 59 is hardware-use (PBHA[0])
  HWU060 (bit[44]): TTBR0 descriptor bit 60 is hardware-use (PBHA[1])
  HWU061 (bit[45]): TTBR0 descriptor bit 61 is hardware-use (PBHA[2])
  HWU062 (bit[46]): TTBR0 descriptor bit 62 is hardware-use (PBHA[3])
  
  HWU159 (bit[47]): TTBR1 descriptor bit 59 is hardware-use (PBHA[0])
  HWU160 (bit[48]): TTBR1 descriptor bit 60 is hardware-use (PBHA[1])
  HWU161 (bit[49]): TTBR1 descriptor bit 61 is hardware-use (PBHA[2])
  HWU162 (bit[50]): TTBR1 descriptor bit 62 is hardware-use (PBHA[3])

When HWUx_y = 0: descriptor bit y is a software-defined bit (SW bits [58:55])
When HWUx_y = 1: descriptor bit y is reserved for hardware use (PBHA implementation)

Default (Linux): All HWU bits = 0 (no hardware use of these descriptor bits)
```

---

## 2. Standard vs PBHA Descriptor Bit Usage

```
PTE/Block descriptor upper attributes (bits[63:51]):

Standard ARM64 (HWU=0):
  Bit[63:60] = PBHA[3:0] or Reserved — not used unless HWU bits enable PBHA
  Bit[59] = UXN/XN  — user execute-never
  Bit[58] = PXN     — privileged execute-never  
  Bit[57] = Reserved (was XNTable in some ARM revisions)
  Bit[56] = Reserved
  Bit[55] = SW[0]   — software-defined bit 0 (Linux: PTE_DIRTY / PTE_SPECIAL)
  Bit[54] = SW[1]   — software-defined bit 1
  Bit[53] = SW[2]   — software-defined bit 2
  Bit[52] = Contiguous hint (or SW bit)
  Bit[51] = DBM     — Dirty Bit Management (ARMv8.1)

  Specifically in Linux arm64 pte bits:
  Bit[55] = PTE_DIRTY    (software dirty tracking)
  Bit[56] = PTE_SPECIAL  (special mapping, e.g., pfn_valid=false)
  Bit[57] = PTE_DEVMAP   (device memory via vm_insert_pfn)
  Bit[58] = PTE_PROT_NONE (PROT_NONE mapping, swap entry marker)

With PBHA enabled (HWU059-062 = 1):
  Bits[62:59] = PBHA[3:0] — page-based hardware attributes
  → Linux SW bits at [58:55] are PRESERVED (they are below bit 59)
  → Bits [62:59] become hardware-controlled, not software
```

---

## 3. PBHA (Page-Based Hardware Attributes)

```
PBHA is an ARMv8.5 feature that allows hardware (CPU microarchitecture,
cache, interconnect) to use 4 descriptor bits[62:59] for implementation-
specific purposes.

Common microarchitecture uses of PBHA bits:

1. Cache Partitioning (LLC way-locking / cache QoS):
   SoC hardware may use PBHA[0] to route a page to a specific cache partition.
   PBHA=0 → LLC partition 0 (normal traffic)
   PBHA=1 → LLC partition 1 (latency-sensitive, e.g., real-time tasks)
   
2. Prefetch Control:
   PBHA[1] = 1: Enable hardware prefetcher for this page's data
   PBHA[1] = 0: Disable prefetching (for memory-mapped IO-like regions
                that shouldn't pollute cache with speculative prefetch)

3. Memory Encryption:
   Some SoC implementations use PBHA bits to control memory encryption keys:
   PBHA[3:2] = encryption key index (0–3, for up to 4 memory encryption domains)

4. Quality of Service (QoS):
   PBHA[2:0] = QoS priority hint for interconnect bandwidth allocation

Note: PBHA is IMPLEMENTATION-DEFINED.
  The ARM spec only says bits[62:59] are available for hardware use
  if the corresponding HWU bits are set in TCR_EL1.
  What the hardware DOES with these bits is up to the SoC designer.
```

---

## 4. Setting HWU Bits in TCR_EL1

```c
// To enable PBHA bits[62:59] for TTBR0 (user) pages:
u64 tcr = read_sysreg(tcr_el1);
tcr |= TCR_HWU059 | TCR_HWU060 | TCR_HWU061 | TCR_HWU062;
// = (1UL << 43) | (1UL << 44) | (1UL << 45) | (1UL << 46)
write_sysreg(tcr, tcr_el1);
isb();

// Now pages can be tagged with PBHA values in PTE[62:59]:
pte_t pte = mk_pte(page, prot);
pte = pte_modify(pte, __pgprot(pte_val(pte) | (pbha_value << 59)));
set_pte(ptep, pte);

// Requirement: Only safe if hardware actually implements PBHA
// Check ID_AA64MMFR1_EL1.HPDS (Hierarchical Permission Disables + PBHA support):
//   ID_AA64MMFR1_EL1.HPDS (bits[15:12]):
//     0b0000 = HWU not supported
//     0b0001 = HWU for TTBR0/TTBR1 supported (HPD supported)
//     0b0010 = HWU + PBHA supported (ARMv8.5)
```

---

## 5. Risks of Enabling HWU Without Hardware Support

```
If HWU bits are set in TCR but hardware doesn't implement PBHA:

Scenario A (hardware ignores bits[62:59]):
  PBHA bits in PTE are stored and retrieved correctly but have no effect
  Software can still use bits[62:59] as pure software bits
  Result: harmless but pointless

Scenario B (hardware misinterprets bits):
  Some hardware (pre-ARMv8.5) may interpret bits[62:59] differently:
  Bit[62] = APTable[1] for table descriptors
  Bit[61] = APTable[0] for table descriptors
  Setting HWU062=1 on such hardware might cause the PTE to
  look like a table descriptor with APTable set → permission override!

  For LEAF page descriptors, bits[62:61] are standard permission bits
  in some older hardware variants.
  Setting PBHA values here could accidentally restrict permissions.

Safety rule: ONLY set HWU bits if PBHA is confirmed by ID register check.
Linux does NOT set HWU bits by default (all zero).
Vendor BSPs for PBHA-capable SoCs may enable them.
```

---

## 6. Interaction with Linux Software Bits

```
Linux ARM64 uses PTE bits[58:55] as software-defined bits:
  Bit[58] = PTE_PROT_NONE  — page is protected (PROT_NONE mmap)
  Bit[57] = PTE_DEVMAP     — device memory page
  Bit[56] = PTE_SPECIAL    — special (non-struct-page-backed) mapping
  Bit[55] = PTE_DIRTY      — software dirty tracking (when HW dirty not available)

With HWU059-062 = 0 (Linux default):
  Bits[62:59] are Reserved/Software and Linux may use them for SW bits
  The ARM spec says: when HWU=0, bits[62:59] are IGNORED by hardware
  → Linux can safely use them as SW bits without hardware interference

With HWU059-062 = 1 (PBHA enabled):
  Bits[62:59] are claimed by hardware
  Linux must NOT use them for software bits
  → The Linux SW bits at [58:55] are unaffected (they're always SW bits)

PBHA does NOT conflict with Linux SW bits [58:55] because PBHA uses [62:59].
The conflict would be if someone tried to use [62:59] as SW bits while PBHA is enabled.
```

---

## 7. Relationship to FEAT_HPDS (ARMv8.2) and FEAT_HPDS2 (ARMv8.5)

```
Historical progression of HWU/PBHA support:

ARMv8.0: No hierarchical permission disable, no HWU bits
  Table descriptor bits[62:59] = APTable/PXNTable/XNTable
  These are always active (hierarchical perms always apply)

ARMv8.2 — FEAT_HPDS (Hierarchical Permission Disables):
  Adds HPD0/HPD1 to TCR_EL1 to disable hierarchical perms
  Adds HWU fields to TCR_EL1 (but descriptor bits still same)
  HWU allows OS to designate bits[62:59] as software-only
  → When HPD=1 (table-level APTable/PXN/XN disabled), bits[62:59]
    in TABLE descriptors are free for software use

ARMv8.5 — FEAT_HPDS2 (full PBHA):
  HWU bits in TCR now explicitly enable PBHA
  When HWU=1, hardware may use bits[62:59] for implementation features
  ID_AA64MMFR1_EL1.HPDS = 0b0010 confirms PBHA support

Detection:
  FEAT_HPDS:  ID_AA64MMFR1_EL1.HPDS[15:12] >= 0b0001
  FEAT_HPDS2: ID_AA64MMFR1_EL1.HPDS[15:12] >= 0b0010
```

---

## 8. Interview Questions & Answers

**Q1: What are the HWU bits in TCR_EL1, and what do they enable?**

`HWU059–HWU062` (bits[43:46] for TTBR0, bits[47:50] for TTBR1) are the **Hardware Use** bits in `TCR_EL1`. Each bit controls whether the corresponding descriptor bit (59, 60, 61, or 62) in page/block table entries is available for hardware use (PBHA — Page-Based Hardware Attributes). When all four bits are set (`HWU062=HWU061=HWU060=HWU059=1`), descriptor bits[62:59] become implementation-defined hardware control bits rather than software-reserved. PBHA implementations use these bits for cache partitioning, prefetch control, QoS hints, or memory encryption key selection. Linux defaults to `HWU=0` (all software-reserved), since mainstream ARM64 hardware doesn't expose PBHA to OS in standard Linux configurations.

**Q2: Can you safely use PTE bits[62:59] as software-defined metadata in Linux?**

Only when `HWU=0` (the default). When `HWU=0`, the ARM architecture says bits[62:59] in leaf page/block descriptors are **ignored by hardware** — they don't affect cache behavior, permissions, or execution. The OS can store any value there. However, Linux currently only uses bits[58:55] as software bits (`PTE_DIRTY`, `PTE_SPECIAL`, `PTE_DEVMAP`, `PTE_PROT_NONE`). If `HWU` bits are set (PBHA enabled), then bits[62:59] are claimed by hardware, and placing OS metadata there would interfere with hardware operation. Any OS extension that uses bits[62:59] as SW bits MUST verify `HWU=0` or use bits in the [58:55] range which are always available for software use.

---

## 9. Quick Reference

| TCR bit | Field | Enables |
|---|---|---|
| [43] | HWU059 | PTE bit 59 = PBHA[0] for TTBR0 |
| [44] | HWU060 | PTE bit 60 = PBHA[1] for TTBR0 |
| [45] | HWU061 | PTE bit 61 = PBHA[2] for TTBR0 |
| [46] | HWU062 | PTE bit 62 = PBHA[3] for TTBR0 |
| [47] | HWU159 | PTE bit 59 = PBHA[0] for TTBR1 |
| [48] | HWU160 | PTE bit 60 = PBHA[1] for TTBR1 |
| [49] | HWU161 | PTE bit 61 = PBHA[2] for TTBR1 |
| [50] | HWU162 | PTE bit 62 = PBHA[3] for TTBR1 |

| PTE bits | Standard Linux use | With PBHA |
|---|---|---|
| [62:59] | Reserved (ignored by HW) | PBHA[3:0] (HW-use) |
| [58] | PTE_PROT_NONE | PTE_PROT_NONE (unchanged) |
| [57] | PTE_DEVMAP | PTE_DEVMAP (unchanged) |
| [56] | PTE_SPECIAL | PTE_SPECIAL (unchanged) |
| [55] | PTE_DIRTY | PTE_DIRTY (unchanged) |
