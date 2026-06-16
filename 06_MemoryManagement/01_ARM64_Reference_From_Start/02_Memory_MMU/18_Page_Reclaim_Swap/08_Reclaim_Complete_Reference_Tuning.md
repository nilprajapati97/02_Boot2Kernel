# Page Reclaim Complete Reference and Tuning Deep Dive

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), comprehensive reclaim reference

---

## 1. Concept Foundation

Page reclaim is the kernel's mechanism for freeing memory when needed.

Multi-faceted system:
- kswapd background daemon (NUMA-aware)
- direct reclaim in allocation path
- watermark-driven control
- swappiness policy
- pressure signals (PSI)
- OOM killer fallback

---

## 2. ARM64 Hardware Detail Summary

Per-node management enables NUMA locality.
Watermarks tuned per-zone to balance responsiveness and efficiency.

---

## 3. Linux Kernel Implementation Summary

Core path:
1. memory pressure detected (allocation failure or watermark check)
2. trigger reclaim (kswapd or direct)
3. shrink_lruvec() scans LRU lists
4. page candidates: drop or swap
5. pages freed back to allocator
6. allocation retried

---

## 4. Hardware-Software Interaction: Complete Picture

Memory lifecycle:
1. pages allocated and used
2. access patterns change
3. memory pressure builds (external allocation demand)
4. kswapd detects and scans LRU
5. old/cold pages evicted or swapped
6. freed pages recycled for new allocations
7. workload adapts as latency increases

Balance point:
- sufficient free pages maintained by kswapd
- direct reclaim minimized to preserve latency
- OOM avoided by capacity management

---

## 5. Interview Q and A

Q1: How do kswapd and direct reclaim interact?
Both use same page eviction logic; kswapd is proactive, direct is reactive on allocation failure.

Q2: What is the ideal watermark tuning?
Watermarks set such that kswapd prevents reaching direct reclaim threshold.

Q3: When does PSI memory.full spike?
System thrashing: multiple tasks each doing page reclaim, all blocked on I/O.

Q4: Why not disable swap and use only RAM?
Capacity constraint; swap provides safety valve but at latency cost.

Q5: How do you profile reclaim activity?
Monitor kswapd wake frequency, direct reclaim rate, and PSI metrics simultaneously.

Q6: What is vmstat and how does it show reclaim?
vmstat shows pgsteal_* (pages reclaimed), pswpin/pswpout (swap activity), allocstall_* (direct reclaim stalls).

---

## 6. Pitfalls and Gotchas

- Tuning watermarks without understanding NUMA interaction.
- Setting swappiness too high and causing swap thrashing.
- Ignoring PSI and only monitoring raw memory counters.
- Misconfiguring memory cgroup limits leading to direct reclaim spikes.
- Not monitoring OOM killer; can mask underlying capacity problems.

---

## 7. Quick Reference: Full Reclaim Stack

| Component | Role |
|---|---|
| watermarks | define thresholds for kswapd activation |
| kswapd | background reclaim, per-node |
| LRU lists | organize pages by age and type |
| reclaim policy | decide file vs. anon, when to swap |
| swappiness | tune file vs. swap preference |
| direct reclaim | synchronous path on allocation failure |
| OOM killer | fallback when all else fails |
| PSI | measure pressure impact on applications |

| Tuning lever | Effect |
|---|---|
| vm.min_free_kbytes | adjust watermarks higher/lower |
| vm.swappiness | bias reclaim toward swap or file drop |
| memory.limit_in_bytes (cgroup) | per-cgroup pressure and reclaim |
| /proc/[pid]/oom_adj | tune OOM killer preference |
