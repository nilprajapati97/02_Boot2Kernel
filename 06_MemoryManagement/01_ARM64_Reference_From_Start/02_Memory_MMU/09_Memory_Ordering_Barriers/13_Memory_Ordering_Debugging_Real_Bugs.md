# Memory Ordering: Debugging and Real-World Bugs

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. How Memory Ordering Bugs Manifest

```
Memory ordering bugs are among the HARDEST bugs to debug:
  - They are TIMING-DEPENDENT: only appear under specific scheduling/load conditions
  - They are ARCHITECTURE-DEPENDENT: pass on x86 (stronger model), fail on ARM64
  - They are NON-DETERMINISTIC: may occur 1 in 1,000,000 executions
  - They can be MASKED by printk/debugging: barriers in printk hide the bug!
  
Common symptoms:
  - Corrupted data structures (linked list corruption, etc.)
  - NULL pointer dereferences on seemingly valid pointers
  - Stale values read from "recently updated" variables
  - Races that disappear when running under stress tools
  - Different failure modes on different ARM64 CPU generations

Memory ordering bug categories:
  1. Missing smp_wmb() (producer): data visible after flag, not before
  2. Missing smp_rmb() (consumer): reads flag, reads data before flag is processed
  3. Store-Load reordering: Dekker/mutex-like algorithms without full barrier
  4. DMA cache coherency: stale CPU cache after DMA completion
  5. Missing ISB: using stale TLB or system register values after update
  6. Missing DSB: cache maintenance not completed before dependent operation
```

---

## 2. Real Kernel Bugs Due to Missing Barriers

```
Bug Example 1: kfifo race (pre-2009 kernel)
  Before 2009: kfifo used a simple head/tail ring buffer
  Bug: producer wrote data, updated head WITHOUT smp_wmb()
  Consumer read head (new value), then read data (old/stale values!)
  
  Fix: Add smp_wmb() in producer before updating head
       Add smp_rmb() in consumer before reading data after reading head
  
  CVE/Bug: Linux kfifo ring buffer corruption under SMP load

Bug Example 2: Misuse of DMA map direction
  Driver used DMA_FROM_DEVICE for a bidirectional buffer
  CPU wrote to buffer (via normal stores, filling CPU cache)
  dma_map_single(DMA_FROM_DEVICE): DC IVAC → DISCARDS CPU's dirty data!
  DMA writes partially (doesn't overwrite all fields)
  CPU reads: some fields from DMA (correct), some from stale DRAM (lost CPU writes!)
  
  Fix: use DMA_BIDIRECTIONAL (DC CIVAC: clean THEN invalidate)
       Never use DMA_FROM_DEVICE on a buffer with outstanding CPU writes

Bug Example 3: RCU pointer publish without smp_store_release()
  Early custom RCU-like code: WRITE_ONCE(ptr, new_obj) without barrier
  Reader: ptr = READ_ONCE(my_ptr); use(ptr->field);
  
  Reader on another CPU: loads ptr (new value!) BUT ptr->field was NOT initialized yet
  (Writer's initialization stores were in store buffer, not yet visible)
  
  Fix: rcu_assign_pointer() → smp_store_release() → STLR

Bug Example 4: Spinlock unlock without STLR (hypothetical)
  If spin_unlock() used STR instead of STLR:
  CPU0 writes to protected data, then STR(lock=0)
  CPU1 sees lock=0 (lock is free)
  CPU1 reads protected data — BEFORE CPU0's STR(lock=0) was ordered after data writes!
  CPU1 sees stale protected data!
  
  Linux: arch_spin_unlock() uses STLR (release barrier) to prevent this
```

---

## 3. Debugging Tools

```
LKMM (Linux Kernel Memory Model) — formal verification:
  tools/memory-model/: formal LKMM specification
  herd7 tool: simulates memory model to check ordering properties
  
  Usage:
    cd tools/memory-model
    herd7 -conf linux-kernel.cfg litmus_test.litmus
    
  Write a litmus test to check your specific pattern:
    C producer_consumer_test
    {}
    P0(int *data, int *flag) {
        WRITE_ONCE(*data, 1);
        smp_wmb();
        WRITE_ONCE(*flag, 1);
    }
    P1(int *data, int *flag) {
        int r0 = READ_ONCE(*flag);
        smp_rmb();
        int r1 = READ_ONCE(*data);
    }
    exists (1:r0=1 /\ 1:r1=0)  /* can consumer see flag=1 but data=0? */
    
  Expected output: "Not valid" = the pattern is SAFE
                   "Valid"     = the pattern can fail! (add more barriers)

Kernel KCSAN (Kernel Concurrency Sanitizer):
  CONFIG_KCSAN: dynamic race detector for the kernel
  Instruments memory accesses and detects concurrent accesses without barriers
  
  How it works:
    Randomly inserts "watches" on memory accesses
    If two conflicting accesses occur without proper ordering: report!
    
  Enable:
    CONFIG_KCSAN=y
    CONFIG_KCSAN_REPORT_RACE_UNKNOWN_ORIGIN=y
    
  Usage:
    Boot with KCSAN kernel
    Run stress tests (syzkaller, lkdtm, custom load)
    Look for kcsan: DATA RACE in dmesg:
      BUG: KCSAN: data-race in producer_func / consumer_func
      Write at 0xffff... by task 123 on cpu 0:
        producer_func at driver.c:45
      Read at 0xffff... by task 456 on cpu 1:
        consumer_func at driver.c:89
      Suggest: add smp_wmb()/smp_rmb() or use WRITE_ONCE()/READ_ONCE()

ThreadSanitizer for user space:
  For user-space lock-free code (testing algorithms before kernel integration)
  tsan: -fsanitize=thread (Clang/GCC)
  Detects: races, missing barriers in user-space code
  
  Note: TSan uses a different model than Linux's actual CPU model
  TSan confirms logical races; may not confirm ARM64-specific CPU reordering
```

---

## 4. Code Review Checklist for Memory Ordering

```
When reviewing code for memory ordering:

Producer path:
  □ Is there a smp_wmb() OR smp_store_release() before the flag write?
  □ Is the flag write done with WRITE_ONCE() or smp_store_release()?
  □ Are data initializations complete before the flag write?

Consumer path:
  □ Is the flag read using READ_ONCE() or smp_load_acquire()?
  □ Is there a smp_rmb() after reading the flag (if READ_ONCE used)?
  □ Does the consumer use the flag's VALUE before accessing related data?

Spinlock/mutex usage:
  □ Is all shared data access inside lock/unlock?
  □ Is the unlock using smp_store_release() / STLR?
  □ Is the lock using LDAXR (acquire)?

DMA operations:
  □ Is dma_map_single() called with the correct direction?
  □ Is dma_unmap_single() called before CPU reads DMA results?
  □ Are DMA buffers cache-line aligned?
  □ Do DMA buffers share cache lines with non-DMA data?

MMIO register access:
  □ Are readl/writel used (not raw pointer dereference)?
  □ Is wmb() used before writing a trigger register?
  □ Are __raw_readl/__raw_writel used only where ordering is externally guaranteed?

RCU:
  □ Is rcu_dereference() used inside rcu_read_lock()?
  □ Is rcu_assign_pointer() used for pointer publication?
  □ Is synchronize_rcu() called before freeing old objects?

System register updates:
  □ Is ISB called after writing SCTLR_EL1, TCR_EL1, MAIR_EL1?
  □ Is DSB+TLBI+DSB+ISB used when changing page table mappings?
  □ Is DSB used after cache maintenance?
```

---

## 5. Interview Questions & Answers

**Q1: You have a bug where a device driver's DMA completion sometimes produces corrupt data. The driver uses dma_map_single() correctly. What systematic debugging steps would you take?**

Systematic debugging approach:

1. **Enable CONFIG_DMA_API_DEBUG**: this adds runtime checks for DMA API misuse — wrong directions, double-maps, access after unmap, etc. If the driver is misusing the API, this will immediately report it.

2. **Enable CONFIG_KCSAN**: check for concurrent CPU accesses to the DMA buffer without proper synchronization. If the driver and DMA completion handler both access the buffer without barriers, KCSAN will detect the race.

3. **Check DMA direction**: `DMA_FROM_DEVICE` (device writes to CPU memory) should use invalidation. If the driver incorrectly uses `DMA_TO_DEVICE` or the CPU writes to the buffer after `dma_map_single(FROM_DEVICE)`, the cache will contain stale data.

4. **Check cache line sharing**: use `pahole` or `offsetof` calculations to verify that DMA buffer fields don't share cache lines with non-DMA variables. Partial-line `DC IVAC` can corrupt adjacent non-DMA data.

5. **Verify `dma_unmap_single()` placement**: the unmap MUST occur after DMA completion (typically in the interrupt handler), before the CPU reads results. If the driver reads results before calling unmap, the cache may not be invalidated yet.

6. **Force coherent allocation**: temporarily replace `dma_map_single()` with `dma_alloc_coherent()` to bypass the cache coherency issue entirely. If the bug disappears, it confirms a cache coherency problem.

7. **Check for SMMU issues**: if SMMU is present, use `iommu_debugfs` to verify mappings. SMMU misconfiguration can cause DMA to write to the wrong physical address.

---

## 6. Quick Reference

| Tool | Purpose | How to Enable |
|---|---|---|
| KCSAN | Dynamic race detector | CONFIG_KCSAN=y |
| KASAN | Memory safety (use-after-free) | CONFIG_KASAN=y |
| DMA API debug | DMA misuse detection | CONFIG_DMA_API_DEBUG=y |
| herd7/LKMM | Formal memory model check | tools/memory-model/ |
| ThreadSanitizer | User-space race detection | -fsanitize=thread |
| lockdep | Lock ordering violations | CONFIG_LOCKDEP=y |

| Symptom | Likely Cause | Fix |
|---|---|---|
| Stale flag value after write | Missing WRITE_ONCE | Use WRITE_ONCE or STLR |
| Data corruption after flag seen | Missing smp_wmb/rmb | Add producer/consumer barriers |
| DMA result stale | dma_unmap before CPU read | Call dma_unmap after DMA done |
| ISR sees old data | Missing volatile / READ_ONCE | Use READ_ONCE in polling loop |
| System register not taking effect | Missing ISB | Add ISB after MSR |
| TLB entry still active after TLBI | Missing DSB after TLBI | DSB ISH after TLBI |
