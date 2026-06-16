# Reclaim Storms and Anti-Thrash Techniques

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

A reclaim storm occurs when kswapd and direct-reclaim threads spin continuously without sufficiently reducing pressure, causing applications to stall in reclaim paths instead of executing useful work.

Key signatures:
- sustained high `pgscan` with low `pgsteal` efficiency
- multiple threads in direct reclaim simultaneously
- PSI memory `full` trending upward continuously

---

## 2. ARM64 Hardware Detail

### 2.1 Translation impact during reclaim

During heavy reclaim on ARM64, page unmapping generates TLB invalidations proportional to the number of mapped CPUs. On many-core SoCs, this broadens the stall surface.

### 2.2 Cache thermal effects

Aggressive page scanning evicts working-set data from L2/L3 caches. Recovery requires re-warming after pressure subsides, adding a second latency wave after the reclaim storm ends.

---

## 3. Linux Kernel Implementation

Relevant kernel paths and tunables:
- `vm.min_free_kbytes` — watermark floor; raising it pre-emptively triggers earlier reclaim
- `vm.watermark_boost_factor` — temporarily raises watermarks after compaction
- `vm.watermark_scale_factor` — controls distance between low and high watermarks
- MGLRU — multi-gen LRU, better eviction decision quality than classic LRU

### 3.1 Diagnostic checklist

1. watch `/proc/vmstat` deltas: `pgscan_kswapd`, `pgscan_direct`, `pgsteal_*`
2. check `allocstall` counter growth — direct reclaim activity
3. compare scan-to-steal ratio: below 2:1 is healthy; above 5:1 is storm territory
4. verify MGLRU is enabled and min_free headroom is not too tight

### 3.2 Anti-thrash knobs

| Knob | Adjustment Direction | Effect |
|---|---|---|
| `vm.min_free_kbytes` | raise | trigger kswapd sooner, reduce direct reclaim |
| `vm.watermark_scale_factor` | raise | widen gap, more proactive kswapd sweep |
| `memory.high` per cgroup | lower | throttle pressure before watermarks breach |
| THP policy | set to `madvise` | reduce compaction-triggered storms |

### 3.3 MGLRU advantages

MGLRU tracks memory access generations and evicts cold pages more precisely, reducing useless scanning of hot pages and improving scan-to-steal ratio.

Enable: `echo 1 > /sys/kernel/mm/lru_gen/enabled`

---

## 4. Hardware-Software Interaction

Reclaim storms create positive feedback on ARM64:
- page scan triggers eviction and cache invalidation
- TLB broadcasts from unmapping stall CPUs
- stalled CPUs cannot process new allocations, worsening pressure
- more threads fall into direct reclaim

Breaking the loop requires proactive watermark tuning and cgroup isolation before storms develop.

---

## 5. Interview Q and A

Q1: What distinguishes a reclaim storm from normal kswapd activity?  
Direct reclaim in allocator hot paths and continuously rising PSI with no stabilization.

Q2: Why is scan-to-steal ratio a useful diagnostic?  
It shows whether reclaim work is productive or scanning already-active pages.

Q3: How does MGLRU help?  
It tracks access recency more accurately, reducing wasted scans of warm pages.

Q4: Why raise `min_free_kbytes` under storm conditions?  
It shifts reclaim work to kswapd proactively and keeps direct reclaim out of hot paths.

Q5: What secondary effect should you watch after a storm?  
Cache re-warm time — latency may remain elevated even after PSI drops.

Q6: What is the best long-term prevention?  
Cgroup boundaries that throttle aggressors at `memory.high` before watermarks are breached.

---

## 6. Pitfalls and Gotchas

- Setting `min_free_kbytes` too high on embedded systems — wastes real capacity.
- Confusing high `pgscan` with productive reclaim.
- Ignoring cache cold-start latency after storm recovery.
- Applying watermark tuning globally without understanding workload NUMA distribution.

---

## 7. Quick Reference Table

| Symptom | Likely State | Anti-thrash Action |
|---|---|---|
| `pgscan_direct` rising fast | direct reclaim active | raise `min_free_kbytes` |
| scan/steal ratio > 5:1 | inefficient reclaim | enable MGLRU; check THP |
| PSI full sustained | storm in progress | throttle cgroups, protect critical |
| p99 bad after PSI drops | cache cold start | allow warm-up window before decisions |
