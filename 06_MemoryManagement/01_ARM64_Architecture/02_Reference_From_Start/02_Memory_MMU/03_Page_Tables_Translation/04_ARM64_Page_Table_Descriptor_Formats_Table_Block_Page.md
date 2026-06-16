# ARM64 Page Table Descriptor Formats: Table vs Block vs Page

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

Each entry in an ARM64 page table is a 64-bit descriptor. The descriptor format determines what the entry points to and what attributes apply to the mapping. ARM64 defines three fundamental descriptor types:

1. **Invalid** (bits[1:0] = 0b00 or 0b10): Not mapped; access causes Translation Fault
2. **Block Descriptor** (bits[1:0] = 0b01, used at L1 and L2): Maps a large contiguous region (1GB or 2MB for 4KB granule)
3. **Table Descriptor** (bits[1:0] = 0b11, used at L0/L1/L2): Points to the next level table
4. **Page Descriptor** (bits[1:0] = 0b11, used at L3 only): Maps a single page

Note: At L3, the encoding `0b11` means Page Descriptor (leaf). At L0/L1/L2, `0b11` means Table Descriptor (non-leaf). The hardware knows the current level from the walk state, so the same encoding means different things depending on level.

---

## 2. Descriptor Type Identification

```
bits[1:0] interpretation:

At L0, L1, L2 (non-leaf levels):
  0b00 = Invalid (fault on access)
  0b01 = Block Descriptor (2MB at L2, 1GB at L1 for 4KB granule)
  0b10 = Reserved (UNPREDICTABLE behavior — never use)
  0b11 = Table Descriptor (next level table pointer)

At L3 (leaf level):
  0b00 = Invalid (fault on access)
  0b01 = Reserved (UNPREDICTABLE)
  0b10 = Reserved (UNPREDICTABLE)
  0b11 = Page Descriptor (4KB physical page)
```

---

## 3. Table Descriptor Format (L0, L1, L2)

```
Bit 63        48 47        12 11       2 1 0
 ┌──────────────┬────────────┬──────────┬─┬─┐
 │  Upper attrs │  Next table│ Ignored  │T│V│
 │  (security)  │  PA[47:12] │          │ │ │
 └──────────────┴────────────┴──────────┴─┴─┘

V (bit 0)  = 1 (valid)
T (bit 1)  = 1 (table type, combined with V = bits[1:0] = 0b11)

Next table PA[47:12] = bits[47:12] → physical address of next-level table
  (lower 12 bits are implicit zero — table must be 4KB-aligned)

Upper attributes (bits[63:59]):
  Bit[63]  NSTable  — Next level table is Non-Secure (TrustZone)
  Bit[62]  APTable  — AP limit for entire subtree (AP[1]=1: read-only for all)
  Bit[61]  XNTable  — XN for entire subtree (1: no execution below this table)
  Bit[60]  PXNTable — PXN for entire subtree (1: no privileged execution)
  Bit[59]  Reserved

Lower bits (bits[11:2]):
  Ignored by hardware (can be used for software metadata, but usually 0)
```

**Table attributes propagation**: Upper attributes in a table descriptor apply hierarchically to ALL entries below this table, in addition to the leaf descriptor's own attributes. The most restrictive combination is used. This is the Hierarchical Attribute Disabled (HAD) feature.

---

## 4. Block Descriptor Format (L1 and L2)

```
Bit 63  59 58  55 54 53 52 51 48 47       21/30 20  12 11 10 9 8 7 6 5 4:2 1 0
 ┌──────┬──────┬──┬──┬──┬─────┬───────────┬──────┬──┬─┬─┬─┬─┬─┬─────┬─┬─┐
 │ Attrs│SW use│XN│PX│Ct│ RES0│  OA[47:21]│ RES0 │nG│AF│SH│AP│NS│Mem │T│V│
 └──────┴──────┴──┴──┴──┴─────┴───────────┴──────┴──┴─┴─┴─┴─┴─┴─────┴─┴─┘

V (bit 0) = 1 (valid)
T (bit 1) = 0 (block type, bits[1:0] = 0b01)

OA = Output Address (PA of block):
  For L2 block (2MB): OA[47:21] (bits 47 down to 21; lower 21 bits = offset)
  For L1 block (1GB): OA[47:30] (bits 47 down to 30; lower 30 bits = offset)

Memory attribute fields (same as page descriptor):
  AttrIndx [4:2]  = 3-bit index into MAIR_EL1 (memory type)
  NS      [5]     = Non-Secure bit
  AP      [7:6]   = Access Permission
  SH      [9:8]   = Shareability
  AF      [10]    = Access Flag
  nG      [11]    = non-Global (1 = ASID tagged, 0 = global)
  
  Contiguous [52] = 1 if this entry is part of a contiguous hint range
  PXN [53]       = Privileged Execute Never (1 = no kernel exec)
  UXN/XN [54]    = User Execute Never (1 = no user exec)

Software use bits [58:55]: 4 bits available for OS use (PTE_DIRTY, PTE_SPECIAL, etc.)

Upper attributes [63:59]: Same role as Table descriptor upper attrs
```

---

## 5. Page Descriptor Format (L3)

The Page Descriptor is nearly identical to Block Descriptor, except:
- bits[1:0] = 0b11 (not 0b01 — this is the L3 "page" indicator)
- OA = Output Address bits[47:12] (since page offset is 12 bits)

```
Bit 63  59 58  55 54 53 52 51 48 47       12 11 10 9 8 7 6 5 4:2 1 0
 ┌──────┬──────┬──┬──┬──┬─────┬────────────┬──┬─┬─┬─┬─┬─┬─────┬─┬─┐
 │ Attrs│SW use│XN│PX│Ct│ RES0│  OA[47:12] │nG│AF│SH│AP│NS│Mem │1│1│
 └──────┴──────┴──┴──┴──┴─────┴────────────┴──┴─┴─┴─┴─┴─┴─────┴─┴─┘

OA[47:12] = Physical Page Base Address (PA of 4KB page)
Page offset = VA[11:0] (not stored in descriptor — taken directly from VA)

Final PA = OA[47:12] << 12 | VA[11:0]
```

---

## 6. Descriptor Field Reference

### AttrIndx [4:2] — Memory Attribute Index

```
3-bit index into MAIR_EL1:
  0 = MAIR_EL1[7:0]   (typically: Device-nGnRnE)
  1 = MAIR_EL1[15:8]  (typically: Normal, Inner/Outer Non-Cacheable)
  2 = MAIR_EL1[23:16] (typically: Normal, Inner/Outer WB, RA, WA)
  3 = MAIR_EL1[31:24] (typically: Normal, Outer WB, RA, WA; Inner NC)
  4-7 = remaining MAIR entries
```

### AP [7:6] — Access Permission

```
AP[1] AP[0]:
  0b00 = EL1 read/write; EL0 no access
  0b01 = EL1 and EL0 read/write (user-writable)
  0b10 = EL1 read-only; EL0 no access
  0b11 = EL1 and EL0 read-only

Note: EL0 "no access" means user space cannot access this mapping.
AP bits control Data access; PXN/UXN control Instruction fetch.
```

### SH [9:8] — Shareability

```
0b00 = Non-shareable (not coherent with other CPUs)
0b01 = Reserved (UNPREDICTABLE)
0b10 = Outer Shareable
0b11 = Inner Shareable (most common for Normal memory)
```

### AF [10] — Access Flag

```
AF=0: Access Flag fault generated on first access to this descriptor.
      Used for page aging / LRU reclaim (software AF management).
AF=1: No fault on access (normal operation).

With TCR_EL1.HA=1 (Hardware AF): hardware sets AF=1 automatically on access.
Without HA: software must set AF=1 after handling the fault.
```

### nG [11] — Non-Global

```
nG=0: Global entry — not tagged with ASID. Valid for all ASIDs.
      Used for kernel mappings (always valid regardless of current process).

nG=1: Non-global entry — ASID-tagged. Only valid for the matching ASID.
      Used for user space mappings.
```

---

## 7. Linux Kernel PTE Value Assembly

```c
// arch/arm64/include/asm/pgtable-hwdef.h

// Attribute bit positions:
#define PTE_TYPE_MASK        (3UL << 0)
#define PTE_TYPE_FAULT       (0UL << 0)   // Invalid
#define PTE_TYPE_PAGE        (3UL << 0)   // L3 page
#define PTE_TABLE_BIT        (1UL << 1)   // L0/L1/L2 table

#define PTE_USER             (1UL << 6)   // AP[1]: user access
#define PTE_RDONLY           (1UL << 7)   // AP[0]: read-only
#define PTE_SHARED           (3UL << 8)   // SH[1:0] = Inner Shareable
#define PTE_AF               (1UL << 10)  // Access Flag
#define PTE_NG               (1UL << 11)  // Non-global
#define PTE_CONT             (1UL << 52)  // Contiguous hint
#define PTE_PXN              (1UL << 53)  // Privileged Execute Never
#define PTE_UXN              (1UL << 54)  // User Execute Never

// Memory attribute index (AttrIndx):
#define PTE_ATTRINDX(x)      ((x) << 2)
#define PTE_ATTRINDX_MASK    (7UL << 2)

// Building a PTE for a normal RW user page:
static inline pte_t pfn_pte(unsigned long pfn, pgprot_t prot)
{
    return __pte((pfn << PAGE_SHIFT) | pgprot_val(prot));
}

// Example: RW user page, Normal WB memory:
// pgprot = PTE_TYPE_PAGE | PTE_AF | PTE_NG | PTE_SHARED |
//          PTE_ATTRINDX(MT_NORMAL_WB) | PTE_USER
//        = 0b11 | (1<<10) | (1<<11) | (0b11<<8) | (2<<2) | (1<<6)
//        = 0x713 (simplified)
```

---

## 8. Contiguous Hint Bit [52]

The **Contiguous** bit is an optimization hint telling the TLB hardware that a group of consecutive page/block descriptors all map a contiguous PA range:

```
For 4KB granule:
  16 × 4KB page descriptors = 64KB TLB contiguous hint
  
This allows the TLB to cache all 16 pages as a single "contig TLB entry"
→ effectively a 64KB TLB entry from a 4KB page table

Requirements for contiguous hint:
  - 16 consecutive PTE entries in the same L3 table
  - All have contiguous=1
  - Mapped to 64KB-aligned PA range
  - Same attributes (memory type, permissions)

Linux uses contiguous hint for:
  - Kernel linear map (large 64KB-aligned kernel memory ranges)
  - Transparent huge page contiguous regions
```

---

## 9. Interview Questions & Answers

**Q1: How does the ARM64 hardware distinguish a Table Descriptor from a Page Descriptor if both have bits[1:0]=0b11?**

The hardware tracks the current translation level internally during the page table walk. At L0, L1, and L2, any descriptor with bits[1:0]=0b11 is a Table Descriptor (points to the next level table). At L3, bits[1:0]=0b11 is a Page Descriptor (the leaf). The hardware never needs to "guess" — it knows the level from the walk state machine. This is why a "huge page" at L2 uses bits[1:0]=0b01 (Block) rather than 0b11, which would be interpreted as a Table Descriptor pointing to a PTE level.

**Q2: What is the Access Flag (AF) bit used for?**

The AF bit implements page aging for the page reclaim system. When a new page mapping is created (e.g., on a page fault), the kernel may clear AF=0 in the PTE. The hardware then generates an Access Flag Fault on the first access to this page, allowing the kernel to record that the page was accessed (set AF=1, mark the page as "recently used"). The kernel's page reclaim code uses this information to identify pages that haven't been accessed recently (good candidates for reclaim). With `TCR_EL1.HA=1`, the hardware sets AF automatically without a software fault.

**Q3: What is the difference between nG=0 (global) and nG=1 (non-global)?**

`nG=0` (global): The TLB entry is not tagged with an ASID. It matches for any ASID — any process can use this cached translation. The kernel uses global mappings for its own pages (TTBR1 region) so TLB entries survive context switches. `nG=1` (non-global): The TLB entry is tagged with the current ASID (from TTBR0_EL1[63:48]). It only matches for the process with that ASID. User pages use non-global mappings to prevent one process from using another's TLB entries.

---

## 10. Quick Reference: Descriptor Formats

```
bits[1:0] at Level 0/1/2:
  0b00 = Invalid (Translation Fault)
  0b01 = Block Descriptor (maps large contiguous region)
  0b11 = Table Descriptor (pointer to next level)

bits[1:0] at Level 3:
  0b11 = Page Descriptor (maps single page)
  other = Invalid or Reserved

Key attribute bits (Page/Block descriptor):
  [4:2]   AttrIndx  — Memory type (MAIR_EL1 index)
  [5]     NS        — Non-Secure
  [7:6]   AP        — Access Permissions
  [9:8]   SH        — Shareability
  [10]    AF        — Access Flag
  [11]    nG        — Non-Global (ASID tagged)
  [52]    Cont      — Contiguous hint
  [53]    PXN       — Privileged Execute Never
  [54]    UXN/XN   — (User) Execute Never
  [58:55] SW        — Software use bits
```
