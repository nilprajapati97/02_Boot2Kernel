# Memory Watermarks and Zone Management Deep Dive

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), per-zone memory management

---

## 1. Concept Foundation

Memory watermarks define the free page thresholds at which kswapd wakes and starts reclaiming.

Three tiers:
- pages_high: kswapd starts reclaiming when reached
- pages_low: kswapd continues until this threshold
- pages_min: absolute minimum; direct reclaim triggered if breached

---

## 2. ARM64 Hardware Detail

### 2.1 Zone organization

Each zone tracks free list per order (0 to MAX_ORDER).
Watermarks apply to zone-level free pages aggregated across orders.

### 2.2 Access patterns

Checking watermarks is frequent operation; cache-friendly to avoid long list walks.

---

## 3. Linux Kernel Implementation

### 3.1 Watermark calculation

Pages_min typically: max(2 * fls(nr_cpus), 16)  
Pages_low: 1.5 × pages_min  
Pages_high: 2 × pages_min

Scaled by zone size; larger zones get higher watermarks.

### 3.2 Watermark checking

In fast path (__alloc_pages_nodemask):
- check if zone has enough free pages above pages_high
- if yes, allocate immediately
- if no, enter slowpath → kswapd or direct reclaim

### 3.3 kswapd wake and sleep

balance_pgdat():
- while zone below pages_low, continue reclaim
- when zone reaches pages_high, go to sleep
- sleep interruptible; can wake on new allocation attempt

### 3.4 Direct reclaim at pages_min

If system drops to pages_min despite kswapd, direct reclaim triggered.
Requestor task blocks until pages freed or allocation fails.

---

## 4. Hardware-Software Interaction

Memory pressure scenario:
1. system has 1GB free, pages_high = 100MB
2. allocation burst uses 60MB in short time
3. free drops to 940MB, still above pages_high, no kswapd
4. next allocations gradually consume memory
5. free reaches 100MB (pages_high), kswapd wakes
6. kswapd starts scanning and dropping pages
7. free climbs back to 200MB (pages_high), kswapd sleeps
8. cycle repeats as demand fluctuates

---

## 5. Interview Q and A

Q1: Why have three watermark levels?
Three levels provide hysteresis; prevents kswapd thrashing by waking/sleeping repeatedly at threshold.

Q2: Can you tune watermarks?
Yes, via sysctl vm.min_free_kbytes; changes pages_min and scales others proportionally.

Q3: What if pages_min is set too high?
kswapd wakes frequently; more background reclaim overhead and less available application memory.

Q4: What if pages_min is too low?
System may hit pages_min during spikes; direct reclaim latency spikes on critical tasks.

Q5: How do watermarks interact with cgroups?
Per-cgroup limits apply independently; each cgroup has implicit watermarks based on limit.

Q6: How do you diagnose watermark misconfiguration?
Monitor kswapd wake rate and direct reclaim frequency; balance suggests good tuning.

---

## 6. Pitfalls and Gotchas

- Tuning vm.min_free_kbytes without understanding trade-offs.
- Assuming global watermarks apply equally to NUMA nodes (they don't; per-zone).
- Not accounting for watermark changes across different kernel versions.
- Forgetting that hugepages and fragmentation affect effective free pages.
- Over-aggressive watermark tuning leading to excessive background reclaim.

---

## 7. Quick Reference Table

| Watermark | Typical trigger |
|---|---|
| pages_high | kswapd wakes and starts reclaim |
| pages_low | kswapd continues reclaim until this reached |
| pages_min | direct reclaim triggered; emergency threshold |

| Tuning | Effect |
|---|---|
| vm.min_free_kbytes increase | higher watermarks, more background reclaim |
| vm.min_free_kbytes decrease | lower watermarks, less reclaim but riskier |
