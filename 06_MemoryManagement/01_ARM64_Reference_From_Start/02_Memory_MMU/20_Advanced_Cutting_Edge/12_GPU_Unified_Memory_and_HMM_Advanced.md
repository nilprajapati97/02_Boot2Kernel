# GPU Unified Memory and HMM Advanced Deep Dive

Category: Advanced and Cutting-Edge Topics  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

HMM extends unified addressability across CPU and accelerators.

Key outcomes:
- reduced copy overhead
- shared virtual memory abstractions
- dynamic migration between device and system memory

---

## 2. ARM64 Hardware Detail

### 2.1 IOMMU/SMMU role

SMMU translates device accesses and enforces permissions for shared address spaces.

### 2.2 Coherency implications

Coherent interconnect quality determines synchronization and migration efficiency.

---

## 3. Linux Kernel Implementation

### 3.1 Core primitives

- hmm_range_fault for range validation
- mmu notifiers for invalidation tracking
- migrate_vma for CPU/device page migration

### 3.2 Pinning constraints

GUP/FOLL_PIN paths must avoid long-term pinning conflicts with migration and reclaim.

---

## 4. Hardware-Software Interaction

Compute job flow:
1. process maps data once
2. GPU faults/accesses via shared VA
3. pages migrate as needed
4. kernel syncs permissions and invalidations

---

## 5. Interview Q and A

Q1: Why HMM instead of explicit memcpy?
Simpler programming model and better dynamic placement.

Q2: Main bottleneck in unified memory?
Migration thrash under unstable access locality.

Q3: Why are MMU notifiers critical?
They keep device mappings consistent with CPU page table changes.

Q4: What breaks migration most often?
Long-term page pins and poor locality.

Q5: How does SMMU contribute?
Address translation and permission enforcement for device accesses.

Q6: Key tuning axis?
Reduce cross-domain page ping-pong.

---

## 6. Pitfalls and Gotchas

- Unbounded long-term pinning.
- Ignoring invalidate latency for high-churn mappings.
- Assuming all devices support equivalent coherency.
- No telemetry on migration/refault rates.

---

## 7. Quick Reference Table

| Primitive | Description |
|---|---|
| hmm_range_fault | validate/populate CPU-side ranges for device usage |
| mmu notifier | propagate mapping invalidation to devices |
| migrate_vma | move pages between system and device memory |
| FOLL_PIN | long-term pin tracking with stricter semantics |
