# Memory Ordering Attributes vs Barrier Instructions

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Memory Ordering Concepts

```
ARM64 uses a WEAKLY ORDERED memory model.
Unlike x86 (TSO — Total Store Order), ARM64 makes NO ordering guarantees
between memory accesses unless:
  (a) The memory type is Device (which has ordering constraints), OR
  (b) Explicit barrier instructions are used.

Two orthogonal ordering mechanisms:
  1. MEMORY TYPE (MAIR_EL1 attribute) — controls ordering of accesses TO/FROM a device
     Set per-page via AttrIndx in PTE
     
  2. BARRIER INSTRUCTIONS (DMB/DSB/ISB) — enforces ordering in the instruction stream
     Applied at specific points in code with specific scope

Common misconception:
  "Device nGnRnE means my accesses are always ordered." → FALSE!
  Device nGnRnE orders accesses TO THE SAME DEVICE.
  Does NOT order accesses BETWEEN different devices.
  Does NOT order Device accesses vs Normal memory accesses.
  
  You need BOTH: correct memory type AND explicit barriers where needed.
```

---

## 2. Device Memory Ordering Properties

```
Device memory types encode specific ordering properties:

nGnRnE (No Gather, No Reorder, No Early Write Ack):
  - No Gather: each access is a separate bus transaction (no merging)
  - No Reorder: accesses to same device are observed in program order
  - No Early Write Ack: write ack comes only after device commits the write
  
  What this GUARANTEES:
    Access A to device, then Access B to same device:
    Device observes A before B (program order preserved within device)
    
  What this does NOT guarantee:
    Access A to Device1, then Access B to Device2:
    Device2 may observe B before Device1 observes A
    → NEED barrier between A and B for cross-device ordering

nGnRE (No Gather, No Reorder, with Early Write Ack):
  - Early Write Ack: write ack can come from a write buffer (not from device)
  - Writes may NOT have reached device when program sees acknowledgement
  - Use case: write-behind buffer, device tolerates delayed writes
  - Cannot use for command registers that must be issued in sequence to device

GRE (with Gather, Reorder, Early Write Ack):
  - All three relaxations enabled
  - Accesses to even the SAME device can be reordered/merged
  - Only safe for truly idempotent write-combining regions (framebuffer pixels)
```

---

## 3. Barrier Instructions for ARM64

```
DMB (Data Memory Barrier):
  Ensures ordering between memory accesses (load/store) in the instruction stream.
  Does NOT stall execution until previous writes are GLOBALLY observed.
  
  DMB variants:
    DMB ISH  — Inner Shareable domain (all CPUs in same cluster/SoC)
    DMB OSH  — Outer Shareable domain (CPUs + other agents outside cluster)
    DMB SY   — Full system (all agents everywhere)
    DMB LD   — Loads only (read ordering barrier)
    DMB ST   — Stores only (write ordering barrier)
    DMB ISHLD / ISHST — ISH variants of LD/ST
    DMB NSHLD / NSHST — Non-shareable variants

DSB (Data Synchronization Barrier):
  Stronger than DMB.
  Ensures completion: waits until ALL previous memory accesses complete
  AND all cache maintenance operations complete
  before any new accesses begin.
  Stalls the CPU pipeline until confirmed complete.
  
  DSB variants: same as DMB (ISH, OSH, SY, LD, ST)
  DSB SY: most expensive — wait for ALL system memory operations globally

ISB (Instruction Synchronization Barrier):
  Flushes the CPU instruction pipeline.
  Ensures instructions after ISB fetch and decode AFTER ISB completes.
  Use after: writing TTBR0/TTBR1, modifying MAIR, writing new instructions,
             writing SCTLR, etc.
  Does NOT order data memory accesses (that's DMB/DSB).

Relationship: ISB > DSB > DMB in strictness (ISB implies DSB on most µarches)
```

---

## 4. When You Need Barriers EVEN WITH Device nGnRnE

```
Case 1: Cross-device ordering
  writel(CMD, device1_reg);     // Device nGnRnE → ordered within device1
  writel(CMD, device2_reg);     // Device nGnRnE → ordered within device2
  
  Problem: ARM architecture does NOT guarantee device1 sees the write
  before device2. The two devices are independent ordering agents.
  
  Fix:
  writel(CMD, device1_reg);
  dsb(sy);                       // Ensure device1 write is globally observed
  writel(CMD, device2_reg);      // Now device2 write issues after device1 is done

Case 2: Normal memory before Device signaling
  /* Write flag in shared memory */
  data_buf[0] = computed_result;  // Normal WB write (may be in L1 cache)
  /* Signal device that data is ready */
  writel(DOORBELL, device_reg);   // Device nGnRnE write
  
  Problem: The Normal WB write to data_buf may NOT reach device's read point
  (DRAM) before the Device doorbell write. Device sees doorbell, reads data,
  but data is still in CPU's L1 cache (not yet in DRAM).
  
  Fix:
  data_buf[0] = computed_result;
  dsb(st);                        // Ensure DRAM sees the data write first
  writel(DOORBELL, device_reg);   // Then signal device

Case 3: Device read ordering for polling
  /* Poll device status register */
  status = readl(device_status_reg);   // Device nGnRnE read
  if (status == DONE) {
      /* Process result in DMA buffer */
      val = result_buf[0];              // Normal WB read
  }
  
  Problem: In a superscalar pipeline, the Normal WB read of result_buf
  can SPECULATE ahead of the Device status read. Result is read before
  checking if DMA is actually done.
  
  Fix:
  status = readl(device_status_reg);
  dsb(sy);                              // Ensure status read completes first
  if (status == DONE) {
      val = result_buf[0];              // Now definitely after status confirmed
  }
```

---

## 5. Linux Barrier Macros and Device Access Wrappers

```
Linux kernel memory barrier macros (arch/arm64/include/asm/barrier.h):

  mb()   → DSB SY (full system memory barrier)
  rmb()  → DSB LD (load-only memory barrier)
  wmb()  → DSB ST (store-only memory barrier)
  
  smp_mb()  → DMB ISH (SMP-aware, inner shareable)
  smp_rmb() → DMB ISHLD
  smp_wmb() → DMB ISHST

Device access functions (include/asm-generic/io.h, arch/arm64/include/asm/io.h):

  readl(addr):  ldr w0, [addr]; dsb(ish) equivalent
    Actually: ioread32() → readl() → __raw_readl() + barrier
    Note: barrier on READ ensures subsequent memory accesses happen after read
    
  writel(val, addr): str w0, [addr]; dsb(ish) equivalent
    Actually: iowrite32() → writel() → barrier + __raw_writel()
    Note: barrier on WRITE ensures write is visible before subsequent reads
    
  writel_relaxed(val, addr): __raw_writel() — no barrier
    Use when you add explicit barriers yourself (performance optimization)

  readl_relaxed(addr): __raw_readl() — no barrier

Typically readl/writel include a DSB barrier to prevent speculative accesses
going past the I/O operation. Use *_relaxed versions for high-frequency register
polling where you control ordering manually.
```

---

## 6. Acquire/Release Semantics for Device Drivers

```
Linux spinlock/mutex memory model uses acquire/release:
  spin_lock() → includes "acquire" semantics → no loads/stores move AFTER it
  spin_unlock() → includes "release" semantics → no loads/stores move BEFORE it

ARM64 implementation:
  LDXR + STLR (store-with-release) for spin_lock
  LDAR (load-with-acquire) for spin_unlock read

For device drivers: acquire/release provides single-sided ordering:
  Sufficient for producer-consumer patterns between a single producer and consumer.
  NOT sufficient for strict Device ordering (use DSB SY for that).

Key distinction:
  DMB/DSB: BOTH-sided ordering (ordering before AND after the barrier)
  Acquire: ONE-sided (prevents reordering AFTER acquire)
  Release: ONE-sided (prevents reordering BEFORE release)
  
  Device nGnRnE: ordering WITHIN the device's observation point
  Not about CPU-to-CPU ordering (that's DMB ISH territory)
```

---

## 7. Interview Questions & Answers

**Q1: If a device register is mapped as Device nGnRnE, do you still need a DMB/DSB between writes to two different MMIO regions of the same device?**

**Yes, in general.** Device nGnRnE guarantees that multiple accesses to THE SAME device (same memory-mapped address space, sharing the same underlying ordering agent) are observed in program order. However, if two registers map to different devices (or even different independent IP blocks on the same SoC), there is no ordering guarantee. The ARM architecture specification says Device memory ordering properties apply to accesses to the same "memory location" or "device" — the definition of "device boundary" is implementation-defined by the SoC architect. In practice, to be safe: if ordering between two writel() calls to DIFFERENT register addresses is critical (e.g., write BAR0 command, then signal BAR1 doorbell), always use a `dsb(sy)` or `dsb(st)` between them. Only omit it if the SoC's TRM explicitly guarantees ordering within the same device's register space.

---

## 8. Quick Reference

| Barrier | What it does | Use case |
|---|---|---|
| `DMB ISH` | Orders memory accesses, ISH domain | Between CPU<->CPU shared data |
| `DMB SY` | Orders memory accesses, all agents | Between CPU<->DMA, CPU<->device |
| `DSB ISH` | Wait for completion + order, ISH | After cache maintenance, before TLB ops |
| `DSB SY` | Wait for completion + order, all | After device write before reading result |
| `ISB` | Flush pipeline | After MMU changes, before new instructions |

| Device type | Self-ordering | Needs barrier? |
|---|---|---|
| nGnRnE within same device | YES | Between different devices |
| nGnRE within same device | Partial (write not confirmed) | For read-after-write within device |
| GRE | NO ordering even within device | Always need DSB between accesses |
| Normal WB | NO | Always need DMB between writes and DMA |
