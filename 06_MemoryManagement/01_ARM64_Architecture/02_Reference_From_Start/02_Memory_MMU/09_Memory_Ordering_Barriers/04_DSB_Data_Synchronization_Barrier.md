# DSB: Data Synchronization Barrier

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
DSB (Data Synchronization Barrier): stronger than DMB

DSB guarantees:
  1. All explicit memory accesses (loads + stores) that appear before DSB
     COMPLETE (become globally visible to the specified domain)
  2. All cache maintenance instructions before DSB COMPLETE
  3. TLB invalidation instructions before DSB take effect
  4. Branch predictor invalidation instructions before DSB take effect
  5. The instruction pipeline STALLS until the DSB completes
     (No instructions after DSB can begin execution until DSB completes)

DSB vs DMB:
  DMB: orders memory accesses (pipeline can continue speculatively after)
  DSB: STALLS the pipeline — nothing executes after DSB until ALL 
       preceding memory/cache/TLB/BP operations COMPLETE
       
  DMB guarantees ORDERING of visibility
  DSB guarantees COMPLETION before proceeding

Pipeline behavior:
  ...[STR][STR][DMB][LDR][LDR]...  ← LDR may issue speculatively with DMB
  ...[STR][STR][DSB][LDR][LDR]...  ← LDR waits for DSB to "report complete"

When MUST you use DSB vs DMB?
  Scenarios requiring DSB:
  1. After cache maintenance (DC CIVAC, DC CVAC, IC IVAU)
  2. After TLB invalidation (TLBI ...)
  3. After writing to page tables (before accessing translated memory)
  4. Before executing newly written code (followed by ISB)
  5. After writing to system registers that affect memory behavior
  6. MMIO write-then-read sequences where device state must update
```

---

## 2. DSB Variants

```
DSB <domain><type>  — same domain/type encoding as DMB

Domain options: SY (Full System), ISH (Inner Shareable), 
                OSH (Outer Shareable), NSH (Non-Shareable)
Type options:   (none = LD+ST), LD (load only), ST (store only)

Common combinations:

DSB SY   = Full System, both loads+stores
  - Most conservative, most expensive
  - Used by: mb(), cache maintenance in module loading
  
DSB ISH  = Inner Shareable, both loads+stores
  - CPU cluster scope (typical SMP case)
  - Used by: cache maintenance after D-cache flush before I-cache inv
  
DSB ISHST = Inner Shareable, stores only
  - Used by: after DC CVAC (clean D-cache → only stores matter)
  
DSB ISHLD = Inner Shareable, loads only
  - Rarely needed directly
  
DSB NSH  = Non-Shareable, local CPU only
  - Used at CPU power-on (before coherency joins)
  - Boot code, CPU reset sequence

ARM64 Instruction encoding:
  DSB SY    → SYS #0, C3, C4, #4  (CRm=0xF, op2=4)
  DSB ISH   → SYS #0, C3, C3, #4  (CRm=0xB, op2=4)
  DSB ISHST → SYS #0, C3, C2, #4  (CRm=0xA, op2=4)
  
  (Same CRm encoding as DMB, but op2 changes: DMB=5, DSB=4)
```

---

## 3. DSB After Cache Maintenance

```
JIT code generation: complete sequence

// Step 1: Write machine code bytes via D-cache
for each byte:
  STR  wN, [code_buf + offset]

// Step 2: Clean D-cache to PoU (ensure CPU's D-cache reaches L2)
for each cache-line in [code_buf, code_buf + size):
  DC CVAU, xN   // Clean D-cache VA to Point of Unification

// Step 3: DSB ISH — WAIT for all DC CVAU to complete
DSB ISH           // ← MUST be DSB, not DMB!
                  // Ensures all cache lines have been written to L2 (PoU)

// Step 4: Invalidate I-cache (now L2 has fresh code, I-cache may have stale)
for each cache-line in [code_buf, code_buf + size):
  IC IVAU, xN   // Invalidate I-cache VA to PoU

// Step 5: DSB ISH — WAIT for all IC IVAU to complete  
DSB ISH           // Ensures all I-cache lines are now invalid

// Step 6: ISB — flush instruction pipeline
ISB               // CPU must refetch all instructions

// Now safe to branch to code_buf

Linux implementation (arch/arm64/include/asm/cacheflush.h):
  flush_icache_range(start, end) → calls:
    __flush_cache_user_range(start, end)  → loop DC CVAU + DSB + IC IVAU + DSB + ISB

DMA flush (dma_map_single → DMA_TO_DEVICE):
  for each cache line in buffer:
    DC CVAC, xN    // clean to PoC (DRAM, not just PoU)
  DSB ISH          // wait for all DC CVAC to COMPLETE and reach DRAM
  // Now DMA device can read from DRAM safely

Wrong (would cause intermittent DMA corruption):
  for each cache line:
    DC CVAC, xN
  DMB ISH          // only ordering, does NOT guarantee completion to DRAM!
  // DMA starts: some cache lines may NOT yet be in DRAM!
```

---

## 4. DSB After TLB Invalidation

```
Page table update sequence (changing a mapping):

// Step 1: Update the PTE in the page table
STORE pte_new → page_table[idx]

// Step 2: DSB ISH — ensure PTE write is visible before TLB invalidation
DSB ISH

// Step 3: Perform TLB invalidation
TLBI ASIDE1IS, xASID   // or TLBI VAE1IS, xAddr / TLBI VMALLE1IS

// Step 4: DSB ISH — wait for TLB invalidation to COMPLETE
DSB ISH               // ← MANDATORY: TLBI not complete until after this DSB!

// Step 5: ISB — flush instruction pipeline (if changing code permissions)
ISB               // ensures CPU fetches instructions using new TLB entries

Why TWO DSBs?
  First DSB ISH (before TLBI):
    Ensures the PTE write reaches the page table walker on OTHER CPUs
    Without this: other CPU's hardware page walker might cache the OLD PTE
    
  Second DSB ISH (after TLBI):
    Ensures the TLB invalidation COMPLETES on all Inner Shareable CPUs
    Without this: next load may hit the OLD (now invalidated) TLB entry
    (TLB invalidation is not instantaneous — it's a distributed operation)

Linux page table teardown (mm/memory.c):
  ptep_get_and_clear() → clear_bit() on the PTE
  flush_tlb_page() → calls:
    __flush_tlb_one_user(addr):
      DSB ISHST
      TLBI VAE1IS, xAddr | (ASID << 48)
      DSB ISH
      ISB     (if executable page)

ARM64 spec (D8.14.4): "After a TLBI instruction, a DSB is required before
the invalidation is guaranteed to be complete to all observers within
the shareability domain"
```

---

## 5. DSB in System Register Updates

```
System register writes that require DSB:

TCR_EL1 update (page size change):
  MSR TCR_EL1, xN    // write new translation control
  ISB                // required before using new translation regime
  // Note: usually the full sequence is:
  //   1. switch to temporary page tables (no DSB needed for that context)
  //   2. Write TCR_EL1
  //   3. TLBI VMALLE1IS
  //   4. DSB ISH
  //   5. ISB
  //   6. switch to new page tables

MAIR_EL1 update (memory attribute change):
  MSR MAIR_EL1, xN   // write new MAIR values
  ISB                // required: instructions after ISB use new MAIR values

SCTLR_EL1 update (enable/disable MMU):
  // Before enabling MMU:
  MSR TTBR0_EL1, x_pgd
  MSR TTBR1_EL1, x_swapper_pgd
  DSB ISH            // ensure page tables are visible
  TLBI VMALLE1IS     // invalidate all stale TLBs
  DSB ISH            // wait for TLBI to complete
  ISB                // ensure previous instructions used old MMU state
  ORR x0, x0, #SCTLR_M   // set MMU enable bit
  MSR SCTLR_EL1, x0  // enable MMU
  ISB                // pipeline flush: instructions after use new MMU

CPU power management (WFI):
  // Before WFI: ensure memory is consistent
  DSB SY     // ensure all memory ops complete
  WFI        // wait for interrupt (CPU may power gate)
  // After WFI wakeup: ISB to flush speculative state
  ISB

MMIO polling loop (device register):
  do {
      status = readl(DEVICE_STATUS_REG);  // readl includes DMB
      DSB SY;   // wait for the readl to FULLY COMPLETE before next read
  } while (!(status & DEVICE_READY));
```

---

## 6. Interview Questions & Answers

**Q1: You have a device driver that writes to three MMIO configuration registers (DMA source address, destination address, length) and then writes to a DMA START register. What barriers do you need and why?**

You need a `wmb()` (which compiles to `DSB ST` on ARM64) between the configuration register writes and the DMA START register write. Here's why:

ARM64 uses Device nGnRE memory type for MMIO — the nR (non-Reordering) attribute prevents reordering among accesses to the SAME device region, but the store buffer can still batch or reorder writes before they reach the device. Without a barrier, the DMA START write could reach the DMA controller's registers before the address/length registers have been updated. The `wmb()` / `DSB ST` ensures all stores before it have exited the CPU's store buffer and reached the device (acknowledged by the device's memory-mapped interface) before the START write is issued.

Also note: `readl()`/`writel()` in Linux already include appropriate barriers via `__iomem` typed accesses, so using those abstractions is preferred over raw pointer dereferences with manual barriers.

---

## 7. Quick Reference

| Scenario | Instruction | Notes |
|---|---|---|
| Cache clean before I-cache | DSB ISH | After DC CVAU |
| TLB invalidation complete | DSB ISH | After TLBI |
| Before WFI power gate | DSB SY | Ensures all memory complete |
| After TLB inv, before execute | DSB ISH + ISB | Both needed |
| DMA buffer flush | DSB ISH | After DC CIVAC |
| MMIO register sequence | DSB SY | After write before dependent read |

| DSB Domain | Scope | Use Case |
|---|---|---|
| DSB NSH | Local CPU | Boot/reset, single CPU |
| DSB ISH | All CPUs (inner) | SMP cache/TLB operations |
| DSB OSH | CPUs + DSPs/GPU | Cross-domain memory ops |
| DSB SY | Full system | MMIO, DMA, max correctness |
