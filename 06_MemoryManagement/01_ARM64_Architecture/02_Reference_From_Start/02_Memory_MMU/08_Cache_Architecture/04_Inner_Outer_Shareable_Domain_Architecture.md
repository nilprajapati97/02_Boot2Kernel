# Inner vs Outer Shareable: Cache Domain Architecture

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Shareability Domains Concept

```
ARM64 defines THREE shareability domains for memory attributes:

Non-Shareable (NS):
  Accesses are local to ONE processor
  No coherency required with any other processor or device
  Used for: thread-local storage, scratchpad memory with no sharing
  Cache behavior: data may be cached privately, no sharing obligations

Inner Shareable (IS):
  Shared within one "inner shareable domain"
  Typically: all CPUs in a cluster share the same inner domain
  Coherency guaranteed within this domain
  Used for: CPU-to-CPU shared data, kernel memory
  
Outer Shareable (OS):
  Shared across all inner shareable domains PLUS outer-shareable devices
  Typically: multiple CPU clusters + GPU + coherent DMA devices
  Coherency guaranteed across ALL components in the outer shareable domain
  Used for: GPU-CPU shared buffers (if GPU is in outer domain), cross-cluster shared data

Full system:
  Devices not part of any shareability domain (e.g., non-coherent DMA)
  Coherency must be handled with explicit cache maintenance (DC CIVAC, etc.)

Note: "Inner" and "Outer" are relative to the point of coherency:
  Inner Shareable = components within the closest shared cache boundary
  Outer Shareable = all components sharing a system-level coherent bus
  
  ARM spec leaves exact topology to SoC designer.
  Typical ARM64 SoC mapping:
    Inner Shareable = one CPU cluster (4–8 cores sharing L2/L3)
    Outer Shareable = all clusters + coherent accelerators (via CCI/CCN)
```

---

## 2. ARM64 SoC Topology Example

```
Snapdragon 8 Gen 3 topology:
  
  ┌─────────────────────────────────────────────────────┐
  │                  Outer Shareable Domain              │
  │                                                     │
  │  ┌─────────────────────┐  ┌────────────────────┐   │
  │  │ Inner Shareable: A  │  │ Inner Shareable: B  │   │
  │  │ Prime core cluster  │  │ Performance cluster │   │
  │  │ Cortex-X4 (1 core)  │  │ Cortex-A720 (3)    │   │
  │  │ 8MB L2 (shared IS)  │  │ 2MB L2 each        │   │
  │  └─────────┬───────────┘  └────────┬────────────┘   │
  │            │                       │                 │
  │  ┌─────────┴───────────────────────┴──────────┐     │
  │  │          CCI-700 Coherent Interconnect      │     │
  │  │    System Level Cache (8MB, shared)         │     │
  │  └─────────┬──────────────────────┬────────────┘    │
  │            │                      │                  │
  │  ┌─────────┴──────┐  ┌───────────┴──────────────┐  │
  │  │ Efficiency     │  │ Adreno GPU               │  │
  │  │ Cortex-A520(4) │  │ (Outer Shareable via ACE) │  │
  │  │ 512KB L2 each  │  │ GPU L2 cache             │  │
  │  └────────────────┘  └─────────────────────────┘  │
  └─────────────────────────────────────────────────────┘
  
  Non-coherent devices (USB, PCIe, display):
    Access memory via IOMMU (SMMU)
    CPU must DC CIVAC before device reads CPU-written data
    These are NOT in the Outer Shareable domain

Implications:
  CPU core in cluster A writes to a cache line (Inner Shareable):
    Core B in SAME cluster sees it immediately (L2/CCI snoop)
    Core C in DIFFERENT cluster: depends on whether IS or OS barrier used
    
  DMB ISH: ensures ordering within inner shareable domain (one cluster)
  DMB OSH: ensures ordering across all clusters in outer shareable domain
  DMB SY: full system barrier (includes non-coherent devices?)
           Note: DMB SY does NOT help with non-coherent DMA!
           Non-coherent DMA requires explicit cache maintenance
```

---

## 3. Memory Attribute Encoding for Shareability

```
Shareability in page table descriptor bits[9:8] (SH field):

SH[1:0] | Meaning
--------|----------------------------------------------------------
0b00    | Non-Shareable: no coherency obligations outside this core
0b01    | RESERVED (do not use)
0b10    | Outer Shareable: coherent across outer shareable domain
0b11    | Inner Shareable: coherent within inner shareable domain

MAIR_EL1 interaction with shareability:
  Device memory: SH field IGNORED (device memory is always non-cacheable)
  Normal cacheable memory: SH determines coherency scope
  Normal non-cacheable: SH=0b11 (Inner Shareable) recommended even for NC
    (ensures ordering within CPU cluster for NC memory)

Linux default mappings:
  Kernel linear map: SH=0b11 (Inner Shareable), Attr=Normal WB, RW
  User anonymous pages: SH=0b11 (Inner Shareable), Attr=Normal WB
  Device MMIO: SH=0b00 (Non-Shareable), Attr=Device nGnRnE
  
  Why kernel uses Inner Shareable (not Outer Shareable):
    On most ARM64 SoCs, Inner Shareable already covers ALL CPUs
    (all CPU clusters are in the same "inner" domain for Linux purposes)
    Outer Shareable would be needed only if GPU/other devices need automatic
    coherency — but Linux handles GPU coherency via explicit DMA operations
```

---

## 4. Barriers and Shareability Interaction

```
Memory barrier shareability scope:

DMB ISH (Data Memory Barrier, Inner Shareable):
  All memory accesses before DMB are visible to all observers in Inner Shareable
  domain before any access after DMB.
  
  ARM64 assembly: DMB ISH
  Linux macro: smp_mb()  → expands to DMB ISH on ARM64

DMB OSH (Outer Shareable):
  Completion of all accesses before barrier is visible to all Outer Shareable observers
  More expensive: must wait for completion visible to ALL devices in the domain
  
  Linux: no specific macro (DMB ISH suffices for CPU-only sharing)
  Used in: DMA operations where device is in Outer Shareable domain

DMB SY (Full System):
  All-system barrier (broadest scope)
  In practice on ARM64: same cost as DMB OSH for most SoCs
  (system barrier = outer shareable barrier for standard ARM SoC topology)
  
  Linux: dsb(sy) / dmb(sy) for critical system-wide operations

ST/LD variants:
  DMB ISHST: only Store-Store ordering (lighter weight)
  DMB ISHLD: only Load-Load ordering
  DMB ISH: all ordering (load+store, store+load, etc.)
  
  Use specific variants when possible for better performance:
    Store followed by store: use ISHST (not full ISH)
    Load for acquire: LDAR instruction (no explicit DMB needed)

Shareability and write buffers:
  Non-Shareable: writes may be buffered in CPU write buffer
  Inner Shareable + DSB ISH: drains write buffer to inner domain
  Outer Shareable + DSB OSH: drains write buffer to all outer devices
  This affects: when DMA device sees a CPU write
```

---

## 5. Practical Examples

```
Example 1: Linux spinlock (CPU-to-CPU synchronization)
  All Linux CPU synchronization uses Inner Shareable domain
  Because: all CPUs (even across clusters) are coherent within what Linux
  treats as "inner shareable" (the CCI/CCN coherency domain)
  
  spin_lock() → LDAXR (load-acquire) + STLXR (store-release) loop
  These have implicit Inner Shareable acquire/release semantics
  No explicit DMB needed for CPU-to-CPU synchronization

Example 2: CPU writes buffer, GPU reads it (via coherent interconnect)
  If GPU is in Outer Shareable domain (connected via ACE-Lite):
    CPU: write buffer → store data
    CPU: DMB OSH → ensure stores visible to Outer Shareable domain
    GPU: read buffer → sees CPU's written data (no cache flush needed!)
    
  This is "hardware-coherent DMA" — the GPU just reads the data via ACE

Example 3: CPU writes buffer, USB controller reads it (non-coherent)
  USB DMA controller is NOT in any shareability domain (standalone AHB/AXI)
  CPU: write buffer (may be in CPU L1/L2 cache)
  CPU: DC CVAC (clean cache to PoC) + DSB SY (wait for completion)
  USB: DMA transfer → reads from DRAM (which now has the correct data)
  
  Without DC CVAC: USB DMA reads stale data from DRAM!
  The shareability domain of the CPU mapping doesn't help here —
  non-coherent devices ALWAYS need explicit cache maintenance

Example 4: Checking shareability domain in Linux
  struct device *dev: if dev->dma_coherent == true → hardware coherent
  dma_is_coherent(dev): returns true if device is coherent with CPU caches
  Platform: ARM64 with SMMU + stage2 coherency → may be coherent
  Platform: ARM64 with legacy non-coherent DMA → requires DC CIVAC
```

---

## 6. Interview Questions & Answers

**Q1: What is the difference between Inner Shareable and Outer Shareable, and which one does Linux use for normal memory?**

**Inner Shareable (IS)** refers to a coherency domain that typically encompasses one CPU cluster — the cores that share a common L2/L3 cache and are connected via a local coherent interconnect. Data written by Core A in an Inner Shareable region is automatically visible to Core B in the same cluster without explicit cache operations. `DMB ISH` provides ordering guarantees within this domain.

**Outer Shareable (OS)** is a broader domain that encompasses ALL Inner Shareable domains plus any outer-shareable devices (like a coherent GPU connected via ACE). A `DMB OSH` ensures ordering across ALL components in the Outer Shareable domain, which is more expensive because it must synchronize across multiple clusters and devices.

**Linux uses Inner Shareable** for all normal memory (`SH=0b11` in page table descriptors). This might seem surprising since CPUs span multiple clusters, but in practice, ARM SoC designers configure all CPU clusters to be in the SAME inner shareable domain for the OS coherency protocol — the CCI/CCN interconnect makes all CPU clusters coherent at the "inner" level from Linux's perspective. Outer Shareable is reserved for use when non-CPU accelerators (GPU, coherent DMA engines) need automatic coherency — which Linux typically handles explicitly via DMA API operations rather than relying on hardware outer-shareable coherency.

---

## 7. Quick Reference

| Domain | Typical Scope | ARM64 SoC Example | Barrier |
|---|---|---|---|
| Non-Shareable | Single core | Private scratchpad | None needed |
| Inner Shareable | CPU cluster (+ extended) | All CPUs in Linux view | DMB ISH |
| Outer Shareable | All CPUs + coherent GPU/DMA | Coherent accelerators | DMB OSH |
| Full system | Everything (software) | Explicit cache flush for non-coherent DMA | DC CIVAC + DSB SY |

| SH[1:0] bits | Memory Attribute | Cache Behavior |
|---|---|---|
| 0b00 | Non-Shareable | Cached locally, no snoops |
| 0b10 | Outer Shareable | Coherent with all OS-domain observers |
| 0b11 | Inner Shareable | Coherent with IS-domain CPUs |
