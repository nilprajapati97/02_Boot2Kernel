# Wrong Memory Type Consequences: The Critical Bugs

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Four Categories of Wrong Memory Type Bugs

```
1. Device memory mapped as Normal Cacheable
   → Stale register reads, lost register writes

2. Normal memory mapped as Device
   → Performance collapse, possible fault

3. Coherent DMA buffer mapped as Cacheable without flush
   → DMA coherency failure

4. Wrong Device sub-type (e.g., GRE instead of nGnRnE for control regs)
   → Reordered/merged register accesses, device malfunction
```

---

## 2. Bug Type 1: Device MMIO as Normal Cacheable

```
Scenario:
  Developer accidentally uses memremap(phys, size, MEMREMAP_WB) for MMIO
  or forgets to use ioremap() and does vmap() with PAGE_KERNEL instead.

PTE has: AttrIndx = MT_NORMAL (0xFF) → WB RA WA cached memory

What happens:

  First read of device register:
    CPU L1 miss → reads value from device
    STORES in L1 cache line (read-allocate)
    Returns value to code

  Device updates the register (e.g., DMA engine sets status = DONE):
    Device writes its internal register directly
    CPU's L1 cache still holds the OLD value
    CPU's next read → L1 HIT → returns STALE value ("BUSY" instead of "DONE")
    → Infinite polling loop / hang

  Write to device register:
    CPU writes to L1 cache (WB policy) → cache line is dirty
    Write does NOT reach device (stays in cache)
    Device does not receive the command
    → Device appears unresponsive

  On SMP: worse
    CPU0 writes control register → dirty in CPU0's L1
    CPU1 reads status register (different offset, same page) → different line, OK
    CPU1 writes another register → dirty in CPU1's L1
    Flush/eviction order is unpredictable
    → Race conditions in device initialization sequence

Example: Interrupt controller (GIC) MMIO as Normal WB:
  GICD_ISENABLER write (enable interrupt): buffered in cache
  GIC does not receive the write → interrupt never enabled
  System hangs waiting for interrupt that was never actually enabled in hardware

Debug clue:
  Device works correctly on the very first boot (cold caches) but fails
  after the first cached access pollutes the TLB/cache.
```

---

## 3. Bug Type 2: Normal DRAM Mapped as Device nGnRnE

```
Scenario:
  Developer uses ioremap() for a RAM region (e.g., shared SRAM between CPUs)
  PTE has: AttrIndx = MT_DEVICE_nGnRnE (0x00)

What happens:

  No speculation or prefetching:
    CPU cannot speculate ahead into the buffer
    Hardware prefetcher disabled for Device memory
    Sequential reads: each is a separate uncached bus transaction

  Performance: ~10–50× slower than Normal WB for sequential access
    Normal WB: L1 cache hit after first fetch of 64-byte line → 4 cycles/element
    Device nGnRnE: each 8-byte read = separate bus transaction → ~80-200 cycles

  Possible fault (some architectures):
    Some SoC implementations reject Device accesses to DRAM address ranges
    (hardware range checks say "DRAM = Normal only") → synchronous abort

  Byte access limitation:
    Some implementations require aligned accesses for Device memory
    Unaligned Device access → alignment fault (EC=0x26 in ESR)
    
  DMA interaction:
    If CPU maps shared SRAM as Device but DMA engine maps it as Normal:
    CPU and DMA use different bus transaction protocols → possible ordering issue

Debug clue:
  Program is very slow, profiler shows 100% DRAM bus cycles, no L1 hits.
  mmap region has Device attributes when Normal was expected.
```

---

## 4. Bug Type 3: DMA Coherency Failure (Normal WB + no flush)

```
Scenario (software-managed coherency SoC):
  CPU maps DMA input buffer as Normal WB (AttrIndx = MT_NORMAL = 0xFF)
  DMA engine fills buffer with new data
  CPU reads buffer → reads stale cache values, not DMA data

Timeline:
  T=0: Buffer allocated, CPU may have prefetched/touched some cache lines
  T=1: DMA engine (non-coherent) writes new data to DRAM
       CPU L1/L2/L3 still holds old cache lines for buffer addresses
  T=2: CPU reads buffer → L1 HIT → old stale data
  T=3: CPU processes wrong (old) data → silent data corruption

Fix:
  Before DMA write: flush CPU cache lines for buffer (DC CIVAC)
    Ensures dirty CPU writes reach DRAM before DMA reads
  After DMA write (before CPU read): invalidate CPU cache lines (DC IVAC)
    Ensures CPU cache is empty → next CPU read goes to DRAM → fresh DMA data

  Linux DMA API:
    dma_sync_single_for_device(): DC CIVAC (flush+invalidate)
    dma_sync_single_for_cpu(): DC IVAC (invalidate)

Alternatively (correct approach):
  Map DMA buffer as MT_NORMAL_NC (0x44) if SoC is not HW-coherent
  CPU writes go directly to DRAM (bypasses cache) → DMA always sees fresh data
  CPU reads come directly from DRAM → always sees fresh DMA data
  Cost: no L1 caching for CPU → slower CPU access

HW coherent SoC (e.g., modern ARM SoC with SMMU + CMN):
  DMA is part of Inner Shareable domain
  CPU maps as Normal WB → DMA reads are snooped → always coherent
  No explicit flush needed
```

---

## 5. Bug Type 4: Wrong Device Sub-Type (GRE for Control Registers)

```
Scenario:
  Developer wants write-combining for a device BAR but accidentally
  applies GRE to the register area instead of just the framebuffer.

PTE: AttrIndx = MT_DEVICE_GRE (0x0C)

Control register sequence:
  writel(CMD_START, dev->regs + CMD_OFFSET);     // write command
  writel(ARG_VALUE, dev->regs + ARG_OFFSET);     // write argument
  val = readl(dev->regs + STATUS_OFFSET);         // read status

With GRE (Gathering, Reordering, Early Write Ack):

  1. GATHERING: The two writel() calls are to adjacent (or nearby) addresses.
     Hardware MAY combine them into a single 64-bit transaction.
     Device may not handle 64-bit writes to these registers correctly
     (many devices only support 32-bit register access width).
     → Device receives garbled 64-bit write to CMD/ARG registers

  2. REORDERING: The CMD and ARG writes may be delivered to device in any order.
     If ARG is delivered first: device processes ARG with the previous CMD
     → Wrong command-argument pairing → incorrect device operation

  3. EARLY WRITE ACK: The read of STATUS may execute before the writes
     complete at the device (writes are in a buffer, acknowledged early).
     STATUS read returns old state, not the state after CMD+ARG writes.
     → Premature "ready" detection, wrong state machine progression

Fix:
  Use MT_DEVICE_nGnRnE for register areas.
  Use MT_DEVICE_GRE only for write-combining framebuffer/stream regions.
  Use ioremap() for registers, ioremap_wc() for framebuffer.
```

---

## 6. Memory Type Conflict Between Stage 1 and Stage 2

```
In KVM virtualization:

Guest OS (Stage 1) sets PTE AttrIndx = MT_NORMAL (WB RA WA)
KVM hypervisor (Stage 2) maps same IPA with MemAttr = Device (for MMIO passthrough)

ARM combined attribute rule:
  Final attribute = INTERSECTION (more restrictive) of Stage 1 + Stage 2

  Stage 1: WB RA WA (cacheable, speculative)
  Stage 2: Device nGnRnE (no speculation, strict ordering)
  Result: Device nGnRnE wins (more restrictive)

  → Guest's "Normal WB" mapping effectively behaves as Device nGnRnE
  → Guest is unaware but gets Device ordering semantics
  → This is CORRECT for MMIO passthrough: hypervisor forces device semantics
  → Guest's speculative reads to this region are suppressed by Stage 2

KVM MMIO emulation:
  Guest accesses emulated device → Stage 2 fault (IPA not mapped)
  KVM handles the access in software → no actual device transaction
  No memory type conflict since the Stage 2 mapping is not created until needed
```

---

## 7. Interview Questions & Answers

**Q1: What are the symptoms of mapping device MMIO as Normal Cacheable, and how do you debug it?**

**Symptoms**: Device registers appear "frozen" — reads return stale values that never update (hardware register changes not visible to CPU); writes appear lost (device never receives them, causing timeouts or hang on "device not responding"). On SMP, behavior is non-deterministic — sometimes the device works (when cache happens to be flushed), sometimes not. The bug often appears as an infinite polling loop on a status register.

**Debugging**: (1) Check the PTE for the MMIO mapping: use `/proc/pid/pagemap` + kernel physmap debugging, or dump PTEs with `mm_dump_pgd_range()`; verify `AttrIndx` = 0 (Device nGnRnE), not 4 (Normal WB). (2) Check the ioremap variant used in the driver — must be `ioremap()` or `ioremap_nocache()`, not `kmap()` or `vmap()` with default `PAGE_KERNEL`. (3) Hardware debug: use an interconnect bus analyzer to observe that the device register address gets no actual bus transactions (CPU hits its own cache instead).

---

## 8. Quick Reference: Wrong Type → Consequence

| Wrong mapping | Symptom | Root cause |
|---|---|---|
| Device as Normal WB | Stale register reads, writes lost | Cache intercepts accesses |
| Normal RAM as Device | 50× performance loss, possible fault | No caching, no speculation |
| Normal WB for DMA (no flush) | Silent data corruption in DMA | Stale CPU cache vs fresh DRAM |
| GRE for control registers | Wrong ordering, merged accesses | No sequencing guarantee |
| Normal NC for WB needed | Poor CPU performance | Every access hits DRAM |
