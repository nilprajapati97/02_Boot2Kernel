# Page Table Walk: Hardware PTW, Cache Interactions, TWC

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Concept Foundation

Every TLB miss on ARM64 triggers a **hardware Page Table Walk (PTW)**. The PTW is performed by dedicated hardware in the CPU (the Translation Table Walker), completely transparently to software. Understanding the PTW mechanism is critical for:
- Optimizing memory access latency (PTW cache hit rates)
- Debugging Translation Faults and Permission Faults
- Understanding why page table memory must have specific cacheability attributes
- Sizing TLB and PTW cache requirements for performance tuning

---

## 2. PTW: The Full Walk Sequence

### Trigger

```
CPU issues VA access → TLB lookup → miss → PTW begins

Conditions for TLB miss:
  1. First access to a VA (entry never loaded)
  2. TLB eviction (entry replaced by new entry)
  3. TLBI instruction executed (entry explicitly invalidated)
  4. ASID change (old ASID entries are no longer valid)
  5. Page table modification (software updates PTE, must TLBI)
```

### Walk Sequence (4KB, 48-bit VA)

```
PTW hardware reads TCR_EL1 to determine:
  - Which TTBR to use (VA sign bit → TTBR0 or TTBR1)
  - Walk start level (based on TxSZ / VA width)
  - Granule size (TG0/TG1)
  - Walk cache attributes (IRGN/ORGN/SH in TCR_EL1)

Step 1: Determine base address
  VA[63:48] = 0x0000 → TTBR0_EL1.BADDR
  VA[63:48] = 0xFFFF → TTBR1_EL1.BADDR

Step 2: L0 (PGD) read
  Address = TTBR.BADDR | (VA[47:39] << 3)  // index × 8 bytes
  Memory read → L0 descriptor
  Check: valid? block? table?

Step 3: L1 (PUD) read
  Address = L0.NextLevelAddr | (VA[38:30] << 3)
  Memory read → L1 descriptor
  Check: block descriptor (1GB)? table?

Step 4: L2 (PMD) read
  Address = L1.NextLevelAddr | (VA[29:21] << 3)
  Memory read → L2 descriptor
  Check: block descriptor (2MB)? table?

Step 5: L3 (PTE) read
  Address = L2.NextLevelAddr | (VA[20:12] << 3)
  Memory read → L3 descriptor (page descriptor, bits[1:0]=0b11)

Step 6: Compute PA
  PA = L3.OA[47:12] << 12 | VA[11:0]

Step 7: Attribute check
  - Permissions OK? (AP, PXN, UXN vs access type)
  - AF set? (if not → AF fault instead of filling TLB)
  - Memory type valid?

Step 8: Fill TLB
  TLB entry: {VA[47:12], ASID if nG=1} → {PA[47:12], attributes}
  
Step 9: Re-issue original memory access
  Now hits TLB → translated to PA → L1/L2/L3 cache lookup
```

---

## 3. PTW Cache (Translation Table Walk Cache)

To avoid 4 expensive DRAM reads per TLB miss, ARM64 implements a **PTW Cache** (also called Translation Table Walk Cache or Intermediate Physical Address Cache):

```
PTW Cache levels:
  L1 TWC (Translation Walk Cache): Caches recent L0/L1/L2 intermediate table addresses
  L2 TWC: Larger, caches more intermediate entries

PTW Cache hit scenarios:
  VA = 0x0000_7FFF_12345678 (first access to this exact page)
  Previous access was to:  0x0000_7FFF_12349ABC (in same 2MB range)
  
  PTW Cache hit at L2 (PMD level):
    L0 entry: cached → no DRAM read
    L1 entry: cached → no DRAM read
    L2 entry: cached → no DRAM read
    L3 entry: MISS  → DRAM read (or cache hit if PTE is warm)
    
  Only 1 memory access instead of 4!
  
  Hit rate in practice:
    Hot kernel: L0/L1/L2 often cached → effective walk depth ≈ 1 read
    Cold startup: all 4 levels must be read → full walk cost
```

### PTW Cache Invalidation

```
When must PTW Cache be invalidated?

1. TTBR update: new process (new page tables)
   → Hardware automatically uses new TTBR; old TWC entries for old TTBR are stale
   → Must execute TLBI (which also invalidates TWC) or hardware handles naturally
   
2. Table descriptor changed (e.g., PGD entry updated):
   → TLBI VMALLE1IS (flush all) or TLBI VAE1IS for the affected range
   → DSB ISH must precede TLBI; ISB must follow
   
3. Block-to-table break-before-make sequence (see section 5):
   → Required when splitting a huge page into small pages
   → Must invalidate old block mapping before installing new table entries

ARM64 requirement:
   DSB ISHST (data synchronization barrier, inner sharable, stores)
   TLBI...
   DSB ISH
   ISB
```

---

## 4. Page Table Memory Attributes

Page tables are normal memory but require specific cacheability for correct PTW behavior:

### Required Attributes

```c
// TCR_EL1 walk cacheability fields:
// IRGN0[9:8]:  Inner cache attributes for TTBR0 walks
// ORGN0[11:10]: Outer cache attributes for TTBR0 walks
// SH0[13:12]:   Shareability for TTBR0 walks
// IRGN1[25:24]: Inner cache attributes for TTBR1 walks
// ORGN1[27:26]: Outer cache attributes for TTBR1 walks
// SH1[29:28]:   Shareability for TTBR1 walks

// Encoding for cache attributes in TCR:
// 0b00 = Non-cacheable
// 0b01 = Write-Back, Read-Allocate, Write-Allocate (WB RA WA)
// 0b10 = Write-Through, Read-Allocate (WT RA)
// 0b11 = Write-Back, Read-Allocate (WB RA, no WA)

// Linux standard configuration (arch/arm64/kernel/head.S):
// IRGN=0b01, ORGN=0b01, SH=0b11 (Inner Shareable, WB RA WA)
// This means page tables are cached in both L1 and LLC (Inner Shareable)
// and writes are write-back

#define TCR_IRGN_WBWA    (UL(1) << 8)   // IRGN=0b01 → WB RA WA
#define TCR_ORGN_WBWA    (UL(1) << 10)  // ORGN=0b01 → WB RA WA
#define TCR_SHARED_INNER (UL(3) << 12)  // SH=0b11 → Inner Shareable
```

### Why Inner Shareable?

```
SMP correctness: On ARM64 SMP, if CPU A modifies a PTE, CPU B must see the
updated PTE when it performs a PTW.

If page tables were Non-Shareable:
  CPU A writes new PTE → CPU A's L1 cache only
  CPU B performs PTW → reads old PTE from its own L1 cache
  → CPU B uses stale translation → DATA CORRUPTION

With Inner Shareable page tables:
  CPU A writes new PTE → write propagates to Inner Shareable domain
  CPU B's PTW sees the updated PTE (after DSB ISH + TLBI sequence)
  → Correct behavior on SMP systems
```

---

## 5. Break-Before-Make Sequence

When changing a live page table entry (especially splitting a block into smaller pages), ARM64 requires a **Break-Before-Make (BBM)** sequence to avoid transient incorrect translations:

### The Problem

```
Scenario: Split 2MB block into 4KB pages
  Current PMD: block descriptor → PA=0x10000000 (2MB, RW)
  Want:        Table descriptor → PTE table with individual pages

If we just overwrite PMD with new Table descriptor:
  Some CPUs may cache old block mapping in TLB
  Other CPUs may use new table mapping
  → Inconsistent views: one CPU maps 0x200000 bytes, another maps 4KB
  → Potential security issue (read via old large mapping sees more than intended)
```

### Correct BBM Sequence

```
1. BREAK: Write an invalid descriptor to the PMD entry
   pmd_clear(pmdp);                    // Write 0 (invalid) to PMD

2. DSB ISH                            // Ensure "invalid" is visible SMP-wide
   dsb(ishst)

3. TLBI VAAE1IS + VA                  // Flush TLB entries for the old block range
   flush_tlb_range(vma, start, end)   // → TLBI VAE1IS × each 4KB page in range

4. DSB ISH                            // Wait for TLBI completion

5. MAKE: Write the new descriptor
   set_pmd(pmdp, new_pmd);            // Install Table descriptor
   
6. ISB                                // Ensure instruction fetch sees new mapping
```

Linux code:
```c
// arch/arm64/mm/huge_memory.c: pmdp_huge_clear_flush()
pmd_t pmdp_huge_clear_flush(struct vm_area_struct *vma,
                              unsigned long address, pmd_t *pmdp)
{
    pmd_t pmd = pmdp_huge_get_and_clear(vma->vm_mm, address, pmdp);
    flush_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
    return pmd;
}
// Sets PMD to 0 (BREAK), then flushes TLB (DSB+TLBI), then caller sets new PMD (MAKE)
```

---

## 6. PTW and Stage 2 Translation (Virtualization)

In virtualized environments (KVM), PTW is two-stage:

```
Stage 1: Guest virtual → Guest physical (IPA = Intermediate Physical Address)
  Uses TTBR0_EL1 / TTBR1_EL1 (guest OS page tables)
  
Stage 2: Guest physical (IPA) → Real physical (PA)
  Uses VTTBR_EL2 (hypervisor page tables, managed by KVM)

Combined walk:
  Guest VA → S1 PTW → IPA → S2 PTW → PA
  
  Total memory accesses (4KB, 48-bit, both stages):
    S1: 4 reads (L0/L1/L2/L3)
    Each S1 table address itself needs S2 translation:
      Each of the 4 S1 reads = 4 S2 walks × 4 reads = 16 reads
    Final PA from S2: 4 reads
    Total: 4 S1 reads + (4 × 4) S2 lookups + 1 final access
         = 4 + 16 + 1 = 21 memory accesses per TLB miss!
    
  → TLBs and TWC are CRITICAL for virtualization performance
  → EL2 has its own TLB (combined S1+S2 cached)
  → IPA walk cache caches Stage 2 translations for intermediate PAs
```

---

## 7. PTW and the DSB/TLBI Requirement

The ARM64 memory model requires explicit synchronization around page table modifications:

```c
// Protocol for modifying a PTE (e.g., updating permissions):
// 1. Modify page table (store to PTE memory)
pte_t old = *ptep;
pte_t new_pte = pte_wrprotect(old);
WRITE_ONCE(*ptep, new_pte);

// 2. Broadcast: ensure all CPUs see the write
// (Inner Shareable page tables propagate automatically,
//  but need barrier before TLBI)
dsb(ishst);     // Data Synchronization Barrier, Inner Shareable, stores

// 3. Invalidate TLB entry
__tlbi(vae1is, __pa(addr) >> PAGE_SHIFT | ((u64)asid << 48));

// 4. Ensure TLBI completion before anyone reloads the entry
dsb(ish);       // Data Synchronization Barrier, Inner Shareable

// 5. Ensure subsequent instruction fetch sees updated mappings
isb();          // Instruction Synchronization Barrier (if code pages changed)

// Why DSB before TLBI?
//   TLBI instruction requires the page table write to be
//   "globally observed" before it takes effect.
//   Without DSB, TLBI may execute before the store is visible to other CPUs.

// Why DSB after TLBI?
//   TLBI is a broadcasted operation; must wait for it to complete
//   before assuming stale TLB entries are gone.
```

---

## 8. Interview Questions & Answers

**Q1: How many memory accesses does a full 4-level ARM64 page table walk require, and how does the PTW cache reduce this?**

A full walk requires 4 memory accesses: one each for L0 (PGD), L1 (PUD), L2 (PMD), and L3 (PTE). Plus the final data access = 5 total per TLB miss. The PTW cache (Translation Walk Cache) caches intermediate table addresses. If L0-L2 are cached, only the L3 read is needed — reducing 4 reads to 1. In practice, hot kernel regions have PTW cache hit rates of 90%+, so effective walk cost is often just 1-2 DRAM accesses. In a virtualized environment, worst case is 21 memory accesses per TLB miss (4 S1 walks × 4 S2 subwalks + 1 final), making the combined S1+S2 TLB critical for KVM performance.

**Q2: Why must page table memory be Inner Shareable on ARM64 SMP?**

In ARM64 SMP, if CPU A writes a new PTE and CPU B performs a page table walk for the same VA, CPU B must observe CPU A's write. This requires the write to be visible in the shared cache domain. If page tables were Non-Shareable, CPU A's PTE write would only be in CPU A's L1 cache. CPU B's PTW hardware would read from its own cache or DRAM, potentially getting a stale value. Inner Shareable ensures that PTE writes propagate to the Inner Shareable domain (all CPUs in the same cluster) before the subsequent `DSB ISHST + TLBI` sequence. This is why `TCR_EL1.IRGN/ORGN/SH` must be configured for Inner Shareable (SH=0b11, IRGN=0b01, ORGN=0b01) in all Linux ARM64 configurations.

**Q3: What is the Break-Before-Make sequence and when is it required?**

BBM is required when changing a page table entry to a "different" mapping (different PA, or splitting a large block into smaller pages). The sequence is: (1) Write an invalid descriptor to the entry (Break); (2) `DSB ISH` to ensure the invalid entry is visible SMP-wide; (3) `TLBI` to invalidate the old TLB entry; (4) `DSB ISH` to wait for TLBI completion; (5) Write the new descriptor (Make). Without BBM, there's a window where different CPUs may see inconsistent mappings — one sees the old block, another sees the new table pointer. This could allow a CPU to walk through the old large block descriptor to access memory that was intended to be broken into separate, differently-protected small pages.

---

## 9. Quick Reference

| PTW Step | Memory Reads | PTW Cache? |
|---|---|---|
| L0 (PGD) | 1 | Usually cached (very stable) |
| L1 (PUD) | 1 | Often cached (stable per 1GB region) |
| L2 (PMD) | 1 | Moderately cached (per 2MB region) |
| L3 (PTE) | 1 | Less cached (per 4KB page) |
| Final data | 1 | L1/L2/L3 cache |
| **Total worst** | **5** | **No cache** |
| **Total typical** | **1-2** | **L2/L3 PTW hits** |

| Page Table Attribute | Required Value | Why |
|---|---|---|
| TCR_EL1.SH0/SH1 | 0b11 (Inner Shareable) | SMP coherency |
| TCR_EL1.IRGN0/IRGN1 | 0b01 (WB RA WA) | Performance |
| TCR_EL1.ORGN0/ORGN1 | 0b01 (WB RA WA) | Performance |

| BBM Step | Operation | Purpose |
|---|---|---|
| 1 | Write invalid descriptor | Break old mapping |
| 2 | DSB ISHST | Ensure break is visible |
| 3 | TLBI | Flush old TLB entries |
| 4 | DSB ISH | Wait for TLBI completion |
| 5 | Write new descriptor | Make new mapping |
