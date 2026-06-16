# 10.03 — AMD / x86_64 vs ARM Memory Architecture

> Sources: *AMD64 Architecture Programmer's Manual Vol 2 (System Programming)*; *Intel SDM Vol 3*; ARM ARM as referenced in earlier sections.

Comparison angle for an AMD interview — public material only.

---

## 1. Architectural Heritage

| Aspect | ARMv8-A | AMD64 / x86_64 |
|---|---|---|
| ISA family | RISC, fixed 4-byte instr (A64) | CISC, variable-length |
| Privilege levels | EL0..EL3 (+S/NS, Realm) | Ring 0..3 + SMM + VMX root/non-root |
| Memory model | Weak (other-multi-copy atomic) | TSO (Total Store Order) |
| Page table format | VMSAv8 — 4/16/64K granule, 3–4 levels | x86 — 4K base, PAE/long mode, 4-level (or 5-level LA57) |
| Translation registers | TTBR0/1 split | CR3 (single) |
| Cache protocols | Implementation: MESI/MOESI/CHI states | MESI/MOESI (AMD historically MOESI) |
| Coherency | Architectural — Inner/Outer shareable | Architectural — fully coherent within socket |

---

## 2. Page Tables

### x86_64 (Long Mode, 4-level)

```
CR3 → PML4 → PDPT → PD → PT → 4K page
       512    512   512  512
```

- 48-bit VA (canonical: bits 63:48 sign-extended from bit 47).
- LA57 extends to 5-level → 57-bit VA.
- Large pages: 2 MB (PD entry as leaf), 1 GB (PDPT entry as leaf).
- One pointer CR3 — kernel and user share the same address space; KPTI (Kernel Page Table Isolation, Meltdown mitigation) uses two CR3 values.

### ARMv8 VMSAv8 (4K granule, 4 levels)

```
TTBR0 (low VAs) → L0 → L1 → L2 → L3 → 4K page
TTBR1 (high VAs) → ... (separate root)
```

- 48-bit VA standard, 52-bit with FEAT_LVA.
- Hugepages: 2 MB (L2 block), 1 GB (L1 block) at 4K granule; 32 MB / 512 MB at 16K granule; 512 MB at 64K granule.
- **Two TTBRs** — kernel uses TTBR1 (high half), user TTBR0 (low half). No CR3 swap on syscall (until KPTI/EPD0).

---

## 3. PTE Bits Compared

| Concept | ARM PTE | x86 PTE |
|---|---|---|
| Valid | Bit 0 | Bit 0 (P) |
| Block/Page distinction | Bit 1 | Bit 7 (PS) at non-leaf |
| RW | AP[2] | Bit 1 (R/W) |
| User access | AP[1] | Bit 2 (U/S) |
| No-execute | UXN/PXN bits | Bit 63 (NX) |
| Access flag | AF bit (bit 10) | Bit 5 (A) |
| Dirty | DBM scheme | Bit 6 (D) |
| Cacheability | AttrIndx → MAIR | PWT/PCD/PAT bits → PAT MSR |
| Global | nG inverted (bit 11) | Bit 8 (G) |
| Shareability | SH[1:0] | implicit (always coherent) |

---

## 4. Memory Type Mechanisms

### x86 PAT (Page Attribute Table)

- IA32_PAT MSR holds 8 entries, each selecting one of: UC, WC, WT, WP, WB, UC-.
- PTE bits PWT/PCD/PAT (3 bits) index into PAT MSR.
- MTRRs (Memory Type Range Registers) provide a coarse fallback at physical-address ranges.

### ARM MAIR

- MAIR_EL1 holds 8 entries × 8 bits each.
- PTE field AttrIndx (3 bits) selects entry.
- Encodes Normal cacheability (inner+outer) or Device subtype (nGnRnE/nGnRE/nGRE/GRE).

Both designs are functionally similar — small MSR-based indirection from a few PTE bits.

---

## 5. Memory Ordering

### x86 TSO

Reorderings allowed:
- Older store → younger load (to **different** address) — store buffer.

Forbidden:
- Store → store reorder (stores leave in order).
- Load → load reorder (loads in order).
- Load → store reorder (load can't move past store).

`MFENCE` for full fence; `LFENCE` mostly for speculative load barriers (post-Spectre).

### ARMv8 weak

Most reorderings allowed; need DMB / LDAR / STLR. SB, MP outcomes possible without barriers.

### Implication

x86 code ported to ARM may need barriers added; ARM code on x86 has extra-strong ordering "for free" (often unnecessary barriers can be removed).

---

## 6. Atomics

| Feature | x86 | ARM |
|---|---|---|
| Basic atomic ops | `LOCK <op>` prefix | LDXR/STXR (load/store exclusive); FEAT_LSE adds LDADD/CAS/SWP single-instr |
| CAS | `LOCK CMPXCHG` | `CASAL` (FEAT_LSE) |
| 128-bit CAS | `CMPXCHG16B` | `CASPAL` (FEAT_LSE) |
| RMW semantics | Implicit full fence | Acquire/release variants via *A/*L suffix |

LSE atomics are critical for scalability on many-core ARM servers — LL/SC exclusive monitor doesn't scale beyond ~16 cores under contention.

---

## 7. Coherency Fabric

| Aspect | AMD (modern) | ARM (Neoverse) |
|---|---|---|
| Topology | Infinity Fabric, multi-chiplet IOD | CMN-700 mesh (CHI protocol) |
| Coherency states | MOESI variants | UC/UD/SC/SD/I + variants |
| NUMA | Per-CCD (chiplet) NUMA effects | Per-socket and per-quadrant |
| Last-level cache | L3 per CCX (32 MB Zen 4) | SLC via CMN home nodes |

---

## 8. Virtualization

| Aspect | AMD-V / SVM | ARM EL2 |
|---|---|---|
| Hypervisor mode | VMX root (Intel) / SVM (AMD) | EL2 |
| Nested paging | NPT (4-level extra PT) | Stage-2 (4-level extra) |
| IOMMU | AMD-Vi | ARM SMMU |
| Interrupt virt | APICv (Intel) / AVIC (AMD) | GICv3/v4 list registers |

Concepts are 1:1 with different naming. ARM Stage-2 ≈ AMD NPT.

---

## 9. Pitfalls (porting / interview)

1. **Assuming TSO** when writing ARM lock-free code — SB will bite you.
2. **Mapping `volatile` to memory ordering** — neither x86 nor ARM treat C `volatile` as a barrier; use explicit atomics.
3. **WC memory** on x86 has no exact ARM equivalent — closest is Normal NC + write-combining behavior of the bus, but semantics differ.
4. **CR3 swap latency** on x86 is significant; ARM benefits from two-TTBR split.
5. **LSE-disabled** kernel builds on Neoverse — performance regression vs LL/SC at scale.
6. **PAT MSR mismatch** across cores on x86 — multi-CPU consistency required at boot.

---

## 10. Interview Q&A (AMD-flavored)

**Q1. ARM vs AMD memory consistency model?**
ARM: weakly ordered (other-multi-copy-atomic with barriers). AMD/x86: TSO — only store→load reordering allowed.

**Q2. How does CR3 differ from TTBR0/TTBR1?**
x86 has one CR3 — kernel and user share VA space, switched per process. ARM splits user (TTBR0) and kernel (TTBR1) so no swap on kernel entry (pre-KPTI).

**Q3. PAT vs MAIR — compare.**
Both: small table of memory-attribute entries indexed by PTE bits. PAT has 8 entries × 3 bits encoding; MAIR has 8 × 8 bits with explicit Normal/Device encodings.

**Q4. Cache coherency state set?**
AMD MOESI; ARM CHI uses UC/UD/SC/SD/I (functionally MOESI-equivalent).

**Q5. NPT vs Stage 2?**
Same idea: hypervisor-controlled second translation from guest-physical to real-physical. NPT is x86 naming, Stage 2 is ARM.

**Q6. How do AMD and ARM handle TLB shootdown?**
AMD: software IPIs invoke `INVLPG` on each CPU. ARM: `TLBI ...IS` hardware-broadcasts within the Inner-Shareable domain — no IPI cost.

**Q7. Why are ARM atomics structured as LL/SC plus LSE?**
LL/SC (LDXR/STXR) is flexible but doesn't scale under contention. FEAT_LSE adds single-instruction atomics (LDADD, CAS, SWP) that scale better in the interconnect.

**Q8. Differences in IOMMU?**
AMD-Vi and ARM SMMU both translate device accesses with stage-1/stage-2 page tables; SMMUv3 added in-memory queues and SVA via PASID/ATS/PRI similar to AMD-Vi's PASID extensions.

---

## 11. Cross-refs

- [01.04 Weak memory model](../01_Memory_Model/04_Weakly_Ordered_Memory_Model.md)
- [06.04 Coherency vs consistency](../06_Memory_Barriers_Ordering/04_Coherency_vs_Consistency.md)
- [02.02 Translation regimes](../02_Virtual_Memory_VMSAv8/02_Translation_Regimes_and_ELs.md)
- [09.03 KVM/Xen](../09_Virtualization_and_Stage2/03_Hypervisor_Modes_KVM_Xen.md)
