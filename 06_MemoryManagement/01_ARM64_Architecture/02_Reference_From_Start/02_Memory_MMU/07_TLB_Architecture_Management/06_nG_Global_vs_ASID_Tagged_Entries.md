# Global vs ASID-Tagged TLB Entries: nG Bit Deep Dive

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm

---

## 1. nG Bit Definition

```
nG (not Global) bit: PTE bit[11]
  Also present in Block descriptors at L1/L2

  nG=0 (Global):
    TLB entry is NOT tagged with an ASID.
    Entry can be used regardless of the current ASID.
    Any process accessing this VA can hit this TLB entry.
    
  nG=1 (non-Global, ASID-tagged):
    TLB entry IS tagged with the ASID that was active when the entry was created.
    Entry ONLY matches when the current ASID equals the entry's stored ASID.
    Other processes (different ASID) cannot hit this entry.

ARM64 hardware behavior:
  When PTW fills the TLB from a PTE with nG=1:
    TLB entry stored as: {ASID=current_ASID, VPN=VA_page, PPN=PA_page, attrs...}
  
  When PTW fills the TLB from a PTE with nG=0:
    TLB entry stored as: {ASID=ignored, VPN=VA_page, PPN=PA_page, attrs...}
    Lookup: VA matches → return entry regardless of ASID
```

---

## 2. Linux Usage

```
Kernel pages (TTBR1 range, addresses >= 0xFFFF000000000000):
  All mapped with nG=0 (Global)
  
  Rationale:
    ALL processes share the SAME kernel virtual address space
    (same TTBR1_EL1 page table for all processes after init)
    Kernel pages don't need per-process differentiation
    
    A global TLB entry for a kernel address is valid for ALL processes
    → One TLB entry serves ALL processes instead of needing per-ASID copies
    → Conserves TLB capacity: kernel pages take up 1 entry instead of N_processes entries

User pages (TTBR0 range, addresses < 0x0001000000000000):
  All mapped with nG=1 (ASID-tagged)
  
  Rationale:
    User processes have DIFFERENT physical pages for the same VA
    (e.g., both process A and B have .text at VA 0x400000 but different PA)
    Must distinguish between processes → ASID tag required
    
    Without nG: a TLB entry for process A's 0x400000 could wrongly serve process B
    With nG=1: A's TLB entry tagged with A's ASID → only hits when A is running

Special case: KPTI
  In KPTI mode, the trampoline page (kernel exception vector) must be
  accessible from BOTH the user-mode page table AND kernel-mode page table.
  
  Solution: trampoline page is nG=0 (global) in BOTH page tables
  This allows the exception handler trampoline to be found in TLB
  regardless of which page table is active at exception entry.
  
  Without nG=0 for trampoline: exception entry might miss TLB for the
  vector address if ASID changed → extra TLB miss at critical path
```

---

## 3. TLBI and Global vs Non-Global Entries

```
TLBI instructions affect global and non-global entries differently:

TLBI VMALLE1IS:
  Invalidates ALL entries: both global (nG=0) and ASID-tagged (nG=1)
  Regardless of ASID
  Use: full flush, ASID rollover

TLBI VAE1IS, Xt:
  Invalidates: entry matching VA + ASID (from Xt[63:48])
  Effect on global entries:
    If the entry is GLOBAL (nG=0): STILL invalidated IF VA matches
    (Global entries can still be invalidated by VA)
  Effect on ASID-tagged entries:
    Only invalidated if both VA and ASID match

TLBI VAAE1IS, Xt:
  "VAA" = VA, All ASIDs
  Invalidates entries matching VA for ALL ASIDs
  Handles both global and non-global entries at this VA
  
  Used when: kernel changes a VA that might have global entry in TLB
  Example: kernel changes a vmalloc mapping → TLBI VAAE1IS
           (ASID irrelevant for global kernel entries, but VAAE1IS is safe for both)

TLBI ASIDE1IS, Xt:
  Invalidates all entries with matching ASID
  Does NOT affect global entries (nG=0 have no ASID → ASIDE1IS skips them)
  Use: process exit → clear all ASID-tagged entries for that process
  NOTE: kernel global entries are NOT flushed by ASIDE1IS (correct behavior)
```

---

## 4. nG and VMID Interaction

```
VMID (Virtual Machine ID):
  Used by EL2 (hypervisor) to tag Stage 2 TLB entries
  Similar to ASID but for VM identification

Stage 1 TLB entries (guest OS, EL1/EL0):
  Tagged with: ASID + VMID (if Stage 2 active)
  nG=1 entries: tagged with ASID from current TTBR0 AND current VMID
  nG=0 entries: tagged with VMID only (ASID ignored for global entries)

Combined lookup:
  TLB hit requires: VA + ASID + VMID all match
  For global entries: VA + VMID must match (ASID ignored)

VMID size:
  8-bit VMID: 255 VMs (VTCR_EL2.VS=0)
  16-bit VMID: 65535 VMs (VTCR_EL2.VS=1, ARMv8.1)

TLBI in hypervisor context:
  TLBI VMALLS12E1IS: Stage 1+2 flush for all ASIDs, current VMID
  TLBI ALLE1IS: ALL Stage 1 entries for all VMIDs (complete flush)
```

---

## 5. TLB Entry Tagging Summary

```
Complete TLB tagging picture:

Entry type          | Tagged with    | Invalidated by
--------------------|----------------|------------------------------------------
Kernel global       | VMID only      | VMALLE1IS, VAE1IS(any ASID), VAAE1IS
(nG=0, TTBR1)       |                | NOT by ASIDE1IS
--------------------|----------------|------------------------------------------
User ASID-tagged    | ASID + VMID    | VMALLE1IS, VAE1IS(matching ASID),
(nG=1, TTBR0)       |                | ASIDE1IS(matching ASID)
--------------------|----------------|------------------------------------------
Hyp (EL2)           | VMID           | ALLE1IS, ALLE2IS, VMALLE1IS
--------------------|----------------|------------------------------------------
Stage 2             | VMID           | IPAS2E1IS, VMALLS12E1IS

Key insight:
  ASIDE1IS does NOT flush kernel global entries.
  A process exit (ASIDE1IS) correctly leaves kernel TLB entries intact.
  This is important: kernel TLB entries are expensive to re-establish.
  Flushing them on every process exit would hurt overall system performance.
```

---

## 6. Interview Questions & Answers

**Q1: Why are kernel pages mapped with nG=0 (global) but user pages with nG=1 (ASID-tagged)?**

Kernel pages are global (`nG=0`) because the kernel's virtual address space is **shared across all processes** — every process that runs in the kernel uses the same `TTBR1_EL1` page table with identical mappings. A TLB entry for, say, `schedule()` at `0xFFFF800010001000` maps to the same PA regardless of which process is running. Making it global means: one TLB fill serves ALL processes. If it were ASID-tagged (`nG=1`), the same kernel VA would need a separate TLB entry per ASID (one for process A, one for B, etc.), wasting enormous TLB capacity — the kernel has many frequently-accessed code and data pages.

User pages are ASID-tagged (`nG=1`) because different processes map the **same virtual addresses to different physical pages**. Process A's heap at `0x1000000` maps to PA=0x20000000; process B's heap at `0x1000000` maps to PA=0x40000000. If this TLB entry were global (`nG=0`), process B could accidentally hit process A's TLB entry and access A's memory — a serious security breach. ASID tagging ensures each process only sees its own translations.

---

## 7. Quick Reference

| nG value | Type | Tagged with | Visible to |
|---|---|---|---|
| 0 (Global) | Global | VMID | All processes |
| 1 (non-Global) | ASID-tagged | ASID + VMID | Only same ASID |

| Linux mapping | nG value | Reason |
|---|---|---|
| Kernel linear map | 0 (Global) | Shared by all processes |
| Kernel vmalloc | 0 (Global) | Kernel-only VA range |
| User .text, .data, heap | 1 (ASID-tagged) | Per-process physical pages |
| KPTI trampoline | 0 (Global) | Must be accessible at exception entry |
