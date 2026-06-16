
---

## Component Description

### 1. A7 Cluster (Efficiency)
- Contains multiple low-power cores
- Optimized for background and light workloads
- Shares a common L2 cache

### 2. A15 Cluster (Performance)
- Contains high-performance cores
- Handles compute-intensive tasks
- Has its own L2 cache for speed

### 3. L2 Cache (Per Cluster)
- Shared within each cluster
- Reduces latency compared to main memory access
- Improves performance and efficiency

### 4. Coherent Interconnect (CCI)
- Connects both clusters to memory
- Maintains cache coherency
- Ensures all cores see consistent data

### 5. System Memory
- External RAM and peripherals
- Accessed via the interconnect

---

## Key Concept: big.LITTLE Architecture

- Combines:
  - **Efficiency cores (A7)** → low power
  - **Performance cores (A15)** → high speed
- Operating system dynamically schedules tasks between clusters

---

## Data Flow (Simplified)

1. Core executes instruction
2. Checks:
   - L1 cache (inside core)
   - L2 cache (shared in cluster)
3. If not found → request goes through interconnect
4. Data fetched from system memory
5. Interconnect ensures all cores see updated data

---

## Summary

- Two clusters → different performance/power goals
- Separate caches → reduce contention
- Interconnect → keeps everything synchronized
