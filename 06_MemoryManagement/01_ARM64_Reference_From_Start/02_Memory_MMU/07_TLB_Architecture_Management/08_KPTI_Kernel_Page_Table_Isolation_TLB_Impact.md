# KPTI: Kernel Page Table Isolation and TLB Impact

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. What is KPTI?

```
KPTI (Kernel Page Table Isolation):
  Mitigation for CVE-2017-5754 (Meltdown vulnerability)
  
  Meltdown attack:
    User-space code speculatively reads kernel memory via out-of-order execution
    Even though EL0 has no permission to kernel pages (AP[2:1]=0b00):
    Speculative load from kernel VA proceeds, result stored in CPU's transient state
    Side channel: attacker measures cache timing to extract the secret value
    
  KPTI fix:
    In user mode (EL0): use a MINIMAL page table (tramp_pg_dir)
    that contains ONLY the kernel entry/exit trampoline + user pages.
    NO kernel .text, .data, heap, or any other kernel mappings!
    
    User cannot even SPECULATE on kernel VAs that don't appear in the page table
    → Meltdown speculation reaches "invalid entry" → abort speculation
    
  Cost:
    Every syscall, IRQ, exception: must switch page tables
    EL0→EL1 transition: switch from tramp_pg_dir to full swapper_pg_dir
    EL1→EL0 (ERET): switch back to tramp_pg_dir
    Each switch = TTBR0/TTBR1 update + TLB invalidation = expensive!
```

---

## 2. KPTI Page Table Architecture

```
Two page tables per process:

1. swapper_pg_dir (full kernel page table, used in kernel mode EL1):
   TTBR1_EL1 → swapper_pg_dir:
     User pages (mapped via TTBR0, process-specific)
     Kernel linear map (all DRAM mapped as Normal WB)
     Kernel text (.text, readable + executable)
     Kernel data (.data, .bss, readable + writable)
     vmalloc, vmemmap, fixmap, etc.
     
2. tramp_pg_dir (minimal page table, used in user mode EL0):
   TTBR1_EL1 → tramp_pg_dir (when returning to EL0):
     User pages ONLY (same TTBR0 as before)
     ONE kernel page: the trampoline (exception entry/exit vectors)
     NOTHING ELSE from the kernel virtual address space
     
   Trampoline page:
     Contains: KPTi exception entry stub (kernel_ventry in entry.S)
     Contains: ERET path back to user space
     Physical page: shared between tramp_pg_dir and swapper_pg_dir mappings
     nG=0 (Global) so TLB entry is valid in both page table contexts

Trampoline page permissions:
   AP[2:1]=0b10 (read-only, EL0 cannot write)
   PXN=0 (kernel can execute — the exception handler trampoline)
   UXN=1 (user cannot execute the trampoline directly)
   nG=0 (global, always in TLB regardless of current process's ASID)
```

---

## 3. KPTI Context Switch Sequence

```
EL0 → EL1 (syscall, IRQ, exception):

  1. CPU executes SVC or takes IRQ at EL0
  2. Exception taken to EL1:
     TTBR1_EL1 currently points to tramp_pg_dir (minimal table)
     
  3. Trampoline code runs (in the one kernel page mapped in tramp_pg_dir):
     Save user registers to kernel stack
     Switch TTBR1_EL1 to swapper_pg_dir:
       MSR TTBR1_EL1, swapper_pg_dir_phys_addr
     TLBI VMALLE1IS  ← TLB flush!
     DSB ISH
     ISB
     
  4. Now in full kernel context:
     All kernel mappings accessible via TTBR1_EL1
     Normal kernel execution proceeds

EL1 → EL0 (ERET from syscall/IRQ):

  1. Kernel finishes, prepares ERET
  2. Trampoline ERET stub:
     Switch TTBR1_EL1 to tramp_pg_dir:
       MSR TTBR1_EL1, tramp_pg_dir_phys_addr
     TLBI VMALLE1IS  ← TLB flush!
     DSB ISH
     ISB
     Restore user registers
     ERET → returns to EL0

Cost per syscall:
  2 × TTBR1 switches: ~10 cycles each
  2 × TLBI VMALLE1IS + DSB ISH + ISB: ~2 µs each
  Total KPTI overhead per syscall: ~4–10 µs
  
  System with 1,000,000 syscalls/sec (busy web server):
    KPTI cost: 1,000,000 × 4 µs = 4 sec/sec → IMPOSSIBLE (4× overhead!)
    
  In practice: Intel CPUs with Meltdown needed full KPTI
  Most ARM64 CPUs: NOT vulnerable to Meltdown
  Linux ARM64 determines vulnerability and applies KPTI only where needed
```

---

## 4. ARM64 Meltdown Vulnerability Status

```
ARM Meltdown status (CVE-2017-5754):
  Most ARM64 CPUs: NOT vulnerable to Meltdown variant 3
  
  Reason: ARM64 speculative execution does NOT proceed past a
  permission fault for a speculative LOAD in user mode.
  The speculation stops at the permission check.
  
  Vulnerable ARM CPUs:
    Cortex-A15, A57, A72 are documented as vulnerable to some Meltdown variants
    See: ARM security advisory whitepaper
  
  Detection in Linux:
    cat /sys/devices/system/cpu/vulnerabilities/meltdown
    → "Not affected" (most ARM64 CPUs)
    → "Mitigation: PTI" (if KPTI enabled)
  
  KPTI config in Linux:
    arm64.nopti: disable KPTI (if CPU is not vulnerable)
    CONFIG_UNMAP_KERNEL_AT_EL0: enable KPTI in kernel build
    
    Default: Linux auto-detects via MIDR (CPU ID) and CPU feature bits
    arm64_kernel_unmapped_at_el0(): returns true only for vulnerable CPUs
    
  Result: most ARM64 deployments run WITHOUT KPTI
  → No syscall overhead from KPTI
  → ARM64 has significant performance advantage over x86 for syscall-heavy workloads
    (x86 CPUs are generally Meltdown-vulnerable → must use KPTI always)
```

---

## 5. TLB Impact of KPTI

```
When KPTI IS active:
  Every EL0↔EL1 transition: 2 TLBI VMALLE1IS + 2 DSB ISH + 2 ISB
  
  TLB behavior:
    Global kernel entries (nG=0) from swapper_pg_dir exist in TLB
    After switch to tramp_pg_dir: TLBI VMALLE1IS flushes THEM TOO
    → Kernel TLB entries must be re-established on every syscall return to kernel
    → Causes kernel TLB cold misses on every syscall
    
  This cascades:
    Syscall → kernel TLB cold → page table walks for kernel code/data
    Each kernel function call may TLB miss (L1 iTLB miss for .text)
    Kernel data accesses TLB miss too (.data, stack, heap)
    → 10–30% syscall performance overhead on vulnerable CPUs

KPTI ASID optimization:
  With KPTI, Linux uses TWO ASIDs per process:
    ASID_USER = ASID | 0x0000  (for tramp_pg_dir context, EL0)
    ASID_KERNEL = ASID | 0x8000 (for swapper_pg_dir context, EL1)
  
  This avoids full TLBI on each switch:
    TLB entries tagged with ASID_USER can coexist with ASID_KERNEL entries
    Switch from user to kernel: change ASID from USER to KERNEL variant
    Only flush entries for the specific ASID pair on ASID rollover
    
  Reduces KPTI overhead from ~30% to ~5–10% on workloads with moderate syscall rates
```

---

## 6. Interview Questions & Answers

**Q1: Does KPTI affect ARM64 systems in practice, and why might it not be needed?**

KPTI was designed to mitigate Meltdown (CVE-2017-5754), which relies on a CPU speculatively reading kernel memory from user mode and leaking it via side channels. **Most ARM64 CPUs are NOT vulnerable to Meltdown** because the ARM architecture specifies that speculative out-of-order execution does NOT proceed past a privilege-level permission fault for speculative loads. Unlike Intel CPUs where the speculation continues even after a permission fault (allowing the side channel), ARM's speculation correctly suppresses the result.

Linux ARM64 determines vulnerability at boot time using the CPU's MIDR (Main ID Register) and CPU feature list. Only documented vulnerable microarchitectures (a few older ARM designs) get KPTI enabled. For most ARM64 deployments (mobile, server, embedded), KPTI is NOT active — meaning there's no per-syscall page table switch overhead. This gives ARM64 a notable performance advantage over x86 for syscall-heavy workloads (web servers, databases, containers): `cat /sys/devices/system/cpu/vulnerabilities/meltdown` typically shows "Not affected" on ARM64.

---

## 7. Quick Reference

| Component | KPTI Off (normal) | KPTI On (Meltdown mitigation) |
|---|---|---|
| EL0 page table | User + kernel pages | User + trampoline ONLY |
| EL1 page table | Full kernel + user | Full kernel + user |
| Syscall overhead | Minimal | ~4–30% depending on syscall rate |
| Meltdown protection | Via AP bits (may not be sufficient) | Full isolation |
| ARM64 most CPUs | KPTI OFF | Not needed (not vulnerable) |

| ARM KPTI optimization | Benefit |
|---|---|
| Dual ASID (USER/KERNEL) | Avoid full TLBI on every switch |
| Global trampoline page (nG=0) | Trampoline always in TLB |
| ASID-based partial flush | 5–10× lower overhead vs full TLBI per switch |
