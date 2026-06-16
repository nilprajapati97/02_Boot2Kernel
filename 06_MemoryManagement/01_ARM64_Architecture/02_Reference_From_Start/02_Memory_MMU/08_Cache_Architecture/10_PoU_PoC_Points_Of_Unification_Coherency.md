# PoU and PoC: Points of Unification and Coherency Architecture

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Conceptual Framework

```
ARM64 defines multiple "points" in the memory system hierarchy:

PoU (Point of Unification):
  The memory level where the Instruction Cache (I-cache) and Data Cache (D-cache)
  are "unified" — i.e., both caches see the same data from this point and above.
  
  At PoU: A cache maintenance operation ensures I-cache and D-cache are coherent
  with each other. Any code written via D-cache is visible to I-cache after
  the appropriate maintenance operations at PoU.

PoC (Point of Coherency):
  The memory level beyond which ALL system components (multiple CPUs, GPU, DMA
  engines, DSPs) see a coherent view of memory.
  
  At PoC: A cache maintenance operation ensures ALL observers can see the data.
  
PoP (Point of Persistence) [ARMv8.4]:
  For persistent memory (NVDIMM): the point at which data becomes persistent
  across power loss. DC CVAP cleans to PoP.

Relationship:
  PoU ⊆ PoC ⊆ system boundary
  
  Typically:
    L1 I-cache and L1 D-cache are separate (Harvard)
    L2: unified → L2 is PoU
    L3 or DRAM: visible to all system agents → PoC
```

---

## 2. PoU in ARM64 Hardware

```
CLIDR_EL1 (Cache Level ID Register) identifies PoU:
  Bits[29:27] = LoU (Level of Unification, Uniprocessor)
  Bits[26:24] = LoUIS (Level of Unification, Inner Shareable)
  Bits[23:21] = LoC (Level of Coherency)
  
  LoUIS: the HIGHEST cache level that is unified AND within the Inner Shareable domain
  This is the "PoU for SMP" — the level where all CPUs in the cluster see unified I+D

  Example: Cortex-A78 in a cluster with CCI
    L1: separate I (32KB) and D (32KB) caches
    L2: unified, per-core, 1MB, PIPT
    L3: shared across cores (system cache)
    
    LoU = 2: L2 is the per-core PoU (I and D unified at L2 on this core)
    LoUIS = 2: L2 is also the inner-shareable PoU (all cores in cluster see same L2 region)
    LoC = 3: L3 (shared) is the Point of Coherency

  ARMv8.4 CTR_EL0 shortcuts:
    CTR_EL0.IDC (bit 28):
      = 1: D-cache is coherent with I-cache at PoU
            → DC CVAU not needed (hardware maintains D→PoU coherency automatically)
    CTR_EL0.DIC (bit 29):
      = 1: I-cache invalidation not needed after D-cache maintenance at PoU
            → IC IVAU not needed (hardware handles I-cache sync)
```

---

## 3. PoU Operations: Code Patching and JIT

```
When is PoU maintenance needed?
  1. Kernel module loading: ELF .text copied via memcpy (D-cache) → must be executable
  2. ftrace / kprobes: kernel patches jump instructions (writes via D-cache)
  3. JIT compilation: runtime code generation (Java JVM, eBPF JIT, LLVM)
  4. Signal handling: kernel writes signal trampoline code to user stack
  5. BPF: eBPF program compiled to native code and executed
  6. ARMv8 alternatives patching: kernel patches CPU feature-dependent instructions at boot

Correct PoU flush sequence:
  // After writing new code to [start, end):
  
  // Step 1: Push D-cache to PoU
  addr = start;
  do {
      DC CVAU, addr;
      addr += dcache_line_size;
  } while (addr < end);
  DSB ISH;  // Wait: all DC CVAU complete
  
  // Step 2: Invalidate I-cache from PoU
  addr = start;
  do {
      IC IVAU, addr;
      addr += icache_line_size;
  } while (addr < end);
  DSB ISH;  // Wait: all IC IVAU complete
  
  // Step 3: Pipeline flush
  ISB;  // Flush instruction fetch pipeline
  
  // Now safe to jump to [start, end) and execute new code
  
If CTR_EL0.IDC=1: can skip Step 1 (DC CVAU) — D-cache automatically coherent with PoU
If CTR_EL0.DIC=1: can skip Step 2 (IC IVAU) — I-cache automatically sync'd at PoU

Linux flush_icache_range() implementation with IDC/DIC optimization:
  arch/arm64/kernel/insn.c:
    if (icache_is_pipt()) {
        flush_icache_range(); // DC CVAU + IC IVAU
    } else {
        flush_icache_range_safe(); // handles VIPT aliasing
    }
  
  Alternative patching for IDC=1 CPUs:
    __ALTERNATIVE_CFG(DC CVAU loop, nop, ARM64_HAS_CACHE_IDC, CONFIG_ARM64)
    Replaces DC CVAU with NOP at boot time if CPU has IDC capability
```

---

## 4. PoC Operations: DMA and Cross-Agent Coherency

```
PoC operations needed for:
  1. Non-coherent DMA: device reads/writes to PA; CPU caches must reach PoC
  2. GPU: if GPU accesses CPU memory via bus (not via coherent ACE/CCI)
  3. Secure/non-secure world boundary: NS=1 and NS=0 memory in TrustZone
  4. kexec: loading new kernel image — new kernel will execute, old cache must be clean

DC CVAC (Clean VA to PoC):
  Writes dirty cache data to PoC (DRAM or LLC)
  Cache line remains VALID in L1/L2 (not evicted, just written back)
  Used before: DMA_TO_DEVICE operations

DC CIVAC (Clean+Invalidate VA to PoC):
  Writes dirty data to PoC AND removes cache line from L1/L2
  Used for: DMA_BIDIRECTIONAL, pre-DMA full sync

DC IVAC (Invalidate VA at PoC):
  Discards cache line (even if dirty — DATA LOSS if dirty!)
  Used ONLY when: known clean (or after DMA_FROM_DEVICE where device wrote to DRAM)

PoC vs PoU distinction in practice:
  flush_icache_range: uses DC CVAU (PoU) + IC IVAU (PoU) → cheaper
    Only needs to go to L2 (PoU) — not all the way to DRAM (PoC)
    
  __dma_flush_area: uses DC CIVAC (PoC) → more expensive
    Must push data all the way to DRAM for DMA device to see it
    L2 clean is NOT enough if DMA device accesses DRAM directly
    
  Cost comparison:
    DC CVAU (to PoU/L2): ~8–15 cycles per cache line
    DC CIVAC (to PoC/DRAM): ~50–200 cycles per cache line (may stall on DRAM)
    → Always use PoU operations when PoC is not needed!
```

---

## 5. PoU Per-CPU vs PoU Inner Shareable

```
ARM64 has two flavors of PoU:

1. PoU per-processor (LoU):
   The PoU for a single CPU core (its own I-cache and D-cache)
   DC CVAU + IC IVAU: ensures THIS CPU's I-cache sees the new code
   Does NOT guarantee OTHER CPUs' I-caches are updated
   
   Used when: code executed only by the core that wrote it
   Example: single-threaded JIT where execution happens on same core as JIT compilation

2. PoU Inner Shareable (LoUIS):
   The PoU visible to ALL CPUs in the inner shareable domain
   After DC CVAU + IC IVAU (IS broadcast): ALL CPUs' I-caches updated
   
   IC IVAU is already Inner Shareable (broadcasts to all CPUs with cache coherency)
   So: a single IC IVAU on one CPU flushes I-cache for this VA on ALL CPUs
   (because the IS snoop propagates to all inner-shareable CPUs)
   
   Exception: IC IALLU (Invalidate ALL, no address) is per-core only
   IC IALLUIS: broadcasts to all IS-domain CPUs
   
   Linux uses IC IVAU (Inner Shareable by design on ARM64 SMP)
   → No need for IPI-based per-CPU cache flush for code patching
   
   Compare with x86:
     x86 has coherent I+D caches (PoU = PoC = L3 essentially)
     No DC CVAU or IC IVAU needed — just a DSB is sufficient
     ARM64 requires explicit I-cache maintenance (more work but flexible)
```

---

## 6. Interview Questions & Answers

**Q1: Why does the JIT compiler need DC CVAU followed by IC IVAU, rather than just IC IVAU alone?**

The CPU has SEPARATE L1 I-cache and L1 D-cache (Harvard architecture). When the JIT compiler writes new machine code to memory, those writes go through the CPU's **D-cache** (data path). The L1 D-cache and L1 I-cache are NOT automatically kept in sync — they are independently managed caches.

If we only issue `IC IVAU` (Invalidate I-cache): the I-cache line is discarded. On the next instruction fetch, the CPU will load the instruction from the L2 cache (PoU). BUT if the D-cache has **dirty** (modified, not yet written back) data for that address, the L2 doesn't have the new code yet! The I-cache would re-fetch the OLD code from L2/DRAM.

So the correct sequence is:
1. `DC CVAU`: **clean** the D-cache line to PoU (force dirty new code up to L2)
2. `DSB ISH`: wait for the clean to complete (ordering)
3. `IC IVAU`: **invalidate** the I-cache line (force re-fetch from L2 on next execution)
4. `DSB ISH + ISB`: pipeline fence

After this sequence: L2 has the new code (from DC CVAU), I-cache fetches from L2 (after IC IVAU invalidation) → executes new JIT code correctly. The `DC CVAU` step can be skipped only on CPUs where `CTR_EL0.IDC=1`, which means the CPU's hardware automatically propagates D-cache writes to PoU without explicit software maintenance.

---

## 7. Quick Reference

| Maintenance | Target Level | Instruction | Use Case |
|---|---|---|---|
| Clean to PoU | L2 (I+D unified) | DC CVAU | JIT/code patch Step 1 |
| Inv I-cache to PoU | L1 I-cache | IC IVAU | JIT/code patch Step 2 |
| Inv all I-cache IS | All I-caches | IC IALLUIS | Full I-cache flush on all CPUs |
| Clean to PoC | DRAM/LLC | DC CVAC | DMA_TO_DEVICE |
| Clean+Inv to PoC | DRAM/LLC | DC CIVAC | DMA_BIDIRECTIONAL |
| Inv at PoC | L1/L2 only | DC IVAC | DMA_FROM_DEVICE |

| CLIDR_EL1 Field | Bits | Meaning |
|---|---|---|
| LoU | [32:30] | Level of Unification (per-CPU) |
| LoUIS | [26:24] | Level of Unification, Inner Shareable |
| LoC | [29:27] | Level of Coherency (for DC CVAC/CIVAC) |

| CTR_EL0 bit | Meaning | Optimization |
|---|---|---|
| IDC [28] | HW D→PoU coherency | Skip DC CVAU |
| DIC [29] | HW I-cache PoU sync | Skip IC IVAU |
