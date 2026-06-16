# Page Reclaim and Swap Complete Reference

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Reclaim and swap together provide the kernel's memory pressure control loop.

Hierarchy:
1. background reclaim (kswapd)
2. direct reclaim on allocation slowpath
3. swap as fallback for anonymous pages
4. OOM killer when no progress is possible

---

## 2. ARM64 Hardware Detail

### 2.1 Page table and AF role

AF/PTE state and TLB behavior influence reclaim cost and aging quality.

### 2.2 NUMA effect

Remote memory and remote I/O can amplify reclaim latency on ARM64 servers.

---

## 3. Linux Kernel Implementation

### 3.1 Core pipeline

- Watermarks trigger reclaim
- LRU/MGLRU chooses victims
- File pages dropped or written back
- Anon pages swapped
- Allocation retries

### 3.2 Key mechanisms in this category

- kswapd and direct reclaim
- zone watermarks
- swap subsystem and swappiness
- PSI pressure signals
- OOM selection
- MGLRU and compressed swap extensions

---

## 4. Hardware-Software Interaction

Steady-state target:
- reclaim keeps free memory above watermarks
- direct reclaim rare
- swap bounded
- PSI remains low

If this fails:
- alloc stalls increase
- swap thrashing appears
- OOM risk rises

---

## 5. Interview Q and A

Q1: Best first signal of bad reclaim health?
Rising allocstall + PSI memory.some/full trend.

Q2: Why can free memory look okay but latency be bad?
Reclaim/swap overhead can still stall hot paths.

Q3: How does MGLRU help under pressure?
It lowers wrong evictions and refault churn.

Q4: Why is swappiness workload-dependent?
Anon vs file cache trade-off differs by application behavior.

Q5: When should OOM happen?
Only after reclaim/swap cannot make forward progress.

Q6: Practical tuning order?
Capacity and workload first, then swappiness/watermarks, then advanced knobs.

---

## 6. Pitfalls and Gotchas

- Tuning reclaim before verifying memory sizing.
- Interpreting one vmstat counter in isolation.
- Ignoring memcg limits while blaming global reclaim.
- Overusing swap on latency-sensitive services.

---

## 7. Quick Reference Table

| Layer | Description |
|---|---|
| Watermarks | decide when reclaim starts/stops |
| kswapd | background reclaim engine |
| Direct reclaim | synchronous allocation fallback |
| Swap | anon page spillover |
| PSI | user-visible stall pressure indicator |
| OOM killer | final safety mechanism |
