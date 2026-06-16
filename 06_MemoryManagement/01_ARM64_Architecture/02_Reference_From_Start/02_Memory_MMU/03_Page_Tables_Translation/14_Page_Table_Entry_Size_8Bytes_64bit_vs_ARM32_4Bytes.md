# Page Table Entry Size: 8 Bytes (64-bit) vs ARM32 4 Bytes

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

The fundamental difference in page table entry size between ARM32 and ARM64 reflects the underlying architectural evolution:

- **ARM32 (AArch32)**: 4-byte (32-bit) PTEs, using the legacy Short-descriptor format or Long-descriptor (LPAE) format
- **ARM64 (AArch64)**: 8-byte (64-bit) PTEs exclusively, using the ARM64 long-descriptor format

This 2× size increase has significant implications:
1. Each page table is 2× larger (4KB for 512 entries × 8B vs 4KB for 1024 entries × 4B)
2. More bits available for physical addresses (48 or 52 bits vs 32 or 40 bits)
3. More bits available for attributes (security, virtualization, MTE, PAC, etc.)
4. More physical address range supported without special extensions

---

## 2. ARM32 Page Table Entry Formats

### ARM32 Short Descriptor (Classic, 32-bit VA, 32-bit PA)

```
32-bit Level 1 descriptor (section descriptor for 1MB mapping):
 31      20 19 18 17 16 15 14 12 11 10 9 8 7 6 5 4 3 2 1 0
  ┌─────────┬──┬──┬──┬──┬──┬──┬────┬──┬──┬──┬──┬──┬──┬──┬──┐
  │ BA[31:20]│NG│S │AP│ TEX│XN│ Dom│ P│AP│C │B │1 │B │ 0│10│
  └─────────┴──┴──┴──┴──┴──┴──┴────┴──┴──┴──┴──┴──┴──┴──┴──┘
  bits[1:0] = 0b10 → Section descriptor (1MB)
  BA[31:20] = Section base address → 1MB aligned
  TEX[2:0] + C + B = Memory type encoding

32-bit L2 small page descriptor (4KB mapping):
 31       12 11 10 9 8 7 6 5 4 3 2 1 0
  ┌──────────┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
  │ PA[31:12] │nG│S │AP │TEX│XN│AP│C │B │1 │1 │XN│
  └──────────┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
  bits[1:0] = 0b10 + bit[9]=1 for small page descriptor
  PA[31:12] = Physical frame address (20 bits → 4KB aligned)
  
Limitations:
  PA: only 32 bits → max 4GB physical memory
  No ASID in PTE (ASIDs existed in TTBR)
  No direct hardware Dirty Bit tracking
  Limited attribute bits (TEX+C+B encoding was complex and confusing)
```

### ARM32 LPAE (Large Physical Address Extension) — Long Descriptor

ARM LPAE was introduced for Cortex-A15 to support up to 40-bit PA and uses 64-bit descriptors, similar to ARM64:

```
LPAE L2 Page Descriptor (64-bit):
 63  52 51     40 39  12 11 10  9  8 7  6  5 4:2  1  0
  ┌────┬──────────┬──────┬──┬──┬──┬──┬──┬──┬──┬───┬──┬──┐
  │ UP │ UNK/Ign  │PA[39:12│nG│AF│SH│AP│NS│0│AtIndx│T │V │
  └────┴──────────┴──────┴──┴──┴──┴──┴──┴──┴──┴───┴──┴──┘
  
  PA: 40 bits (PA[39:12]) → 1 TB physical memory
  This is already close to ARM64 format!
  
  LPAE used 3-level tables (L0, L1, L2)
  L1 descriptors: 2 × 4KB (concatenated) for 32-bit VA
```

---

## 3. ARM64 Page Table Entry (64-bit)

```
ARM64 L3 Page Descriptor (standard, 4KB granule):
 63  59 58  55 54 53 52 51 48 47         12 11 10 9:8 7:6 5 4:2 1 0
  ┌────┬──────┬──┬──┬──┬─────┬─────────────┬──┬──┬───┬───┬─┬───┬─┬─┐
  │Uatt│SWuse │XN│PX│Ct│RES0 │  OA[47:12]  │nG│AF│ SH│ AP│NS│Ati│T │V│
  └────┴──────┴──┴──┴──┴─────┴─────────────┴──┴──┴───┴───┴─┴───┴─┴─┘
  
  OA[47:12] = 36 bits of physical address → 2^(36+12) = 256 TB physical
  Full attribute set: XN, PXN, Cont, SH, AP, AF, nG, AttrIndx
  Software use bits: 4 bits for OS (dirty, special, PROT_NONE, devmap)
  Upper attributes: APTable, XNTable, PXNTable, NSTable (cascade protection)
  
  Extra capacity enables:
    - PAC (Pointer Authentication) codes in upper bits
    - MTE allocation tags (bits[63:60] for tagged pointers)
    - LPA2 PA extension (52-bit PA in TTBR and block descriptors)
```

---

## 4. Comparison: Key Differences

```
Feature                │ ARM32 Short Desc │ ARM32 LPAE  │ ARM64
───────────────────────┼──────────────────┼─────────────┼────────────
Entry size             │ 4 bytes          │ 8 bytes     │ 8 bytes
Max VA                 │ 32 bits          │ 32 bits     │ 48/52 bits
Max PA                 │ 32 bits          │ 40 bits     │ 48/52 bits
Table entries          │ 256/1024         │ 512         │ 512
Page table size        │ 1 KB / 4 KB      │ 4 KB        │ 4 KB
ASID in descriptor     │ No               │ nG bit only │ nG + TTBR ASID
Hardware AF            │ No               │ No          │ Yes (ARMv8.1)
Hardware DBM           │ No               │ No          │ Yes (ARMv8.1)
Contiguous hint        │ No               │ No          │ Yes
PAC support            │ No               │ No          │ Yes
MTE support            │ No               │ No          │ Yes
PXN (privileged XN)   │ Yes (section)    │ Yes         │ Yes
Descriptor bit[1:0]    │ Section/Coarse   │ Block/Table │ Block/Table/Page
Number of levels       │ 2                │ 3           │ 2–5 (config)
Memory type encoding   │ TEX+C+B (complex)│ AttrIndx+MAIR│ AttrIndx+MAIR
```

---

## 5. Page Table Memory Footprint Comparison

### ARM32 Short Descriptor

```
L1 Table (first level):
  4096 entries × 4 bytes = 16 KB
  Each entry covers 1 MB of VA → 4096 entries = 4 GB

L2 Table (second level, for 4KB pages):
  256 entries × 4 bytes = 1 KB
  Each entry covers 4 KB → 256 × 4KB = 1 MB per L2 table

Full 4GB user address space:
  L1: 16 KB
  L2: up to 4096 × 1 KB = 4 MB (worst case)
```

### ARM64 4KB Granule

```
PGD (L0): 512 entries × 8 bytes = 4 KB
PUD (L1): 512 entries × 8 bytes = 4 KB (per PGD entry)
PMD (L2): 512 entries × 8 bytes = 4 KB (per PUD entry)
PTE (L3): 512 entries × 8 bytes = 4 KB (per PMD entry)

For 1GB user process (fully mapped with 4KB pages):
  PGD: 4 KB (1 entry used out of 512)
  PUD: 4 KB (1 entry used)
  PMD: 4 KB × 512 = 2 MB (512 PMD entries for 1GB range)
  PTE: 4 KB × 512 × 512 = 1 GB (worst case; with 2MB blocks = only PMD needed)

With 2MB huge pages for 1GB process:
  PGD: 4 KB
  PUD: 4 KB
  PMD: 4 KB (512 PMD block descriptors for 1GB → 1 table)
  PTE: none
  Total: 12 KB (vs 1 GB+ for 4KB pages)
```

---

## 6. Linux Kernel Type Sizes

```c
// arch/arm64/include/asm/pgtable-types.h

// ARM64: all descriptor types are u64 (8 bytes)
typedef u64 pteval_t;
typedef u64 pmdval_t;
typedef u64 pudval_t;
typedef u64 pgdval_t;

// Struct wrappers for type safety:
typedef struct { pteval_t pte; } pte_t;   // sizeof = 8
typedef struct { pmdval_t pmd; } pmd_t;   // sizeof = 8
typedef struct { pudval_t pud; } pud_t;   // sizeof = 8
typedef struct { pgdval_t pgd; } pgd_t;   // sizeof = 8

// On ARM32 (for comparison):
// typedef u32 pteval_t;  // sizeof = 4
// typedef struct { pteval_t pte_low, pte_high; } pte_t;  // LPAE: 8 bytes
// Short descriptor: pte_t = u32 (4 bytes)

// Sanity check at compile time:
BUILD_BUG_ON(sizeof(pte_t) != sizeof(pteval_t));  // Must be 8
```

---

## 7. 64-bit PTE Enables Modern Security Features

The extra 32 bits in ARM64 PTEs enable features impossible in ARM32:

### PAC (Pointer Authentication Codes)

```
PAC uses bits[55:49] or bits[63:56] of pointers for authentication codes.
For page table entries (OA only 48 bits), upper bits [63:48] are available
for software use or hardware features like PAC tagging.

Not directly in PTEs, but in pointers stored IN memory protected by PTEs.
64-bit pointers allow PAC encoding; 32-bit pointers cannot.
```

### MTE (Memory Tagging Extension)

```
MTE uses bits[59:56] of 64-bit addresses for allocation tags.
Tagged pointers require 64-bit address space.
PTE.AttrIndx = MT_NORMAL_TAGGED enables MTE for a region.
ARM32 cannot support MTE (insufficient address bits).
```

### PBHA (Page-Based Hardware Attributes)

```
ARM64 descriptors have bits[62:59] defined as PBHA (Page-Based HW Attributes)
in some implementations. These 4 bits propagate from TLB to the load/store
unit, allowing hardware to implement custom per-page behaviors.
ARM32 has no equivalent.
```

---

## 8. Interview Questions & Answers

**Q1: Why are ARM64 page table entries 8 bytes instead of 4 bytes?**

The primary reason is the need for larger physical addresses — ARM64 supports up to 48-bit (or 52-bit with LPA) physical address space, requiring more bits in the PA field of descriptors than 32-bit entries allow. Additionally, ARM64 descriptors pack many more attribute fields than ARM32: Upper/Lower Table attributes (hierarchical protection), Hardware AF, Hardware DBM, Contiguous hint, PXN, UXN, software-use bits, and room for future extensions (PAC, MTE, PBHA). The 8-byte entry aligns with 64-bit processor word size (all reads/writes to PTEs are naturally aligned), and 512 entries × 8 bytes = exactly one 4KB page, which is a clean and efficient layout.

**Q2: How does ARM32 LPAE relate to ARM64 page tables?**

ARM LPAE (Large Physical Address Extension) introduced 64-bit descriptors and a 3-level translation table structure to ARM32, allowing access to 40-bit physical addresses (up to 1 TB). The descriptor format is nearly identical to ARM64's long-descriptor format — both use 64-bit entries, AttrIndx for memory types, AP for permissions, SH for shareability, nG for ASID-tagged entries. ARM64 essentially standardized and extended the LPAE format, adding PXN (privileged XN), Contiguous hint, hardware AF/DBM, larger VA range (48 vs 32 bits), and removed the complex TEX+C+B encoding in favor of pure MAIR_EL1 indexing.

**Q3: What is the practical impact of 8-byte PTEs on kernel memory usage?**

For a 4-level, 4KB page table, a fully-populated PTE table uses 512 × 8 bytes = 4KB. With 4-byte entries (ARM32 short descriptor), the L2 table was 256 × 4 bytes = 1KB. So ARM64 uses 4× more memory per leaf-level table. However, ARM64's 4-level structure and 512-entry tables mean that page tables are very sparse in practice — most PMD/PUD/PGD entries are invalid (zeroed). A typical process mapping 100MB uses: 1 PGD (4KB) + 1 PUD (4KB) + a few PMDs (4KB each) + PTE tables only for actually-mapped ranges. In practice, the larger entries are offset by the efficiency of PMD-level block mappings (eliminating entire PTE tables for 2MB regions).

---

## 9. Quick Reference

| Feature | ARM32 Short | ARM32 LPAE | ARM64 |
|---|---|---|---|
| Entry size | 4 bytes | 8 bytes | 8 bytes |
| Max PA bits | 32 | 40 | 48/52 |
| Max VA bits | 32 | 32 | 48/52 |
| Levels | 2 | 3 | 2-5 |
| Entries/table | 256-1024 | 512 | 512 |
| Table size | 1-4 KB | 4 KB | 4 KB |
| HW AF | No | No | Yes (v8.1) |
| HW Dirty | No | No | Yes (v8.1) |
| Cont hint | No | No | Yes |
| AttrIndx | No (TEX+C+B) | Yes | Yes |
| Hierarchical attrs | No | No | Yes |
