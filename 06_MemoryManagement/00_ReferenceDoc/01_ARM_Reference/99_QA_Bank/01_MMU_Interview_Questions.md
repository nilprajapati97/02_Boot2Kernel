# 99.01 — MMU Interview Question Bank

> Curated MMU questions for 10-year ARM-systems interviews (NVIDIA, AMD, Qualcomm). Answers reference earlier sections.

---

## Foundation

**Q1. Explain VA → IPA → PA translation for a guest under a hypervisor.**

Two stages: Stage 1 (guest OS owns, TTBR0/1_EL1) translates Guest VA → IPA; Stage 2 (hypervisor owns, VTTBR_EL2) translates IPA → PA. Each Stage-1 PTE fetch is itself an IPA that must go through Stage 2 (nested walk). See [09.01](../09_Virtualization_and_Stage2/01_Two_Stage_Translation_Recap.md).

---

**Q2. What's the difference between TTBR0 and TTBR1?**

TTBR0 holds the base of the **low VA range** page tables (typically userspace, e.g. 0x0…0x0000_FFFF_FFFF_FFFF). TTBR1 holds the base of the **high VA range** (typically kernel, 0xFFFF…). TCR_EL1.{T0SZ,T1SZ} set the size of each region. The split avoids needing to swap roots on syscall entry (pre-KPTI). See [02.04](../02_Virtual_Memory_VMSAv8/04_VA_IPA_PA_Layout.md), [07.02](../07_System_Registers_Quickref/02_TTBR0_TTBR1_TCR.md).

---

**Q3. What translation granules does VMSAv8 support and what's the trade-off?**

4 KB, 16 KB, 64 KB. Larger granule → fewer page-table levels (shorter walk), better TLB coverage per entry, larger block-page sizes (huge pages). But more memory fragmentation and waste for small allocations. Linux uses 4 KB by default; Apple/iOS uses 16 KB. See [02.03](../02_Virtual_Memory_VMSAv8/03_Translation_Granules_4K_16K_64K.md).

---

**Q4. Walk me through a 4-level page walk for a 48-bit VA with 4 KB granule.**

```
VA bits: [47:39] L0 index | [38:30] L1 | [29:21] L2 | [20:12] L3 | [11:0] page offset
```

TTBR → L0 descriptor → L1 table → L1 descriptor → L2 table → L2 descriptor → L3 table → L3 descriptor (page leaf) → physical page. Each non-leaf is a Table descriptor (bit[1]=1); leaf is Page descriptor. Block descriptors can short-circuit at L1/L2 for huge pages. See [03.02](../03_Page_Tables_and_Translation/02_Multi_Level_Page_Walk.md).

---

**Q5. Block vs Page descriptor — what's the difference?**

Both are leaves of the walk. **Page** is always at the lowest level (L3 for 4K granule = 4 KB). **Block** is at L1 (1 GB block) or L2 (2 MB block) for 4K granule — represents a contiguous physical region without further table indirection. The Type field (bit[1]) distinguishes: 0 = Block, 1 = Table (at non-leaf levels) or Page (at leaf). See [03.03](../03_Page_Tables_and_Translation/03_Block_vs_Page_Mappings.md).

---

## Permissions and Attributes

**Q6. Decode `AP[2:1]` field.**

AP[2] = R/W (0 = read-write, 1 = read-only). AP[1] = EL0 access (0 = privileged-only, 1 = EL0 allowed). Combined with UXN/PXN for execute permission. See [03.06](../03_Page_Tables_and_Translation/06_Permission_Checks_AP_UXN_PXN.md).

---

**Q7. What's the difference between UXN and PXN?**

UXN = Unprivileged Execute Never (EL0 can't execute). PXN = Privileged Execute Never (EL1 can't execute). Kernel uses PXN on user pages so a stray kernel branch into userspace faults (mitigates SMEP-like attacks). Linux maps user pages with PXN=1.

---

**Q8. How are memory attributes (cacheability, type) encoded?**

PTE has a 3-bit `AttrIndx` field selecting one of 8 entries in `MAIR_EL1`. Each MAIR entry is 8 bits encoding Normal (Inner+Outer cacheability) or Device (subtype nGnRnE/nGnRE/nGRE/GRE). Indirection lets you change attributes globally without rewriting every PTE. See [01.03](../01_Memory_Model/03_MAIR_and_Attribute_Encoding.md), [07.03](../07_System_Registers_Quickref/03_MAIR_and_Attribute_Indirection.md).

---

**Q9. What's the Access Flag and how is it managed?**

AF=1 means the page has been accessed at least once. If software AF management (TCR.HA=0), HW faults on AF=0 access (AF fault) and OS sets AF=1. If HW AF management (TCR.HA=1, FEAT_HAFDBS), CPU sets AF automatically on first access. Used for LRU page replacement. See [03.05](../03_Page_Tables_and_Translation/05_Access_Flag_and_Dirty_State.md).

---

**Q10. How is the dirty bit implemented on ARM?**

ARM uses DBM (Dirty Bit Modifier): PTE has DBM=1 + AP[2]=1 (RO). On first write, HW sets AP[2]=0 (becomes RW) — that's the "dirty" indication. If TCR.HD=0, software does this via permission fault. With FEAT_HAFDBS (TCR.HD=1), HW does it automatically.

---

## TLB

**Q11. How does TLB shootdown work on ARM vs x86?**

ARM uses **hardware broadcast**: `TLBI <op>IS` is sent over the coherent interconnect (DVM messages on AMBA CHI) and acknowledged by all Inner-Shareable PEs. No IPI needed. x86 uses software IPIs (`smp_call_function` → each CPU runs `INVLPG`). ARM scales better. See [04.03](../04_TLB/03_TLB_Shootdown_and_Broadcast.md).

---

**Q12. What's ASID and how is it allocated?**

Address Space ID — 8 or 16 bit tag (`TCR_EL1.AS`) on TLB entries so context switch doesn't invalidate everything. Linux's allocator (`arch/arm64/mm/context.c`) uses a generation-counter scheme: 256 ASIDs cycle through; on rollover, flush all and bump generation. See [02.05](../02_Virtual_Memory_VMSAv8/05_ASID_and_VMID.md).

---

**Q13. What's VMID?**

Virtual Machine ID — tags TLB entries with the guest VM ID (set in VTTBR_EL2). Avoids needing TLBI on every VM switch. Allocated per active guest by KVM.

---

**Q14. Why use Break-Before-Make?**

If you change a PTE in a way that overlaps an existing valid translation (e.g., 2 MB block → 512 × 4 KB pages, or changing PA/attributes), HW may have stale TLB entries that conflict with new fetches → TLB conflict abort. BBM: (1) write invalid PTE, (2) DSB + TLBI, (3) write new PTE. See [04.02](../04_TLB/02_TLB_Maintenance_Instructions.md).

---

**Q15. How do hugepages help MMU performance?**

(1) One TLB entry covers more memory (4 KB → 2 MB = 512× coverage). (2) Shorter page walk (skip L3 for 2 MB, skip L2+L3 for 1 GB). (3) Better hugepage TLB hit rate for large working sets. Trade-off: internal fragmentation, harder to swap. See [04.04](../04_TLB/04_TLB_Performance_and_Hugepages.md).

---

## Faults

**Q16. Decode `ESR_EL1 = 0x96000045`.**

EC = (0x96000045 >> 26) & 0x3F = 0x25 → Data Abort, same EL. ISS lower bits: DFSC = 0x05 (Translation fault, L1), WnR = 1 (write). So: write to a kernel VA whose L1 PTE is invalid. See [08.02](../08_Faults_and_Aborts/02_ESR_FAR_HPFAR_Decoding.md).

---

**Q17. What's the difference between FAR and HPFAR?**

FAR (Fault Address Register) holds the **virtual** address of the faulting access (Stage 1 VA). HPFAR holds the **IPA** that caused a Stage-2 fault. Both populated together on Stage-2 fault into EL2.

---

**Q18. What's S1PTW?**

`ESR.S1PTW=1` — "Stage 1 Page Table Walk caused a Stage-2 abort". The abort happened while the MMU was fetching a Stage-1 PTE (so the descriptor itself wasn't reachable in Stage 2), not on the original guest access. Different fix-up path.

---

## System Setup

**Q19. Sequence to enable the MMU.**

1. Set up page tables in memory.
2. Write `MAIR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`.
3. `ISB` (recommended).
4. Read `SCTLR_EL1`, OR in `M | C | I` bits.
5. Write back `SCTLR_EL1`.
6. `ISB` — mandatory; subsequent instructions are fetched with MMU on.

See [07.01](../07_System_Registers_Quickref/01_SCTLR_EL1_EL2_EL3.md).

---

**Q20. How does KPTI work on arm64?**

Same idea as x86: two TTBR1 values (or use EPD0/EPD1) so kernel page tables aren't visible to userspace. On entry, swap to full kernel pgd; on exit, swap to user pgd containing only the entry trampoline + vector page. Linux config: `CONFIG_UNMAP_KERNEL_AT_EL0`. See [04.04](../04_TLB/04_TLB_Performance_and_Hugepages.md).

---

## Cross-refs

- [99.02 Cache questions](02_Cache_Interview_Questions.md)
- [99.03 Barrier questions](03_Barrier_and_Ordering_Questions.md)
- [99.04 System design scenarios](04_System_Design_Scenarios.md)
