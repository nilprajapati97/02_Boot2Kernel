# DBM: Dirty Bit Management (ARMv8.1 Hardware Tracking)

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Concept Foundation

The **Dirty Bit** indicates whether a page has been modified (written to) since it was last loaded from backing storage. This is critical for:
- **Page reclaim**: Only dirty pages need to be written back to disk/swap before freeing
- **Copy-on-Write (CoW)**: Detect first write to a shared page
- **mmap file sync**: Determine which file-backed pages need `msync`/writeback
- **Checkpoint/Restart**: Detect modified pages for incremental checkpoint

Without hardware dirty tracking, the OS must simulate it:
1. Map page as **read-only**
2. First write → **Permission Fault** → OS marks page dirty, remaps as writable
3. This causes one fault per write-first access → performance overhead

**ARMv8.1 DBM** (Dirty Bit Management) eliminates this overhead by having hardware update the dirty state automatically.

---

## 2. ARM64 Dirty Bit Encoding

### Software Dirty (Pre-ARMv8.1)

Before hardware DBM, ARM64 had no architectural dirty bit. Linux used a software approach:

```
Software dirty bit approach:
  1. Fresh mapping: AP = 0b01 (user RW) with AP[0]=1 set for "writable"
     BUT map as read-only initially: AP[0]=0 (hardware RO)
  
  2. First write → Permission Fault (DFSC = access permissions fault)
  
  3. Fault handler (do_wp_page / handle_pte_fault):
     - Sets software PTE_DIRTY bit in PTE[55] (SW bit)
     - Changes AP[0]=1 (writable) so future writes don't fault
  
  4. On reclaim: check PTE_DIRTY (SW bit 55) to determine if writeback needed

Software dirty bit location:
  bit[55] = PTE_DIRTY in arch/arm64/include/asm/pgtable-hwdef.h
```

### Hardware DBM (ARMv8.1)

```
AP bit encoding with DBM:

Without DBM:
  AP[7:6]: 
    00 = EL1 RW, EL0 no access
    01 = EL1+EL0 RW
    10 = EL1 RO, EL0 no access  
    11 = EL1+EL0 RO

With DBM (TCR_EL1.HD=1 or individual DBM=1 in descriptor):
  The hardware repurposes AP[0] (bit 6 in descriptor) as the "writable" indicator:
  
  DBM=1 in descriptor bit[51]:
    AP[0]=0 (bit 6=0): Page is WRITABLE (counterintuitive naming!)
      → On write: hardware SETS bit 6 (AP[0]=1) = "dirty marker"
      → Effectively: AP transitions from 0b?0 to 0b?1 on first write
      → No fault generated → write proceeds
      
    AP[0]=1 (bit 6=1): Page has been written (DIRTY)
      → AP[0] was cleared by software (indicating writable)
      → Hardware set it back to 1 (indicating dirty)
      
  Detecting dirty: compare AP[0] with expected state
    If we set AP[0]=0 (writable) and now AP[0]=1 → hardware dirtied it
```

---

## 3. DBM Descriptor Bit

```
Bit[51] in page/block descriptor = DBM (Dirty Bit Management)

DBM=0: Standard AP[0] behavior (software-managed write permission)
DBM=1: Hardware dirty tracking enabled for this entry
  → Hardware can modify AP[0] (bit 6) on write access
  → No Permission Fault on first write (hardware updates descriptor atomically)

This bit is also written as:
  PTE_DBM in Linux kernel (= 1UL << 51)
  
ID_AA64MMFR1_EL1.HAFDBS:
  0b0000 = No HW AF or DBM support
  0b0001 = HW AF only (TCR_EL1.HA supported)
  0b0010 = HW AF + HW Dirty Bit (TCR_EL1.HA + TCR_EL1.HD)
```

---

## 4. TCR_EL1.HD — Global DBM Enable

```c
// TCR_EL1 bits:
// Bit[40] = HA: Hardware Access Flag
// Bit[41] = HD: Hardware Dirty Bit (requires HA=1 first)

// When HD=1:
//   All descriptors with DBM=1 AND AP[0]=0 are treated as writable
//   Hardware sets AP[0]=1 on first write (dirty marker)
//   No Permission Fault generated

// Linux enabling HA+HD:
// arch/arm64/mm/proc.S

/*
 * If the hardware supports it, enable hardware Access Flag and Dirty
 * Bit management (HA and HD).
 */
alternative_if ARM64_HAS_CNP
    orr     tcr, tcr, #TCR_CNP
alternative_else_nop_endif
alternative_if ARM64_HAS_ADDRESS_AUTH
    orr     tcr, tcr, #(TCR_TBID0 | TCR_TBID1)
alternative_else_nop_endif
alternative_if ARM64_HW_AFDBM
    orr     tcr, tcr, #(TCR_HA | TCR_HD)
alternative_else_nop_endif
```

---

## 5. Linux DBM Implementation

### PTE Creation for Writable Pages

```c
// arch/arm64/include/asm/pgtable.h

// With hardware DBM:
static inline pte_t pte_mkwrite(pte_t pte, struct vm_area_struct *vma)
{
#ifdef CONFIG_ARM64_HW_AFDBM
    if (system_supports_hw_afdbm()) {
        // Set DBM=1 and AP[0]=0 (hardware-writable):
        pte = set_pte_bit(pte, __pgprot(PTE_DBM));
        pte = clear_pte_bit(pte, __pgprot(PTE_RDONLY));  // AP[0]=0 = writable
        return pte;
    }
#endif
    // Software path: set AP[0]=1 (and let write faults handle dirty)
    return clear_pte_bit(pte, __pgprot(PTE_RDONLY));
}
```

### Checking Dirty State

```c
// arch/arm64/include/asm/pgtable.h

static inline int pte_dirty(pte_t pte)
{
#ifdef CONFIG_ARM64_HW_AFDBM
    if (system_supports_hw_afdbm()) {
        // Dirty: DBM=1 AND AP[0]=1 (hardware wrote it → set AP[0] from 0 to 1)
        return (pte_val(pte) & PTE_DBM) && !(pte_val(pte) & PTE_RDONLY);
        // Note: PTE_RDONLY = AP[0]; AP[0]=0 means dirty in DBM mode
        // (hardware sets AP[0]=1 on write, which CLEARS writable permission)
        // Actually: when HW writes, it changes descriptor to mark dirty
        // The exact logic depends on the specific implementation
    }
#endif
    return !!(pte_val(pte) & PTE_DIRTY);  // Software bit
}

// pte_mkclean: clear dirty for a clean-writable page (e.g., after writeback)
static inline pte_t pte_mkclean(pte_t pte)
{
#ifdef CONFIG_ARM64_HW_AFDBM
    if (system_supports_hw_afdbm()) {
        // Reset AP[0]=0 (hardware-writable, hardware will set dirty again)
        return clear_pte_bit(pte, __pgprot(PTE_RDONLY));
    }
#endif
    return clear_pte_bit(pte, __pgprot(PTE_DIRTY));
}
```

---

## 6. Copy-on-Write and DBM

CoW is the most important use case for dirty tracking:

```
CoW scenario (fork + write):
1. Parent maps page: AP = RW (user RW), nG=1, AF=1
2. fork(): child shares same physical page
   → Both parent and child: AP[0]=0 (read-only) for CoW detection
   → PTE_DIRTY cleared (clean shared page)

3. Child writes to page → Permission Fault (because AP[0]=0 = read-only)

4. do_wp_page() in mm/memory.c:
   a. Allocate new physical page
   b. Copy content from shared page
   c. Map new page in child's PTE: AP = RW, DBM=1, AF=1
   d. Now child has a private copy

5. Next write to child's private page:
   Without DBM: Permission Fault again (page was mapped RO for dirty tracking)
   With DBM: Hardware sets AP[0]=1 atomically → no fault → write succeeds

Performance benefit: After CoW split, subsequent writes to the private page
  do NOT generate faults → application runs at full speed
```

---

## 7. DBM vs Software Dirty — Performance Comparison

```
Workload: write-intensive, accessing 100,000 fresh pages

Software dirty tracking:
  100,000 pages × (Permission Fault cost)
  Permission Fault: ~1000-2000 cycles (exception entry, handler, ERET)
  Total: ~100,000 × 1500 = 150,000,000 cycles ≈ 0.1 seconds @ 1.5 GHz

Hardware DBM:
  0 faults (hardware updates AP atomically)
  PTW walk overhead: ~3-5 cycles per write (additional PTW write to set AP bit)
  Total: ~100,000 × 5 = 500,000 cycles ≈ 0.0003 seconds
  
Speedup: ~300× for dirty-tracking-heavy workloads
(e.g., databases, JIT compilers, write-heavy mmap files)
```

---

## 8. Hardware Atomicity of DBM Updates

ARM64 guarantees that hardware AF and DBM updates are atomic:

```
From ARM Architecture Reference Manual:
  "When hardware updates of translation table entries are enabled:
   - The update is performed as if it were a single-copy-atomic write
   - The update is visible to all observers
   - No DSB/ISB required by software to observe the update
   - Updates to AF and AP bits are made with an exclusive store sequence internally"

This means:
  - No race condition between CPU A writing to a page and CPU B checking dirty
  - The AP[0] change is visible to all CPUs in the shareability domain
  - Software can safely read AP[0] after a DSB to check dirty state
```

---

## 9. Interview Questions & Answers

**Q1: Without hardware DBM, how does Linux track whether a page has been written to?**

Linux uses a software dirty simulation: when a page is mapped, it is initially mapped as read-only (`AP[0]=1` = read-only, even if the VMA has `PROT_WRITE`). On the first write, the hardware generates a Permission Fault. The fault handler `do_wp_page()` or `handle_pte_fault()` marks the page dirty by setting the software `PTE_DIRTY` bit in `pteval_t[55]` and then remaps the page as writable (`AP[0]=0`). Subsequent writes don't fault. On reclaim, the kernel checks `pte_dirty()` which reads `PTE_DIRTY` bit to decide if writeback to disk is needed.

**Q2: What is the relationship between bit[51] (DBM) and bit[6] (AP[0]) in ARMv8.1+ dirty tracking?**

`DBM=1` in bit[51] enables hardware dirty management for that descriptor. When `DBM=1` is set, the meaning of `AP[0]` (bit[6]) changes: `AP[0]=0` means "page is writable" (the hardware is authorized to write to the page). On first write, the hardware atomically transitions `AP[0]` from 0 to 1 — this transition is the "dirty" marker. Reading `AP[0]=1` in a descriptor with `DBM=1` means "page was written since last cleaned." Linux's `pte_dirty()` checks for `DBM=1 && AP[0]=0` in the read-back descriptor (the hardware has set `AP[0]=1` on the dirty page, so the absence of `AP[0]=1` on a DBM-enabled page means clean, and presence means dirty).

**Q3: How does hardware DBM benefit applications that do heavy CoW workloads like databases?**

Database processes (e.g., PostgreSQL) use `fork()` to create child processes for query execution. Each `fork()` creates copy-on-write mappings. Without DBM, every page write after CoW requires: (1) a Permission Fault (because the page was mapped RO for dirty tracking), (2) the kernel remapping it RW. With DBM, after the CoW split (which still causes one fault to allocate the private page), subsequent writes to the private page proceed without faults — the hardware atomically updates `AP[0]` to mark dirty. For a PostgreSQL checkpoint with millions of dirty pages, this eliminates millions of Permission Faults per checkpoint cycle, reducing checkpoint time and improving transaction throughput.

---

## 10. Quick Reference

| Feature | Pre-ARMv8.1 | ARMv8.1+ with HA/HD |
|---|---|---|
| AF tracking | Software (AF faults) | Hardware (TCR_EL1.HA=1) |
| Dirty tracking | Software (Permission Faults) | Hardware (TCR_EL1.HD=1, DBM bit) |
| Dirty fault cost | ~1500 cycles/page | 0 cycles (no fault) |
| PTE DBM bit | bit[51] unused | bit[51]=1 enables per-entry DBM |
| AP[0] meaning | Write permission | Dirty indicator (when DBM=1) |
| Detection | `pte_val & PTE_DIRTY` (bit 55) | `pte_val & PTE_RDONLY` inverted |
| Linux config | Default | `ARM64_HW_AFDBM` |
| Kernel check | N/A | `ID_AA64MMFR1_EL1.HAFDBS >= 2` |
