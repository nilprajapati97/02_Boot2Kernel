# ISB: Instruction Synchronization Barrier

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
ISB (Instruction Synchronization Barrier):
  Flushes the INSTRUCTION PIPELINE of the CPU
  
  After ISB:
  - All instructions ALREADY FETCHED and DECODED are DISCARDED
  - CPU refetches from the next PC with the current context
  - Any speculative instructions in-flight are cancelled
  
ISB guarantees:
  1. All system register changes before ISB are VISIBLE to instructions after ISB
  2. TLB state changes (from TLBI+DSB) are used for fetching instructions after ISB
  3. Cache state changes (from DC/IC+DSB) are used for instructions after ISB
  4. Branch predictor changes are reflected for instructions after ISB
  5. All HINT instructions before ISB have completed their effects

ISB vs DSB vs DMB:
  DMB: orders DATA memory accesses (loads/stores)
  DSB: waits for DATA memory accesses to COMPLETE
  ISB: flushes the INSTRUCTION pipeline (instruction fetch side effects)
  
  Note: ISB implies DSB (you cannot meaningfully flush the pipeline
  without first waiting for pending data operations)
  
  ISB does NOT imply full memory consistency:
  ISB alone after a STORE does NOT make the store visible to other CPUs!
  ISB is about the LOCAL CPU's instruction execution state.

ISB encoding: SYS #0, C3, C6, #5 (fixed, no domain/type variants)
```

---

## 2. When ISB Is Required

```
Rule: "Any change that affects how the CPU INTERPRETS memory or 
       executes instructions requires ISB before the effect is visible."

Case 1: After writing to system registers (SCTLR_EL1, TCR_EL1, MAIR_EL1, etc.)
  MSR SCTLR_EL1, x0    // change MMU/cache control
  ISB                  // pipeline flush: instructions after use new SCTLR
  // Instructions after ISB: executed under NEW SCTLR_EL1 settings

Case 2: After enabling/disabling the MMU
  MSR SCTLR_EL1, x0    // SCTLR_M bit set → MMU enabled
  ISB                  // CPU must refetch PC through MMU now
  // First instruction after ISB: translated through new MMU
  
  Without ISB: CPU might fetch a few more instructions from the
  PRE-MMU physical address mapping before the pipeline flushes!
  → Execution of instructions at wrong mapping → undefined behavior

Case 3: After TLBI + DSB (before executing code in newly mapped region)
  STORE new_pte → page_table
  DSB ISH
  TLBI VAE1IS, xaddr
  DSB ISH              // wait for TLBI to complete
  ISB                  // flush pipeline: instructions after use new TLBs
  // Branch to newly mapped code

Case 4: After I-cache invalidation (JIT code, module loading)
  // Wrote machine code, did DC CVAU + DSB + IC IVAU + DSB
  ISB                  // flush pipeline: CPU refetches from updated I-cache
  BL  new_function     // safe to call: pipeline uses updated I-cache

Case 5: After writing new page tables (boot time)
  // Set up identity map
  MSR TTBR0_EL1, x_id_pgd
  ISB
  MSR TTBR1_EL1, x_kern_pgd
  ISB
  // Enable MMU
  MSR SCTLR_EL1, x_sctlr
  ISB

Case 6: After HINT instructions (PRFM, ESB, PSB, TSB)
  // Some HINT instructions need ISB to guarantee effect
  PRFM PLDL1KEEP, [x0]   // prefetch hint
  // ISB not needed for PRFM (non-binding hint)
  // But ESB (Error Synchronization Barrier) DOES need ISB after it
```

---

## 3. Common Mistake: Missing ISB

```
Bug Example 1: Missing ISB after MAIR_EL1 update

void change_memory_attributes(u64 new_mair) {
    msr(MAIR_EL1, new_mair);
    // ← MISSING ISB HERE!
    // Instructions after this may still use OLD MAIR_EL1
    // Memory accesses may use WRONG attributes!
}

Fix:
void change_memory_attributes(u64 new_mair) {
    msr(MAIR_EL1, new_mair);
    isb();              // ← force MAIR update before any memory access
}

Bug Example 2: Missing ISB after TCR_EL1 update (page size change)

void switch_page_size(u64 new_tcr) {
    write_sysreg(new_tcr, tcr_el1);
    // ← MISSING ISB!
    // CPU may still use old TCR for the NEXT FEW memory accesses!
    // Page size field TG0/TG1 not yet in effect!
}

Fix:
    write_sysreg(new_tcr, tcr_el1);
    isb();
    // Now TCR_EL1 is in effect

Bug Example 3: Missing ISB after JIT code generation

void jit_emit(void *buf, size_t size) {
    // ... write code bytes ...
    flush_icache_range(buf, buf + size);  // DC CVAU + DSB + IC IVAU + DSB + ISB
    // flush_icache_range includes ISB ← OK, this is correct
}

// But if someone does it manually:
    for (va = start; va < end; va += line_size) {
        asm("DC CVAU, %0" : : "r"(va));
    }
    asm("DSB ISH");
    for (va = start; va < end; va += line_size) {
        asm("IC IVAU, %0" : : "r"(va));
    }
    asm("DSB ISH");
    // ← FORGOT ISB!
    asm("BL generated_func");  // may execute stale instructions!
    // Must add: asm("ISB");  ← BEFORE the BL
```

---

## 4. ISB and Speculation

```
Speculation and ISB:

ARM64 CPUs heavily speculate:
  Branch prediction: CPU fetches along predicted path
  Memory dependency prediction: loads issued before stores checked
  Value prediction: (some ARM cores) predict load values
  
ISB effect on speculation:
  All in-flight speculative instructions: DISCARDED after ISB
  Branch predictor state: NOT cleared by ISB
  Speculative prefetches: cancelled
  
  ISB is NOT a speculation barrier:
  Speculative loads AFTER the ISB can still be issued speculatively
  ISB only cancels speculation that was ALREADY IN FLIGHT at ISB execution time

Explicit Speculation Barrier:
  ARMv8.5 adds: SB (Speculation Barrier) instruction
  SB: stops all FUTURE speculation through this point
  Different from ISB: ISB flushes pipeline, SB stops future speculation

  Linux: sb() macro → emits SB on CPUs with FEAT_SB
         Falls back to DSB SY + ISB on older CPUs (Spectre v1 mitigation)

Context in Spectre mitigation:
  After array_index_nospec() sanitization:
    sb() / DSB SY + ISB  ← ensure sanitized value is used, not speculated around
  After syscall entry:
    ISB to cancel any speculative state from user mode
    (prevents some speculative execution attacks)
```

---

## 5. Linux ISB Usage in Source

```
isb() macro in Linux (arch/arm64/include/asm/barrier.h):
  #define isb() asm volatile("isb" : : : "memory")
  
  Note: "memory" clobber: tells compiler ALL memory is potentially modified
  This prevents compiler from reordering memory accesses across isb()

Usage locations in Linux kernel:

1. arch/arm64/mm/proc.S: cpu_do_switch_mm()
   MSR TTBR0_EL1, x0
   ISB             ← after TTBR switch

2. arch/arm64/mm/cache.S: flush_icache_range()  
   DSB ISH
   IC IALLUIS      (or IC IVAU loop)
   DSB ISH
   ISB             ← after I-cache invalidation

3. arch/arm64/kernel/setup.c: cpu_init()
   MSR SCTLR_EL1, x0
   ISB             ← after SCTLR update

4. arch/arm64/include/asm/sysreg.h: write_sysreg()
   // Many write_sysreg() calls are NOT followed by ISB automatically
   // Caller is responsible for ISB when needed!
   
5. arch/arm64/kernel/head.S: __enable_mmu
   MSR SCTLR_EL1, x0   // enable MMU
   ISB
   RET             ← first instruction after ISB uses new MMU

6. arch/arm64/kvm/hyp/exception.c: (EL2→EL1 switch)
   ISB after VMID/TTBR switch to ensure new translation context
```

---

## 6. Interview Questions & Answers

**Q1: After a Linux kernel module is loaded and its text section is written, what complete sequence of operations is needed before the module's functions can be called, and why is each step necessary?**

Complete sequence for making module code executable:

1. **Write code to allocated memory**: `vmalloc()` returns kernel virtual address; code bytes written via normal STR instructions.

2. **Apply relocations**: patch up relocations in the code (STR instructions to fix up PLT entries, GOT entries, etc.).

3. **DC CVAU** for each cache line: clean the D-cache to PoU (Point of Unification). The code was written through the D-cache; the L1 D-cache must push its dirty lines to L2 so the I-cache can see the updated code.

4. **DSB ISH**: wait for all DC CVAU operations to complete. Without this, some cache lines might not have reached PoU yet.

5. **IC IVAU** (or `IC IALLUIS`) for each cache line: invalidate the I-cache to PoU. The I-cache may have stale copies of the memory (from before the code was written). Force a refill from the updated L2.

6. **DSB ISH**: wait for all IC IVAU operations to complete. The I-cache invalidation is a distributed operation on SMP.

7. **ISB**: flush the instruction pipeline. The CPU may have already speculatively fetched instructions from the module's address range (if it was previously mapped). After ISB, the CPU refetches from the updated I-cache.

Only after step 7 is it safe to call the module's `init_module()` function.

---

## 7. Quick Reference

| System Register Changed | DSB Needed? | ISB Needed? |
|---|---|---|
| MAIR_EL1 | No | Yes |
| TCR_EL1 | Yes (TLBI) | Yes |
| SCTLR_EL1 | Yes (TLBI) | Yes |
| TTBR0/1_EL1 | No | Yes |
| VBAR_EL1 | No | Yes |
| TPIDR_EL0 | No | No (user register) |

| Operation | Needs ISB? | Reason |
|---|---|---|
| TLB invalidation (TLBI+DSB) | Yes | CPU must refetch through new TLBs |
| I-cache invalidation (IC+DSB) | Yes | CPU must refetch from updated I-cache |
| Page table update (store+TLBI+DSB) | Yes (if code) | Instruction fetch affected |
| D-cache maintenance only (DC+DSB) | No | Data only, no fetch side effects |
| Spinlock acquire | No | DMB/LDAR sufficient for data ordering |
