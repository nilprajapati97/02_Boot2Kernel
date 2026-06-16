# HA / HD: Hardware Access Flag and Dirty Bit Management (ARMv8.1)

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm

---

## 1. Background: Software AF/DB Problem

Before ARMv8.1, ARM64 required **software** to manage the Access Flag (AF) and dirty bits:

```
Software AF management (TCR_EL1.HA=0):
  1. OS maps page with AF=0 in PTE
  2. CPU accesses the page → Access Flag Fault (ESR.DFSC=0b001011, Level 3)
  3. Fault handler (handle_pte_fault) runs:
     a. Sets AF=1 in PTE: pte_mkyoung(pte) sets bit[10]
     b. Writes updated PTE back to page table
     c. Returns from fault
  4. CPU retries the faulting instruction

Cost per first access:
  Exception entry: ~100 cycles
  Kernel fault handler: ~200-400 cycles
  PTE write + TLB shootdown: ~100 cycles
  Return from exception: ~100 cycles
  Total: ~500-1000 cycles per page first access

At scale (workload with thousands of pages):
  Memory-bound process touching 1M pages: 500M-1B wasted cycles
  For AF-based LRU reclaim (mark pages as old by clearing AF):
    kswapd must walk all PTEs and clear AF periodically
    Each clear triggers a new fault on next access → massive overhead
```

---

## 2. HA Bit: Hardware Access Flag (ARMv8.1)

```
TCR_EL1.HA (bit[39]):
  0 = Software manages AF (legacy mode, fault on AF=0 access)
  1 = Hardware automatically sets AF=1 on any access to a page with AF=0
      No Access Flag Fault generated
      Hardware updates PTE atomically in the page table memory

Requires: ID_AA64MMFR1_EL1.HAFDBS >= 1
  ID_AA64MMFR1_EL1.HAFDBS (bits[3:0]):
    0b0000 = HA/HD not supported
    0b0001 = HA supported (no HD)
    0b0010 = Both HA and HD supported

With HA=1:
  CPU hardware (MMU) reads PTE, sees AF=0
  Hardware writes AF=1 back to page table memory atomically
  No fault generated — access proceeds at full speed
  The PTE write uses the same cache attributes as normal PTW (IRGN/ORGN)
  Coherent: the write is Inner Shareable → all CPUs see updated PTE

Performance:
  No fault → no exception handler → no kernel involvement
  Cost: ~3-5 extra cycles for the hardware PTE write
  vs 500-1000 cycles for software AF fault
  → Approximately 100-300× speedup for AF management
```

---

## 3. HD Bit: Hardware Dirty Bit (ARMv8.1)

```
TCR_EL1.HD (bit[40]):
  0 = Software manages dirty state (fault on write to AP[0]=1 RO page)
  1 = Hardware automatically tracks dirty state
      Hardware sets PTE AP[0] = 0 (from read-only to read-write) on write
      Actually: hardware clears the "read-only" protection on first write

  HD requires HA=1 (both bits must be set together to enable dirty tracking)
  Requires: ID_AA64MMFR1_EL1.HAFDBS = 0b0010

DBM (Dirty Bit Management) descriptor bit[51]:
  When HD=1, PTE bit[51] = DBM (Dirty Bit Management enable) bit
  DBM=1: hardware tracks dirty state for this page
  DBM=0: hardware does NOT auto-set dirty (software dirty still works via AP)

How hardware dirty tracking works:
  Map page with AP[1:0] = 0b01 (write-capable in EL0) AND DBM=1 AND HA=1+HD=1
  
  First write by CPU:
    Hardware detects writable PTE (AP[0]=0 means writable in ARM64 dirty scheme)
    Hardware atomically: sets AP[0]=1 in PTE (marks as "hardware dirty")
    No fault generated
    
  Dirty detection:
    pte_dirty(): checks AP[0] in PTE → AP[0]=1 means dirty
    
  Note: ARM64 uses AP[0] inverted vs AP[1]:
    AP[0] in DBM context: 0=writable (not yet dirty tracked), 1=written (dirty)
    This is the opposite of what AP[0] means in standard AP encoding!
    When HD=1 and DBM=1: hardware writes AP[0]=1 on first store to the page.
```

---

## 4. ARM64 DBM PTE Layout

```
Standard PTE AP encoding (without DBM):
  AP[1:0] bits[7:6]:
    0b00 = EL1 RW, EL0 no access
    0b01 = EL1 RW, EL0 RW
    0b10 = EL1 RO, EL0 no access
    0b11 = EL1 RO, EL0 RO

With DBM (bit[51]=1) and HD=1:
  AP[0] (bit[6]) has a special meaning:
    AP[0]=0 = Page is writable (hardware dirty tracking active)
    AP[0]=1 = Page has been written (hardware set this bit = dirty)
  
  pte_mkwrite() with DBM: sets DBM=1, AP[0]=0, AP[1]=1 (EL0+EL1 writable)
  After first write: hardware sets AP[0]=1 → pte_dirty() returns true
  
  pte_mkyoung() (access flag): sets AF=1 (bit[10])
  pte_mkold():  clears AF=0 → next access updates AF again (or HW sets it)

Interaction:
  HA=1: HW sets AF=1 on access
  HD=1: HW sets AP[0]=1 on write (when DBM=1)
  Both together: OS gets accurate AF and dirty state without any faults
```

---

## 5. Linux Kernel Implementation

```c
// arch/arm64/include/asm/pgtable.h

// Check if hardware AF/DBM is available:
static inline bool cpu_has_hw_af(void)
{
    u64 mmfr1 = read_sysreg_s(SYS_ID_AA64MMFR1_EL1);
    return cpuid_feature_extract_unsigned_field(mmfr1,
                ID_AA64MMFR1_HAFDBS_SHIFT) != 0;
}

// Enable HA/HD in TCR_EL1 at boot:
// arch/arm64/kernel/cpufeature.c
static void cpu_enable_hw_dbm(struct arm64_cpu_capabilities const *cap)
{
    u64 tcr = read_sysreg(tcr_el1);
    tcr |= TCR_HA;   // bit[39]
    tcr |= TCR_HD;   // bit[40]
    write_sysreg(tcr, tcr_el1);
    isb();
}

// This is done via alternative patching:
// arch/arm64/include/asm/cpucaps.h
#define ARM64_HW_AFDBM  34  // CPU capability index

// In kernel .c files, uses:
// alternative_if ARM64_HW_AFDBM
//     ... code to use HW AF/DB ...
// alternative_else
//     ... software AF/DB path ...
// alternative_endif

// pte_mkyoung: sets AF (works for both HW and SW modes)
static inline pte_t pte_mkyoung(pte_t pte) {
    return pte_set_bit(pte, PTE_AF);  // PTE_AF = BIT(10)
}

// pte_mkwrite with DBM support:
static inline pte_t pte_mkwrite(pte_t pte) {
    pte = pte_clear_bit(pte, PTE_RDONLY);  // Clear read-only
    if (system_supports_hw_dbm())
        pte = pte_set_bit(pte, PTE_DBM);   // Set DBM=1 for HW dirty tracking
    return pte;
}

// pte_dirty: check if page has been written
static inline int pte_dirty(pte_t pte) {
    return !!(pte_val(pte) & PTE_DIRTY);
    // PTE_DIRTY = BIT(55) in SW mode, or derived from AP[0] in HW mode
}
```

---

## 6. Performance Impact

```
Benchmark comparison (kernel page reclaim workload):

Without HA/HD (ARMv8.0, software only):
  Access Flag Fault rate: 100K-1M faults/sec per core
  Fault handler cost: 500-1000 cycles each
  Dirty tracking: Permission Fault on write (1 fault per page per "clean" period)
  kswapd overhead: significant — must walk all PTEs to clear AF

With HA/HD (ARMv8.1):
  Access Flag Faults: eliminated (hardware does it silently)
  Dirty tracking Faults: eliminated for DBM=1 pages
  kswapd still walks PTEs to check AF/dirty state, but no faults
  
Measured improvement in database workloads:
  Read-heavy (AF dominated): 15-30% throughput improvement
  Write-dirty-heavy (DBM dominated): up to 3× improvement
  Cold start workloads (many page first-accesses): 2-10× improvement

Note: kswapd still needs to CLEAR AF periodically for LRU aging.
  Clearing AF (setting AF=0) still requires a TLB shootdown + TLBI.
  HA/HD doesn't help with the "clear" path, only the "set" path.
```

---

## 7. Interview Questions & Answers

**Q1: Explain TCR_EL1.HA and TCR_EL1.HD, and what ARMv8 version introduced them.**

`HA` (bit[39]) enables **Hardware Access Flag** management. With `HA=1`, the MMU hardware automatically sets `AF=1` in the PTE when a page is first accessed, eliminating the Access Flag Fault that would otherwise trap into the kernel. `HD` (bit[40]) enables **Hardware Dirty Bit** management. With `HD=1` (requires `HA=1`), the hardware atomically sets `AP[0]=1` in a DBM-enabled PTE (`bit[51]=1`) when a writable page is first written, tracking dirty state without Permission Faults. Both were introduced in **ARMv8.1**. Detection: `ID_AA64MMFR1_EL1.HAFDBS = 0b0010` for both, `= 0b0001` for HA only.

**Q2: Why does HD require HA to also be enabled?**

HD (dirty bit hardware management) is architecturally dependent on HA (access flag hardware management) because: (1) Hardware dirty tracking is conceptually "write access tracking" which is a superset of "read access tracking" (HA). Hardware that can silently modify PTE bits on a write must also be able to silently modify them on a read (otherwise you'd get an AF fault from the HA path even with HD enabled). (2) ARM designed them as a progression: HA gives you no-fault access flag; HD additionally gives no-fault dirty tracking. Enabling HD without HA would mean: hardware sets dirty bit on writes (no fault), but hardware still generates AF faults on reads — inconsistent fault behavior. ARM mandates `HA=1` as a prerequisite for `HD=1`.

---

## 8. Quick Reference

| Field | Bit | Function | ARMv version | Detection |
|---|---|---|---|---|
| HA | [39] | Hardware AF set on access | ARMv8.1 | ID_AA64MMFR1.HAFDBS ≥ 1 |
| HD | [40] | Hardware dirty on write | ARMv8.1 | ID_AA64MMFR1.HAFDBS = 2 |
| DBM | PTE[51] | Enable HW dirty for this page | ARMv8.1 | With HA+HD |

| State | HA=0 | HA=1 |
|---|---|---|
| AF=0 access | Fault → SW sets AF | HW sets AF, no fault |
| Dirty tracking | Permission fault | HW sets AP[0], no fault (if HD+DBM) |
| Cost | 500–1000 cycles | ~3–5 cycles |
