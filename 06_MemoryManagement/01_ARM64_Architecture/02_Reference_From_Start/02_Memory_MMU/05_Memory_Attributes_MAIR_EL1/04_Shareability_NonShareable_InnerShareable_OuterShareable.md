# Shareability Domains: Non-Shareable, Inner Shareable, Outer Shareable

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. What Shareability Means in ARM64

Shareability defines **which observers** participate in the coherency domain for a given memory region. It is **not** about whether data is shared by multiple threads (that's a software concept) — it's about which hardware agents maintain a coherent view.

```
ARM64 Shareability domains (from hardware perspective):

Non-Shareable (NSH):
  Only the local processor element observes coherency.
  No cache snooping protocol required.
  Data may be cached in private L1 without broadcast to other CPUs.

Inner Shareable (ISH):
  All processors in the INNER shareability domain observe coherency.
  Typically: all CPUs within the same cluster (sharing L2) +
             potentially across clusters via CCI/CMN interconnect.
  On most ARM64 Linux SoCs, ISH = all CPU cores on the SoC.
  Cache snooping required between all CPUs in ISH domain.

Outer Shareable (OSH):
  All processors AND other agents in the OUTER shareability domain.
  Includes CPUs + GPU + DMA engines + SMMU + potentially PCIe devices.
  Cache snooping required across all agents in OSH domain.

Full System (SY):
  Available in DSB SY, DMB SY barrier instructions.
  Covers all observers including DRAM controller, external peripherals.
  Not directly a memory mapping attribute (not in MAIR).
```

---

## 2. Shareability in PTE Descriptors

```
Page/block descriptor bits[9:8] = SH[1:0]:

  0b00 = Non-Shareable
  0b01 = Reserved (UNPREDICTABLE — never use)
  0b10 = Outer Shareable
  0b11 = Inner Shareable

This field in the PTE applies to DATA ACCESSES through this mapping.
(TCR_EL1.SH0/SH1 controls shareability for PAGE TABLE WALK accesses.)

Normal memory in Linux PTEs:
  Linux sets SH=0b11 (Inner Shareable) for all Normal memory.
  #define PTE_SHARED  (3 << 8)   // SH = 0b11 = Inner Shareable

Device memory in Linux PTEs:
  Device memory: SH=0b00 (Non-Shareable)
  Device registers are not in the coherent memory space.
  SH field is technically IGNORED for Device type memory (ARM spec).
```

---

## 3. Interaction Between Shareability and Cacheability

```
The combination of shareability and cacheability determines
the actual caching behavior and coherency guarantees:

Normal Memory + Inner Shareable + WB RA WA (0xFF):
  ✅ Data cached in L1/L2/LLC
  ✅ All CPUs in ISH domain see coherent data (snooping protocol)
  ✅ TLB walks over coherent page tables
  → STANDARD LINUX CONFIGURATION for kernel and user data

Normal Memory + Non-Shareable + WB RA WA:
  ⚠ Data cached in L1/L2
  ⚠ NO coherency: each CPU may have different L1 cached values
  → DANGEROUS on SMP: leads to stale cache reads on other CPUs
  → Valid for single-core embedded applications

Normal Memory + Non-Shareable + NC (0x44):
  ✅ No caching → all CPUs read directly from DRAM → "coherent" by default
  ✅ Safe on SMP (no cache to be stale)
  → Used for: ISA legacy, very early boot before caches enabled
  → Used for: DMA coherent buffers on SoCs WITHOUT hardware cache coherency

Device Memory + Any Shareability:
  ARM spec: SH field is IGNORED for Device memory
  Device memory never participates in cache coherency protocol
  Effective shareability = non-shareable regardless of SH setting
```

---

## 4. Coherency Domain on Real ARM64 SoCs

```
ARM Neoverse N1 server SoC example:

                    ┌────────────────────────────────────┐
                    │     CMN-600 Coherent Mesh           │
  Inner Shareable Domain (ISH) = all of the following:   │
  ┌─────────────┐  ┌─────────────┐  ┌────────────────┐  │
  │ CPU Cluster │  │ CPU Cluster │  │ GPU (Mali G78) │  │
  │ 4× Cortex   │  │ 4× Cortex   │  │ (may be OSH    │  │
  │ A78 cores   │  │ A78 cores   │  │  depending on  │  │
  │ (L1+L2 each)│  │ (L1+L2 each)│  │  SoC config)   │  │
  └──────┬──────┘  └──────┬──────┘  └────────┬───────┘  │
         │                │                   │          │
  ┌──────┴────────────────┴───────────────────┴─────┐   │
  │              Shared L3 / LLC (CMN-600 HN-F)     │   │
  └──────────────────────────────┬──────────────────┘   │
                                  │                      │
                    ┌─────────────┴──────────────┐       │
                    │      DRAM Controller        │       │
                    │      (DDR5/LPDDR5)         │       │
                    └─────────────────────────────┘       │
                    └────────────────────────────────────┘

On most ARM SoCs:
  ISH domain = all CPU cores (all clusters participate in snoop protocol)
  OSH domain = ISH + GPU + SMMU-attached DMA engines + PCIe (if coherent)
  
  For software purposes: setting SH=Inner Shareable covers all CPUs.
  GPU coherency: depends on SoC — some GPUs are ISH-coherent via CMN,
  others require explicit DMA cache management (OSH or NC for GPU buffers).
```

---

## 5. Why Device Memory has No Meaningful Shareability

```
Device registers are memory-mapped physical register files in hardware blocks:
  - Not cached in CPU data caches (Device memory bypasses all caches)
  - Not part of the cache coherency protocol
  - Accesses go directly to the device via the interconnect

Because Device memory is never cached:
  There is no cache line to be "shared" or "coherent"
  The SH field in a Device PTE is architecturally ignored
  
  Reading a Device register:
    No L1 cache lookup → direct bus transaction → device register
  Writing a Device register:
    No cache write buffer → direct bus transaction → device register
    (With nE, must wait for device ack; with E, can be buffered in bus)

The only "shareability" that matters for device registers:
  MEMORY BARRIERS (DMB, DSB) — not MAIR shareability
  
  DMB ISH: ensure all Device and Normal memory accesses before the barrier
           are observable to all ISH observers (CPUs) before accesses after.
  
  DMB SY: full system barrier — also includes peripheral devices.

For device driver correctness, use DMB SY or writel()/readl() helpers
(which include appropriate barriers) rather than relying on SH bits.
```

---

## 6. Practical Example: GPU Buffer Sharing

```
Scenario: CPU and GPU sharing a frame buffer

Option A: GPU is ISH-coherent (modern SoC with coherent GPU):
  Map buffer as: Normal WB RA WA + Inner Shareable (0xFF + SH=0b11)
  GPU coherency: GPU GPU accesses are snooped by CMN → CPU and GPU
                 see the same data without any explicit flushes
  Driver code: no dma_sync_*() calls needed
  Performance: excellent (GPU gets cache hits if CPU pre-warmed data)

Option B: GPU is NOT coherent (older SoC, no cache-coherent DMA):
  Map buffer as: Normal NC (0x44) + Non-Shareable (SH=0b00)
  CPU writes go directly to DRAM → GPU reads from DRAM → consistent
  But: CPU reads from DRAM too → very slow for CPU-side processing
  
  Alternative: Map as WB for CPU, flush before GPU access:
    CPU maps as Normal WB (fast CPU access)
    Before GPU DMA: call dma_sync_single_for_device() → DC CIVAC flush
    After GPU DMA: call dma_sync_single_for_cpu() → DC IVAC invalidate
    CPU maps as WB again for read-back
    
  Linux DMA API handles this automatically based on SoC coherency topology.
```

---

## 7. Interview Questions & Answers

**Q1: What is the difference between Inner Shareable and Outer Shareable in ARM64?**

Both mean the memory region participates in cache coherency, but with different scopes. **Inner Shareable** covers all observers within the inner shareability domain, which typically corresponds to a single CPU cluster or all CPUs on the SoC sharing the same coherent interconnect. **Outer Shareable** extends further to include other system agents — GPUs, DMA engines, SMMU-attached accelerators, potentially PCIe devices.

On most ARM64 SoCs, Linux uses **Inner Shareable** for all Normal memory because the ARM Coherent Mesh Network (CMN) or Cache Coherent Interconnect (CCI) covers all CPU cores under one ISH domain. Linux doesn't need Outer Shareable for CPU-to-CPU coherency. Whether GPU or DMA engines need OSH depends on the specific SoC's interconnect topology.

**Q2: Can you safely use Non-Shareable + Write-Back on a multi-core ARM64 SoC? Why or why not?**

No, **NSH + WB is UNSAFE on SMP**. With NSH, there is no cache snooping protocol between CPUs. CPU0 writes to a Normal WB NSH cache line in its L1. CPU1 reads the same address — it finds a hit in its own L1 with the old value (or a miss that fetches from DRAM, also the old value). CPU1 never sees CPU0's write until CPU0 flushes and CPU1 invalidates. Without explicit cache maintenance (DC CIVAC + DC IVAC), the two CPUs have different views of memory — a race condition and potential data corruption for all shared data structures (page tables, kernel structures, user heap). Linux always uses Inner Shareable (SH=0b11) to avoid this.

---

## 8. Quick Reference

| SH[1:0] | Name | Scope | Linux use |
|---|---|---|---|
| 0b00 | Non-Shareable | Local CPU only | Device mem, early boot |
| 0b01 | Reserved | — | NEVER |
| 0b10 | Outer Shareable | CPUs + GPU + DMA | Some GPU scenarios |
| 0b11 | Inner Shareable | All CPUs on SoC | **ALL Normal memory** |

| Combination | Safe on SMP? | Speed | Use case |
|---|---|---|---|
| WB + ISH | Yes | Fast | Linux default for all memory |
| NC + NSH | Yes | Slow | DMA buffers (no HW coherency) |
| WB + NSH | **NO** | Fast but unsafe | Never use on SMP |
| Device + any SH | N/A | N/A | SH ignored for Device |
