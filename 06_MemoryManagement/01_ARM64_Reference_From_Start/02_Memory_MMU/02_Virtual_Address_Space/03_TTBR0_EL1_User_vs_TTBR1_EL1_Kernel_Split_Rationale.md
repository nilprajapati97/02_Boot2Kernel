# TTBR0_EL1 (User) vs TTBR1_EL1 (Kernel): Split Rationale

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Concept Foundation

ARM64 has two separate translation table base registers for EL1:

- **TTBR0_EL1**: Points to the user-space page table root. Translates VAs where bit[VA_BITS-1] = 0 (upper bits = 0x0000...).
- **TTBR1_EL1**: Points to the kernel-space page table root. Translates VAs where bit[VA_BITS-1] = 1 (upper bits = 0xFFFF...).

This split is fundamentally different from ARM32, which used a single page table (TTBR0) for all VA ranges. The dual-TTBR design solves critical problems in OS design.

**Why two TTBRs?**

1. **Process switch efficiency**: When switching processes, only TTBR0_EL1 needs updating (user space changes). TTBR1_EL1 remains constant (kernel space is the same for all processes). Without the split, every context switch would require flushing all TLB entries.

2. **ASID efficiency**: The ASID is embedded in TTBR0_EL1. Non-global (nG) kernel entries use no ASID — they are shared across all processes. TLB entries for kernel code/data survive process switches without invalidation.

3. **Security isolation**: User code cannot accidentally (or maliciously) affect kernel page table traversal by manipulating its own TTBR0. The kernel page table is under TTBR1 which only EL1 can modify.

4. **KPTI compatibility**: Kernel Page Table Isolation (Meltdown mitigation) swaps TTBR1_EL1 between "full kernel" and "minimal kernel" page tables on each syscall entry/exit. This is only possible because kernel and user VAs are in separate TTBR domains.

---

## 2. TTBR Register Format

```
TTBR0_EL1 / TTBR1_EL1 format (64-bit):

Bits [63:48]  ASID   — Address Space ID (if AS=1 in TCR_EL1: 16-bit ASID)
                        (if AS=0 in TCR_EL1: bits[63:56] = ASID, 8-bit)
Bits [47:1]   BADDR  — Base address of translation table (bits[47:x] where x depends on granule)
Bit  [0]      CnP    — Common not Private (ARMv8.2: share TLB entries across CPUs in same domain)
```

### 4KB Granule, 48-bit VA Example

```
TTBR0_EL1 for process PID=42 with ASID=42:
  Bits[63:48] = 42     (ASID)
  Bits[47:12] = PGD base address >> 12  (page-aligned)
  Bit [0]     = CnP flag

Example value:
  0x002A_0000_1234_5000
       ^^^^              = ASID 0x002A = 42
            ^^^^^^^^^^^^  = PGD physical address 0x0000_1234_5000
```

---

## 3. VA Range Selection: Which TTBR Is Used?

The CPU selects the TTBR based on the **upper bits of the VA**:

```
For T0SZ = T1SZ = 16 (48-bit VA):

VA bits [63:48] = 0x0000 → Use TTBR0_EL1 (user space)
VA bits [63:48] = 0xFFFF → Use TTBR1_EL1 (kernel space)
VA bits [63:48] = anything else → Translation Fault (non-canonical)
```

This is a hardware comparison: if `VA[63:63-TxSZ+1]` is all-zeros → TTBR0; all-ones → TTBR1.

```c
// Pseudocode of hardware TTBR selection:
if (VA[63:48] == 0x0000) {
    use TTBR0_EL1, TCR_EL1.T0SZ, TG0
} else if (VA[63:48] == 0xFFFF) {
    use TTBR1_EL1, TCR_EL1.T1SZ, TG1
} else {
    Translation Fault
}
```

---

## 4. ASID — Address Space Identifier

The ASID is the key mechanism that avoids full TLB flushes on context switch:

```
TTBR0_EL1 for process A (ASID=5):
  [63:48] = 0x0005, [47:0] = PA of process A's PGD

TTBR0_EL1 for process B (ASID=7):
  [63:48] = 0x0007, [47:0] = PA of process B's PGD
```

TLB entries tagged with ASID=5 remain valid even after switching to process B. The TLB naturally contains both process A and process B translations. When process B accesses a VA, the TLB lookup matches on both VA and ASID. Process B cannot see process A's TLB entries because ASID doesn't match.

### ASID Allocation in Linux

```c
// arch/arm64/mm/context.c
static DEFINE_RAW_SPINLOCK(cpu_asid_lock);
static atomic64_t asid_generation;
static unsigned long *asid_map;  // Bitmap of allocated ASIDs

// ASID rollover: when all ASIDs are used, generation counter increments
// and all TLBs are flushed (generation change detected by checking
// TTBR0 generation tag on context switch)

void check_and_switch_context(struct mm_struct *mm)
{
    u64 asid = ASID(mm);
    u64 cpu_asid = ASID(this_cpu_read(active_asids));
    
    if (asid_generation(asid) != asid_generation(cpu_asid)) {
        // ASID is from old generation — need to allocate new ASID
        asid = new_context(mm);
    }
    cpu_switch_mm(mm->pgd, mm);
}
```

---

## 5. Context Switch: Only TTBR0 Changes

```c
// arch/arm64/mm/context.c
void cpu_switch_mm(pgd_t *pgd, struct mm_struct *mm)
{
    // Build new TTBR0 value with process ASID + page table base
    u64 ttbr0 = phys_to_ttbr(virt_to_phys(pgd));
    ttbr0 |= ASID(mm) << 48;  // Embed ASID in upper bits
    
    write_sysreg(ttbr0, ttbr0_el1);  // Only this changes!
    isb();
    // TTBR1_EL1 stays constant — kernel page tables unchanged
}
```

**TTBR1_EL1 never changes** during normal process context switches (only changes during KPTI syscall entry/exit).

---

## 6. KPTI — Kernel Page Table Isolation

KPTI (Meltdown mitigation, PTI) extends the TTBR split concept:

```
Two TTBR1_EL1 values per CPU:
  1. "Full" kernel PGD: all kernel text, data, vmalloc, etc.
     Used while executing in kernel mode (EL1)
  
  2. "Stub" kernel PGD: only syscall entry stubs + vectors
     Used while executing in user mode (EL0)
     Prevents Meltdown: user mode cannot speculatively access kernel memory

Context:
  User mode (EL0):     TTBR1 = stub (minimal) page table
  Syscall entry:       swap TTBR1 to full page table (on swapper_pg_dir)
  Syscall return:      swap TTBR1 back to stub page table
```

```c
// arch/arm64/kernel/entry.S — KPTI swapper
.macro kpti_install_ng_mappings
    // Called at syscall entry from EL0:
    mrs     x25, ttbr1_el1
    bic     x25, x25, #TTBR_ASID_MASK
    orr     x25, x25, #SWAPPER_DIR_OFFSET   // Switch to full kernel page tables
    msr     ttbr1_el1, x25
    isb
.endm
```

---

## 7. nG Bit — Non-Global Kernel Entries

Kernel page table entries have the `nG` bit = 0 by default, meaning they are **global** — not tagged with any ASID. Global TLB entries are shared across all ASIDs. This means:

- Kernel TLB entries survive process switches without invalidation.
- A kernel TLB hit is valid regardless of which process is current.

User entries have `nG=1` (non-global), meaning they are ASID-tagged. Only valid for the process with matching ASID.

---

## 8. CnP — Common not Private (ARMv8.2)

`TTBR.CnP=1` allows multiple CPUs in the same Inner Shareable domain to share TLB entries. When CnP=1, a TLB entry installed on CPU0 is visible on CPU1 without a separate TLB fill. This can reduce TLB miss rate in multi-core scenarios.

```c
// Linux enables CnP when hardware supports it:
// arch/arm64/mm/proc.S
#define TTBR_CNP_BIT  UL(1)
// Added to TTBR values when ARM64_HAS_CNP capability detected
```

---

## 9. Interview Questions & Answers

**Q1: Why does ARM64 have two TTBRs instead of one?**

One TTBR would require a single unified page table for both user and kernel space (as in ARM32). On process switch, the entire TLB would need to be flushed (since the new process has different user mappings). With two TTBRs, only TTBR0 changes on context switch; TTBR1 (kernel) stays constant. This allows kernel TLB entries to survive context switches. Additionally, the ASID (in TTBR0) allows user TLB entries to survive if the new process has a different ASID — no flush needed at all until ASIDs are exhausted.

**Q2: How does the CPU know which TTBR to use for a given VA?**

The CPU compares the upper bits of the VA against the T0SZ and T1SZ values in TCR_EL1. If the top `T0SZ` bits are all-zero, TTBR0 is used. If the top `T1SZ` bits are all-one (due to sign extension), TTBR1 is used. Any other pattern is non-canonical and causes a Translation Fault at level 0 of the walk.

**Q3: What happens to TTBR0_EL1 when the kernel is running with no user process?**

Kernel threads (kthreads) don't have a user address space. Linux sets TTBR0_EL1 to `reserved_pg_dir` for kernel threads — a special PGD where all entries are invalid. Any access to a user-space VA from a kernel thread causes an immediate Translation Fault. Alternatively, `EPD0=1` can be set in TCR_EL1 to disable TTBR0 walks entirely for kernel threads.

**Q4: Explain ASID exhaustion and what Linux does when all ASIDs are used.**

ARM64 supports either 256 (8-bit) or 65536 (16-bit) ASIDs. When all ASIDs are allocated (tracked in a bitmap), the generation counter increments. All CPUs then perform a TLB flush (`TLBI VMALLE1IS` or `TLBI ALLE1IS`). After the flush, the ASID bitmap is reset and allocation starts fresh. To detect that a process's ASID is from an old generation (and therefore invalid after the flush), Linux stores the generation number in the high bits of the ASID field and checks it on each context switch.

---

## 10. Quick Reference

| TTBR | Region | VA Range (48-bit) | Changes On |
|---|---|---|---|
| TTBR0_EL1 | User space | 0x0000...0000 to 0x0000FFFFFFFFFFFF | Process context switch |
| TTBR1_EL1 | Kernel space | 0xFFFF000000000000 to 0xFFFFFFFFFFFFFFFF | KPTI entry/exit only |

| Field | Location | Purpose |
|---|---|---|
| ASID | TTBR0_EL1[63:48] | Process identifier for TLB |
| Base address | TTBR0_EL1[47:0] | Physical address of PGD |
| CnP | TTBR.bit[0] | Share TLB across CPUs (ARMv8.2) |
| nG | PTE bit | Global (kernel) vs ASID-tagged (user) |
