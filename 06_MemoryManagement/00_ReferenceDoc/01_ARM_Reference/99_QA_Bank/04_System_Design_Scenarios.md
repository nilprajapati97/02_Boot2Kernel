# 99.04 — System Design Scenarios

> Open-ended interview problems. Use these as scaffolds — interviewers want to see structured thinking that touches MMU, caches, coherency, barriers, IOMMU, virtualization.

---

## Scenario 1 — Design a shared-memory channel between CPU and a non-coherent DSP

**Constraints**
- Single-producer/single-consumer ring buffer.
- DSP and CPU live on the same SoC sharing DRAM via NoC; DSP cache not snooped by CPU coherency domain.
- Throughput ~1 GB/s, low latency.

**Discussion points**
- **Memory type**: simplest correct choice — map the ring buffer as **Normal Non-Cacheable** on the CPU side (matching SMMU stage-1 attributes for the DSP). Eliminates need for cache maintenance per message.
- **Alternative**: Normal Cacheable + explicit `DC CVAC` after each enqueue, `DC IVAC` before each dequeue. Higher throughput per-line but per-message DC overhead.
- **Ordering**: write payload, then `DSB OSH` (outer shareable — DSP is outside Inner Shareable domain), then update head index with release semantics (`STLR` or `STR; DSB OSH`).
- **Consumer**: read head with acquire (`LDAR`), `DSB OSH`, then read payload.
- **Index aliasing**: cacheline-align head/tail to avoid false sharing between producer and consumer.
- **Buffer size**: power of two; index wraparound via masking.
- **SMMU setup**: map the buffer with matching cacheability; DMA-BUF + iommu_map for the DSP context.

**Trade-offs**
- NC: simpler, predictable, ~½ the bandwidth of cached.
- Cached + explicit maintenance: max bandwidth but per-msg DC cost; complex on producer-consumer overlap.
- Hybrid: payload NC, indices cached → check ISA-specific perf.

References: [01.02](../01_Memory_Model/02_Cacheability_Shareability.md), [05.03](../05_Caches/03_Cache_Maintenance_Ops_DC_IC.md), [10.04](../10_Advanced_and_Vendor_Context/04_Qualcomm_SoC_Memory_Subsystem.md).

---

## Scenario 2 — Diagnose: NIC driver gets corrupt packets after enabling jumbo frames

**Symptoms**: Standard 1500 B packets fine. 9000 B jumbo packets show occasional CRC mismatches or stale payload bytes near 64 B boundaries.

**Diagnostic checklist**
1. Is the NIC IO-coherent or does the driver use `dma_sync_*`?
2. Cache line size — is the driver assuming 64 B but CPU has 128 B `CTR_EL0.DminLine`?
3. Buffer alignment — jumbo buffer may span a page boundary; SMMU mapping correct?
4. Inbound DMA sequence — `DC IVAC` over full range before posting RX? If `DC IVAC` is done at wrong granularity (64 B step) on a 128 B-line CPU, every other line is missed.
5. Producer/consumer index ordering — release/acquire correct?

**Most likely cause**: line-size assumption. Modern Cortex-X / Apple silicon has 128 B lines; older 64 B driver loops invalidate every-other-line, leaving dirty CPU data in the cache that mixes with DMA-written data.

**Fix**: read `CTR_EL0.DminLine` at probe; use as loop stride.

References: [05.03](../05_Caches/03_Cache_Maintenance_Ops_DC_IC.md), [05.05](../05_Caches/05_VIPT_PIPT_Aliasing.md).

---

## Scenario 3 — Design a lock-free single-producer-multi-consumer queue on ARMv8

**Outline**
- Use a fixed-size ring with sequence numbers (Vyukov MPMC style).
- Producer: `STLR seq` after writing payload.
- Consumers: `LDAR seq`; if match, claim with `CAS` (use FEAT_LSE `CASAL` if available).
- Use ASID-stable storage; align each slot to a cache line to avoid false sharing.
- For backoff under contention: `WFE` after `LDAR` fails; producer uses `SEV` after publish.

**Barriers**
- Publisher: payload writes, then `STLR` (release).
- Consumer claim: `LDAR` (acquire) + `CASAL` (acquire+release).
- No DMB needed in fast path.

**ARM-specific tuning**
- Prefer LSE over LDXR/STXR (scales beyond ~16 cores).
- `WFE`/`SEV` for power and contention behavior (avoids cache-line ping-pong while spinning).

References: [06.02](../06_Memory_Barriers_Ordering/02_Acquire_Release_LDAR_STLR.md), [01.05](../01_Memory_Model/05_Atomicity_and_Single_Copy_Atomic.md).

---

## Scenario 4 — Bring up a hypervisor on a new ARMv8 platform

**Steps**
1. Verify EL2 is implemented and reachable from boot (PSCI / ATF entry).
2. Detect VHE (`ID_AA64MMFR1_EL1.VH`); pick code path.
3. Set up EL2 page tables for hypervisor's own code/data (TTBR0_EL2 / TTBR1_EL2 with VHE).
4. Set `HCR_EL2.{VM, E2H?, TGE?, IMO, FMO, AMO}` per design.
5. Configure `VTCR_EL2` — granule (4K typical), IPS, SL0, T0SZ, VMID size.
6. Allocate Stage-2 page tables for first guest; populate IPA→PA map for guest's RAM.
7. Set `VTTBR_EL2` = {VMID, S2 base}.
8. Configure GIC virtualization (GICv3 maintenance interrupt, ICH_HCR_EL2).
9. Set up VFIO + SMMUv3 for any passthrough devices, stage-2 matching guest's IPA map.
10. Save host context, `eret` to guest entry point.

**Pitfalls**
- VMID rollover handling.
- Stage-2 TLBI on guest destruction.
- DMA isolation — every device must be behind SMMU before passthrough.

References: [09.01](../09_Virtualization_and_Stage2/01_Two_Stage_Translation_Recap.md), [09.03](../09_Virtualization_and_Stage2/03_Hypervisor_Modes_KVM_Xen.md), [07.04](../07_System_Registers_Quickref/04_VTTBR_VTCR_Stage2.md).

---

## Scenario 5 — Optimize a memory-bound matrix-multiply kernel for Neoverse

**Workload**: 8K × 8K FP32 matmul, BLAS-style.

**Approach**
1. Profile with `perf stat -e cycles,instructions,L1D_CACHE_REFILL,L1D_TLB_REFILL,LL_CACHE_MISS_RD,STALL_BACKEND`.
2. Most likely bottleneck: LL_CACHE_MISS + DTLB pressure.
3. **Tile** for L1 (e.g., 64×64 blocks for one matrix block in L1).
4. **Pack** B-block into contiguous layout (cache-line aligned).
5. Use **SVE / SVE2 FMLA** for vector inner kernel.
6. Enable **THP** to reduce TLB pressure.
7. **NUMA pinning** — `numactl --cpunodebind=0 --membind=0`.
8. Consider `PRFM PLDL1KEEP` for the next B-block panel (only if PMU shows HW prefetcher missing).
9. Multi-thread with OpenMP; tile per-thread to fit shared L2/SLC partition (use **MPAM** if QoS needed).
10. Verify with `perf` — IPC, miss-rate trends.

References: [05.06](../05_Caches/06_Cache_Performance_Prefetch.md), [04.04](../04_TLB/04_TLB_Performance_and_Hugepages.md), [10.05](../10_Advanced_and_Vendor_Context/05_Performance_Counters_PMU.md).

---

## Scenario 6 — Add a new device to the SoC that needs DMA + virtual addressing

**Discussion**
- Connect through SMMUv3 with its own StreamID.
- Decide: PASID-based (per-process SVA) or per-domain (fixed by kernel)?
- SVA path: enable ATS + PRI on the device; PCI-side capability negotiation; kernel `iommu_sva_bind_device`.
- Per-domain path: driver allocates `iommu_domain`, attaches device, `dma_map_*` for buffer translation.
- For VM passthrough: VFIO attaches device to a domain whose S2 PTs match guest IPA.

**Failure modes**
- Misconfigured stream table → bypass or wrong context → silent data corruption.
- Forgotten unmap → leaked IOMMU mappings.
- IOTLB pressure → use 2 MB blocks in the S2 / S1 page tables.

References: [09.02](../09_Virtualization_and_Stage2/02_IPA_and_SMMU_IOMMU.md), [10.04](../10_Advanced_and_Vendor_Context/04_Qualcomm_SoC_Memory_Subsystem.md).

---

## Scenario 7 — Investigate "occasional kernel panic with TLB conflict abort"

**Likely cause**: Mishandled BBM (Break-Before-Make) when changing block↔page or modifying TCR/TTBR.

**Investigation**
1. Decode panic ESR — confirm DFSC = 0x30 (TLB conflict).
2. Bisect recent commits touching `arch/arm64/mm/`, hugepage code, or driver `vm_insert_*` paths.
3. Look for sequences that change an existing valid PTE in place without invalidating first.
4. Verify huge-page split paths invalidate the entire range covered by the original block.
5. Check device driver iomap paths for re-mapping the same PA with different attributes.

**Fix**: enforce BBM — invalidate (zero PTE + TLBI + DSB) before installing new mapping with different size/attributes/PA.

References: [04.02](../04_TLB/02_TLB_Maintenance_Instructions.md), [08.02](../08_Faults_and_Aborts/02_ESR_FAR_HPFAR_Decoding.md).

---

## Scenario 8 — Port x86 driver code to ARM

**Common issues to call out**
- TSO assumptions broken: SB / MP litmus patterns now visible — audit lock-free code, add `smp_rmb`/`smp_wmb`.
- `volatile` is not a memory barrier on either platform; replace with `READ_ONCE`/`WRITE_ONCE` + explicit barriers.
- `wmb()` on ARM is much weaker (`DSB OSH ST`) than x86 `MFENCE` — almost never the right primitive; use `smp_wmb()` for SMP and `dma_wmb()` for device.
- Cache maintenance — x86 is automatic for DMA; ARM needs `dma_sync_*` on non-coherent SoCs.
- CR3 → TTBR0/TTBR1 split — no per-syscall PT swap on ARM (pre-KPTI).
- 16 KB / 64 KB granule possibility — don't hardcode 4 KB.
- Cache line size — read `CTR_EL0.DminLine`, don't assume 64 B.

References: [10.03](../10_Advanced_and_Vendor_Context/03_AMD_x86_vs_ARM_Memory.md), [06.01](../06_Memory_Barriers_Ordering/01_DMB_DSB_ISB.md).

---

## Cross-refs

- [99.01 MMU questions](01_MMU_Interview_Questions.md)
- [99.02 Cache questions](02_Cache_Interview_Questions.md)
- [99.03 Barrier questions](03_Barrier_and_Ordering_Questions.md)
- [README](../README.md)
