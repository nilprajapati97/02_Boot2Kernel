# TLB Shootdown and IPI Storms

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

TLB shootdown is the process of invalidating stale translations across CPUs after page-table changes.

In large systems, excessive shootdowns can become an inter-processor interrupt (IPI) storm that increases tail latency.

---

## 2. ARM64 Hardware Detail

### 2.1 TLB invalidation semantics

ARM64 provides targeted and global TLBI operations. Scope selection matters:
- by VA + ASID
- by ASID
- full-context invalidation

Wider scope increases correctness margin but also cost.

### 2.2 Architectural cost drivers

Shootdown cost depends on:
- CPU count
- invalidation granularity
- synchronization barriers (`DSB`, `ISB`)
- concurrent page-table mutation rate

---

## 3. Linux Kernel Implementation

Common shootdown triggers:
- unmap/remap activity
- frequent `mprotect` or mapping churn
- memory compaction and migration side effects

### 3.1 Diagnostic strategy

1. identify spikes in mapping activity
2. correlate with IPI and scheduler delay metrics
3. isolate workloads causing frequent page-table updates
4. assess THP split/merge churn contribution

### 3.2 Mitigation strategy

- prefer narrower invalidation scope where safe
- batch mapping changes
- reduce avoidable map/unmap churn in userspace allocators
- tune THP behavior if split/merge storms appear

---

## 4. Hardware-Software Interaction

ARM64 invalidation primitives are efficient when used precisely. Software patterns that churn mappings erase that advantage and cause synchronization-heavy broadcast behavior.

---

## 5. Interview Q and A

Q1: Why do shootdowns affect latency more than throughput?  
They introduce synchronized stalls across CPUs, inflating tail response times.

Q2: What is the first optimization target?  
Reduce unnecessary page-table churn before micro-tuning TLBI scope.

Q3: Why can THP policy influence shootdowns?  
Split/merge operations can increase mapping mutation frequency.

Q4: How do ASIDs help?  
They allow more selective invalidations and reduce global flush frequency.

Q5: What confirms an IPI-storm hypothesis?  
Temporal alignment of mapping churn, IPI spikes, and p99 degradation.

Q6: Safe mitigation during outage?  
Constrain mapping-heavy workloads and batch updates to reduce interrupt fanout.

---

## 6. Pitfalls and Gotchas

- Using broad invalidation paths by default.
- Ignoring userspace allocator behavior as root cause.
- Fixating on CPU utilization while latency tail explodes.
- Applying risky MM policy changes without rollback plan.

---

## 7. Quick Reference Table

| Symptom | Likely Cause | Immediate Action |
|---|---|---|
| IPI spikes + p99 drift | shootdown amplification | reduce mapping churn |
| frequent global invalidations | coarse invalidation scope | use narrower scope where valid |
| latency spikes on THP events | split/merge churn | tune THP policy |
| scheduler delay growth | cross-core synchronization | batch page-table updates |
