# Swap and zswap Operational Tuning

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Swap provides a pressure relief valve. zswap adds a compressed in-RAM cache in front of disk swap.

Used correctly, both extend effective working set capacity; used incorrectly, they become latency amplifiers and disk stress sources.

---

## 2. ARM64 Hardware Detail

### 2.1 ARM64 latency implications

ARM64 server and embedded deployments vary widely in storage I/O latency:
- fast NVMe: swap round trip in tens of microseconds
- slow eMMC: round trip in milliseconds
- zswap can absorb cold-page eviction and delay or eliminate disk hits

### 2.2 Memory bandwidth considerations

Compression in zswap consumes CPU cycles and memory bandwidth. On bandwidth-sensitive workloads, zswap tuning must account for this cost.

---

## 3. Linux Kernel Implementation

Key components:
- `swappiness` sysctl — 0 to 200, balances file-page vs anon reclaim preference
- `zswap.enabled`, `zswap.max_pool_percent`, `zswap.compressor`
- `zram` — block device backed by RAM, pairs with swap
- per-cgroup swappiness via `memory.swappiness` (v1) or MGLRU influence

### 3.1 Operational tuning guide

| Scenario | swappiness | zswap pool | Recommended |
|---|---|---|---|
| latency-critical server | 10 or less | 10-15% | minimize swap use |
| batch or burst workload | 60-80 | 20-30% | let zswap absorb bursts |
| memory-constrained embedded | 100 | 30-40% | maximize effective RAM |

### 3.2 zswap configuration sequence

1. enable: `echo 1 > /sys/module/zswap/parameters/enabled`
2. set pool cap: `echo 20 > /sys/module/zswap/parameters/max_pool_percent`
3. choose compressor: `lz4` (fast, lower ratio) or `zstd` (higher ratio, slightly more CPU)
4. monitor with `/sys/kernel/debug/zswap/`

### 3.3 Danger signs

- `zswap.stored_pages` near pool limit → spillover to disk swap
- high `zswapout` rate → working set larger than pool capacity
- latency spikes correlated with swap I/O → reduce swappiness

---

## 4. Hardware-Software Interaction

The swap decision path under ARM64 follows reclaim watermarks and PSI signals. zswap inserts a compression step that adds CPU overhead but saves memory bandwidth over full page-out.

On ARM64 SoCs with slower storage, zswap is often the deciding factor between acceptable and unacceptable tail latency under pressure.

---

## 5. Interview Q and A

Q1: What does `swappiness=0` do?  
It avoids anonymous page reclaim until file-page reclaim is exhausted; it does not disable swap entirely.

Q2: Why prefer zswap over raw swap for latency?  
Compressed pages remain in RAM, avoiding storage I/O round-trips.

Q3: What compressor is best for ARM64 production?  
`lz4` for latency-sensitive; `zstd` for memory-constrained where higher ratio matters.

Q4: When does zswap stop helping?  
When the working set overflows the pool and pages spill to disk, defeating the purpose.

Q5: How do you detect zswap pool exhaustion?  
`/sys/kernel/debug/zswap/pool_pages` near the cap threshold.

Q6: Right swappiness for a latency-critical service?  
10 or below; rely on cgroup limits and headroom rather than swapping.

---

## 6. Pitfalls and Gotchas

- Setting `swappiness=0` and believing swap is disabled.
- Using a small zswap pool on workloads with large cold sets.
- Ignoring compressor CPU overhead on bandwidth-limited platforms.
- Forgetting to monitor pool saturation until latency has already spiked.

---

## 7. Quick Reference Table

| Knob | Range | Production Guidance |
|---|---|---|
| `swappiness` | 0–200 | 10 for servers, 60–80 for batch |
| `zswap.max_pool_percent` | 1–100 | 15–25 for most workloads |
| `zswap.compressor` | lz4, zstd, lzo | lz4 for low latency |
| `zswap stored_pages` | monitor | alert before pool exhaustion |
