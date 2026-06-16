# Page Reclaim on NUMA Systems Deep Dive

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), NUMA memory pressure

---

## 1. Concept Foundation

Page reclaim on NUMA systems must balance:
- reclaiming local node when local pressure exists
- avoiding remote reclaim if possible
- managing allocator fairness and user expectations

Challenge:
- single node filling up while others have free space
- fallback to remote allocation versus aggressive local reclaim

---

## 2. ARM64 Hardware Detail

### 2.1 Per-node watermarks and pressure

Each zone and node has independent:
- pages_min
- pages_low
- pages_high

Per-node kswapd daemon reclaims when node-local free drops below threshold.

### 2.2 NUMA-specific reclaim triggers

Alloc request for local node can trigger:
- local node reclaim first
- fallback to remote nodes if local unsuccessful
- or policy can force local-only if that's goal

---

## 3. Linux Kernel Implementation

### 3.1 Per-node kswapd

One kswapd thread per NUMA node.
Each manages its own node independently.

Benefit: parallel reclaim across nodes.

### 3.2 Reclaim policy and fallback

balance_pgdat_node() or equivalent logic:
1. check if node is balanced
2. if not, initiate reclaim on that node
3. fallback zonelist determines remote fallback order

### 3.3 Cgroup memory limit interaction

Per-node cgroup limits:
- memory.limit_in_bytes applies globally
- memory.numa_stat shows per-node usage

Reclaim must respect both global and per-node constraints.

---

## 4. Hardware-Software Interaction

Pressure scenario:
1. node 0 accumulates many pages
2. local kswapd0 starts reclaiming when low watermark hit
3. other nodes remain free
4. remote fallback is available but expensive
5. policy determines whether to reclaim aggressively local or allow remote

Tradeoff:
- aggressive local: reduces remote latency but may hurt other workloads
- allow remote: distributes load but increases latency

---

## 5. Interview Q and A

Q1: Why have per-node kswapd instead of global reclaim?
Per-node allows parallel reclaim and reduces contention on global reclaim locks.

Q2: What determines if a failing allocation tries remote versus local reclaim?
Zonelist fallback order and GFP flags (e.g., GFP_THISNODE forces local only).

Q3: Can memory.limit_in_bytes in cgroups cause imbalance across nodes?
Yes. If cgroup is pinned to few nodes, it can cause high local pressure while others stay free.

Q4: What is the latency impact of cross-node reclaim?
Same reclaim actions but on remote pages; latency and fairness both degrade.

Q5: How do you diagnose NUMA-specific reclaim pressure?
Monitor per-node kswapd activity and check which nodes have watermark crossing events.

Q6: Can background collapse (THP khugepaged) run on wrong node?
Yes, if memory tiering or tier-specific policy routes it to cold tier unfairly.

---

## 6. Pitfalls and Gotchas

- Assuming global memory pressure is same across all nodes.
- Misconfiguring cgroup node constraints causing artificial imbalance.
- Over-tuning watermarks leading to excessive per-node reclaim.
- Not monitoring per-node kswapd activity when debugging memory behavior.
- Forgetting that swappiness affects per-node reclaim ratios.

---

## 7. Quick Reference Table

| Scenario | Typical outcome |
|---|---|
| node 0 pressure, node 1 free | reclaim on node 0 or fallback to node 1 depending on policy |
| cgroup constraint to node 0 | pressure builds on node 0, cannot use node 1 |
| aggressive watermark | frequent reclaim even when system has free memory |
| swappiness high | node reclaim favors anon swap over file-backed drop |

| Observable | Meaning |
|---|---|
| kswapd0 active | node 0 below low watermark |
| per-node zone_reclaim_stat | reclaim count per node |
| numa_meminfo from numastat | current per-node free and used |
