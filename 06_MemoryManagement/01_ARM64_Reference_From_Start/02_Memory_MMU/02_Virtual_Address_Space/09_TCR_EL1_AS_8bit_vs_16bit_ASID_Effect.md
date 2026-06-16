# TCR_EL1.AS: 8-bit vs 16-bit ASID and Its Effect

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

The ASID (Address Space Identifier) is a hardware tag embedded in TLB entries and in the TTBR0_EL1 register. It identifies the address space (process) that owns a particular TLB entry.

Without ASIDs, every context switch would require a complete TLB flush — all cached address translations would be invalidated, and the new process would suffer TLB misses for every memory access until the TLB is repopulated.

With ASIDs:
- TLB entries survive context switches
- Each entry is tagged with the ASID of the process that created it
- TLB lookups match on (VA, ASID) pair
- Process A cannot accidentally use Process B's TLB entries (different ASID)

**TCR_EL1.AS** controls whether the ASID field is 8 bits or 16 bits, which determines the total number of simultaneously usable ASIDs.

---

## 2. TCR_EL1.AS — ASID Size Field

```
TCR_EL1.AS (bit[36]):
  0 = 8-bit ASID  →  256 possible ASID values (0–255)
  1 = 16-bit ASID → 65536 possible ASID values (0–65535)
```

The ASID is stored in `TTBR0_EL1[63:48]`:
- 8-bit ASID:  `TTBR0_EL1[63:56]` = ASID (8 bits), bits[55:48] must be 0
- 16-bit ASID: `TTBR0_EL1[63:48]` = ASID (all 16 bits used)

```c
// arch/arm64/mm/context.c
// Linux prefers 16-bit ASID if hardware supports it:
static bool asid16_supported(void)
{
    return cpus_have_const_cap(ARM64_HAS_ASID16);
    // Checked from ID_AA64MMFR0_EL1.ASIDBits field:
    // 0b0000 = 8-bit supported
    // 0b0010 = 16-bit supported
}

// Set in TCR_EL1:
#define TCR_ASID16  (UL(1) << 36)
// Linux adds this if ARM64_HAS_ASID16 is detected
```

---

## 3. ASID in TTBR0_EL1

```
TTBR0_EL1 layout (16-bit ASID mode):

Bit 63                    48 47                              1 0
 ┌────────────────────────────┬────────────────────────────────┬───┐
 │     ASID [63:48]           │     Page table base [47:1]     │CnP│
 │     16 bits                │     47 bits                    │   │
 └────────────────────────────┴────────────────────────────────┴───┘

Example for process PID=1000, ASID=1000, PGD at PA=0x1234_5000:
  TTBR0_EL1 = 0x03E8_0000_0001_2345
                ^^^^ = ASID 0x03E8 = decimal 1000
                     ^^^^^^^^^^^^^^^^^ = PGD PA >> 1 (bit-shifted)
                                                    ^ = CnP=1 (if enabled)
```

---

## 4. How ASID Isolation Works in the TLB

Each TLB entry stores:
- VA (the virtual address tag)
- PA (the physical address result)  
- Page attributes (permissions, memory type)
- ASID (if nG=1, non-global) or "global" flag (if nG=0)
- VMID (if virtualization is active — `VTTBR_EL2.VMID`)

```
TLB lookup key:
  { VA[47:12], ASID } → { PA, attributes }

Process A (ASID=5) accesses VA 0x1000:
  TLB entry: { VA=0x1000, ASID=5 } → { PA=0x200000, RW, Normal-WB }

Process B (ASID=7) accesses VA 0x1000:
  TLB lookup: { VA=0x1000, ASID=7 } → MISS (different ASID)
  Page table walk for B's 0x1000 → fills new TLB entry
```

Both entries coexist in the TLB. No interference, no flush needed on switch.

---

## 5. ASID Allocation in Linux

```c
// arch/arm64/mm/context.c

#define NUM_USER_ASIDS   ASID_FIRST_VERSION  // 256 or 65536

static atomic64_t asid_generation;  // Current generation number
static unsigned long *asid_map;     // Bitmap: bit N set = ASID N in use

// ASID format stored in mm->context.id:
//   High bits: generation number
//   Low bits:  ASID value (8 or 16 bits)

// Context switch triggers ASID check:
void check_and_switch_context(struct mm_struct *mm)
{
    u64 asid, old_active_asid;
    
    asid = READ_ONCE(mm->context.id);
    
    if (!asid_gen_match(asid)) {
        // ASID from old generation — need new ASID
        raw_spin_lock(&cpu_asid_lock);
        asid = new_context(mm);  // Allocate new ASID
        WRITE_ONCE(mm->context.id, asid);
        raw_spin_unlock(&cpu_asid_lock);
    }
    
    // Install the ASID+PGD into TTBR0:
    cpu_switch_mm(mm->pgd, mm);
    
    // Update active ASID tracking:
    this_cpu_write(active_asids, asid);
}
```

### ASID Generation Rollover

When all ASIDs are used:
```c
static u64 new_context(struct mm_struct *mm)
{
    // Check if ASID still valid in current generation:
    if (asid_gen_match(ASID(mm->context.id))) {
        // Just reuse existing ASID
        return mm->context.id;
    }
    
    // Check bitmap for free ASID:
    asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx);
    
    if (asid == NUM_USER_ASIDS) {
        // All ASIDs exhausted! Roll over generation.
        generation = atomic64_add_return_relaxed(ASID_FIRST_VERSION,
                                                  &asid_generation);
        // Flush all TLBs on all CPUs:
        flush_context();   // TLBI VMALLE1IS
        // Reset bitmap and start fresh:
        bitmap_zero(asid_map, NUM_USER_ASIDS);
        asid = 1;  // ASID 0 reserved for kernel
    }
    
    __set_bit(asid, asid_map);
    return asid | generation;
}
```

The generation number is stored in the high bits of `mm->context.id`. When the kernel checks the current CPU's ASID against the mm's stored ASID, a mismatch in the generation triggers ASID reallocation.

---

## 6. 8-bit vs 16-bit ASID: Performance Impact

### 8-bit ASID (256 processes)

On a system with many processes:
- After 256 processes have unique ASIDs, the generation rolls over
- All TLBs are flushed (`TLBI VMALLE1IS`)
- Next 256 processes get fresh ASIDs, but TLB is cold (all misses)

On a busy server with 1000+ processes, ASID rollovers happen frequently → TLB thrashing → significant performance penalty.

### 16-bit ASID (65536 processes)

- 65536 processes can coexist with unique ASIDs
- Rollovers much less frequent on typical workloads
- Dramatically reduces TLB flush frequency
- Most modern ARM64 cores support 16-bit ASID

### Benchmark Impact

```
System with 500 processes, rapid context switching:
  8-bit ASID: ~3-5 ASID rollovers/second → 3-5 full TLB flushes/second
  16-bit ASID: 0 rollovers → no flush overhead
  
Performance difference: significant for workload with many processes
(e.g., web servers, container platforms)
```

---

## 7. ASID 0 — Reserved for Kernel

ASID 0 is reserved. It is never assigned to any user process. This ensures that kernel TLB entries (marked global, nG=0) are unambiguously distinguishable from user entries.

```c
// arch/arm64/mm/context.c
// Bitmap allocation starts from index 1:
#define ASID_FIRST_VERSION (UL(1) << ASID_BITS)
// ASID_BITS = 8 or 16 depending on hardware
// ASID 0 = reserved (never allocated to user processes)
```

---

## 8. SMP and ASID Management

On multi-core systems, each CPU maintains its own TLB. ASID management is per-CPU but coordinated:

```c
DEFINE_PER_CPU(atomic64_t, active_asids);

// When a new ASID is needed on CPU N:
// - Atomically check active_asids[N]
// - If generation matches → use current ASID
// - If mismatch → local CPU ASID is stale → reallocate

// TLB flush on rollover uses broadcast:
// TLBI VMALLE1IS: Invalidate all EL1 entries (Inner Shareable domain)
// This flushes all CPUs' TLBs simultaneously via cache coherence interconnect
```

---

## 9. Interview Questions & Answers

**Q1: What is an ASID and why is it important for performance?**

An ASID (Address Space Identifier) is a tag stored in TLB entries and `TTBR0_EL1` that identifies which process's address space a translation belongs to. Without ASIDs, context switches would require flushing the entire TLB — all cached translations become invalid. With ASIDs, TLB entries from process A (ASID=5) remain valid while process B (ASID=7) is running. TLB lookups match on `(VA, ASID)`, so processes never see each other's translations. This dramatically reduces TLB miss rate after context switches.

**Q2: What happens when all 256 (8-bit) ASIDs are exhausted?**

When the ASID bitmap is full (all 256 values allocated), Linux increments the generation counter and calls `flush_context()`, which executes `TLBI VMALLE1IS` — flushing all user TLB entries on all CPUs in the inner shareable domain. The bitmap is then reset. The next 256 context switches will allocate fresh ASIDs 1–255. After the flush, all processes suffer TLB cold starts. With 16-bit ASID, 65536 unique ASIDs are available, making rollovers far less frequent.

**Q3: How does Linux store the ASID for a process?**

The ASID is stored in `mm->context.id` — a 64-bit atomic value where the high bits carry the generation number and the low 8 or 16 bits carry the ASID value. When the kernel switches to a process, it extracts the ASID, checks it against the current generation, and if valid, embeds it in `TTBR0_EL1[63:48]` along with the process's PGD physical address. If the generation doesn't match, a new ASID is allocated first.

**Q4: Can two processes share the same ASID?**

Yes, temporarily — during a generation rollover, all processes get reallocated new ASIDs from a freshly cleared bitmap. Between rollovers, each process has a unique ASID. Within a single generation, no two processes share an ASID. If a process is scheduled on two different CPUs simultaneously (for different threads of the same process), they use the same `mm->context.id` ASID — which is correct because they share the same address space.

---

## 10. Quick Reference

| Parameter | 8-bit ASID | 16-bit ASID |
|---|---|---|
| TCR_EL1.AS | 0 | 1 |
| Number of ASIDs | 256 | 65536 |
| TTBR0[63:56] | 8-bit ASID | Upper 8 bits of 16-bit ASID |
| TTBR0[55:48] | 0 | Lower 8 bits of 16-bit ASID |
| Rollover frequency | Frequent (>256 processes) | Rare (>65536 processes) |
| ID register | ID_AA64MMFR0.ASIDBits=0b0000 | ID_AA64MMFR0.ASIDBits=0b0010 |

| ASID value | Usage |
|---|---|
| 0 | Reserved (kernel, never assigned) |
| 1–254 or 1–65534 | User processes |
| 255 or 65535 | Last user ASID before rollover |
