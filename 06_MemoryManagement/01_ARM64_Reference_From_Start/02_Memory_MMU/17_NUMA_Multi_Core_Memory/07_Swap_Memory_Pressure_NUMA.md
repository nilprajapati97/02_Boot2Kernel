# ARM64 Swap and Memory Pressure in NUMA Deep Dive

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), NUMA + swap scenarios

---

## 1. Concept Foundation

Swap behavior on NUMA systems introduces additional complexity:
- swap storage is usually centralized (not per-node)
- swapping remote memory incurs double latency (remote fetch + disk latency)
- policy balance between reclaim and swap depends on interconnect speed

---

## 2. ARM64 Hardware Detail

### 2.1 Swap device placement

Typical configuration:
- NVMe SSD typically attached to one socket
- accessed as remote device from other socket
- latency significantly worse than local memory

### 2.2 Page swapping mechanics

Swap PTE encodes:
- swap device ID
- offset within swap device

On fault:
1. identify swap PTE
2. issue I/O to swap device
3. wait for page content
4. reinstall in memory

---

## 3. Linux Kernel Implementation

### 3.1 Swappiness and NUMA

Swappiness influences NUMA reclaim balance:
- high: prefer swapping over remote fallback
- low: prefer remote memory over swap

Per-NUMA-node swappiness controls possible but rarely used.

### 3.2 Swap space allocation

Swap typically centralized on one node.
Per-node swap could be configured but adds complexity.

### 3.3 Reclaim path interaction

When node hits memory pressure:
- reclaim evaluates swap cost versus remote allocation cost
- typically: remote memory < disk swap < local reclaim

---

## 4. Hardware-Software Interaction

High-pressure scenario:
1. node 0 fills and kswapd0 activates
2. decide: remote fallback, swap, or page cache drop
3. if swappiness high, try swap on central device
4. latency consequence: much higher than anticipated

Operational issue:
- swap appears as solution but latency penalty is severe
- better to allow remote allocation if possible

---

## 5. Interview Q and A

Q1: Why is swap less effective on NUMA?
Both source (remote fetch) and sink (swap device) add latency, compounding the problem.

Q2: Should NUMA systems use swap at all?
Rarely beneficial; better to overprovision capacity or use memory tiering than swap on NUMA.

Q3: What is the latency ranking: local < remote < swap?
Typically: local mem ~70ns, remote ~200ns, swap device ~1ms or more, so yes.

Q4: Can you configure per-node swap on ARM64?
Technically possible but operationally rare and complex to manage.

Q5: How does swappiness=0 help on NUMA?
It disables swap, forcing reclaim or remote fallback; better latency predictability.

Q6: What metric indicates swap is hurting NUMA performance?
High swap-in/swap-out rate concurrent with remote allocation; suggests poor policy fit.

---

## 6. Pitfalls and Gotchas

- Enabling swap without understanding NUMA latency implications.
- Assuming high swappiness helps under NUMA pressure (often wrong).
- Forgetting that swap device I/O is not locality-aware.
- Over-tuning watermarks and triggering unnecessary swap.
- Not correlating swap activity with actual latency measurements.

---

## 7. Quick Reference Table

| Configuration | Outcome |
|---|---|
| swappiness high + central swap | aggressive swap, high latency risk |
| swappiness low + NUMA | prefer remote allocation over swap |
| swappiness=0 + NUMA | disable swap entirely, force memory management |

| Latency tier | Approximate value |
|---|---|
| local node memory | 50-100ns |
| remote node memory | 200-400ns |
| swap device via remote adapter | 1-10ms |
