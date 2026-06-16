# Cache Debugging and Performance Analysis Tools

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. ARM PMU Cache Events

```
ARM64 Architecture-Defined PMU Events for Cache:

Event Name          | ID    | Description
--------------------|-------|------------------------------------------
L1I_CACHE_REFILL    | 0x01  | L1 I-cache line refill (miss)
L1I_TLB_REFILL      | 0x02  | L1 I-TLB miss
L1D_CACHE_REFILL    | 0x03  | L1 D-cache line refill (miss)
L1D_CACHE           | 0x04  | L1 D-cache access (hit + miss)
L1D_TLB_REFILL      | 0x05  | L1 D-TLB miss
LD_RETIRED          | 0x06  | Load instruction retired
ST_RETIRED          | 0x07  | Store instruction retired
L1I_CACHE           | 0x14  | L1 I-cache access
L2D_CACHE           | 0x16  | L2 cache access
L2D_CACHE_REFILL    | 0x17  | L2 cache line refill (miss)
L2D_CACHE_WB        | 0x18  | L2 cache writeback
BUS_ACCESS          | 0x19  | Bus access (L3 or DRAM)
MEMORY_ERROR        | 0x1A  | Memory error (ECC)
L3D_CACHE_ALLOCATE  | 0x29  | L3 cache allocation
L3D_CACHE_REFILL    | 0x2A  | L3 cache miss (→ DRAM)
L3D_CACHE           | 0x2B  | L3 cache access

Microarchitecture events (Cortex-A78, check TRM):
L2D_CACHE_LD        | 0x50  | L2 data cache access from load
L2D_CACHE_ST        | 0x51  | L2 data cache access from store
L2D_CACHE_REFILL_LD | 0x52  | L2 data cache refill from load
L2D_CACHE_REFILL_ST | 0x53  | L2 data cache refill from store

Derived metrics:
  L1 D miss rate = L1D_CACHE_REFILL / L1D_CACHE × 100%
  L2 miss rate   = L2D_CACHE_REFILL / L2D_CACHE × 100%
  L3 miss rate   = L3D_CACHE_REFILL / L3D_CACHE × 100%
  MPKI = (L1D_CACHE_REFILL / LD_RETIRED+ST_RETIRED) × 1000 (misses per 1K instructions)
  
  Typical healthy values:
    L1 miss rate: < 5%
    L2 miss rate: < 1%
    L3 miss rate: < 0.1%
    MPKI L1: < 10 for compute-bound, < 50 for memory-bound
```

---

## 2. perf Commands for Cache Analysis

```bash
# Basic cache performance statistics
perf stat -e L1-dcache-loads,L1-dcache-load-misses,\
             L1-icache-load-misses,\
             LLC-loads,LLC-load-misses,\
             cache-references,cache-misses \
          ./workload

# ARM64 raw PMU events
perf stat -e r0003,r0004,r0016,r0017,r002A,r002B ./workload
# 0003=L1D_REFILL, 0004=L1D_ACCESS, 0016=L2_ACCESS, 0017=L2_REFILL, etc.

# Cache miss profile with call graph
perf record -e cache-misses:u -g ./workload
perf report --sort=dso,symbol --stdio | head -30

# LLC miss profile (DRAM pressure)
perf record -e LLC-load-misses:u -g ./workload
perf script | head -100

# Memory access latency profiling
perf mem record ./workload
perf mem report --sort=mem --fields=overhead,mem,symbol,dso

# False sharing detection (cache-to-cache transfers)
perf c2c record -a -- ./workload
perf c2c report --stdio
# Key columns:
#   Hitm     = hit in modified state (false sharing indicator)
#   %hitm    = % of accesses that are cross-CPU cache hits
#   Symbol   = function name causing false sharing
#   Object   = library/binary

# Top-down microarchitecture analysis (ARM Neoverse PMU)
perf stat -e slots,topdown-retiring,topdown-bad-spec,\
             topdown-fe-bound,topdown-be-bound \
          ./workload
# be-bound (back-end bound) includes memory/cache bound stalls

# Sampling by specific cache event
perf record -e L1-dcache-load-misses:P -g -F 1000 ./workload
# :P = precise sampling (uses PEBS/PRECISE_IP for accurate attribution)
```

---

## 3. Cache Information from Kernel

```bash
# Cache topology per CPU
ls /sys/devices/system/cpu/cpu0/cache/
# index0, index1, index2, index3 (L1D, L1I, L2, L3)

cat /sys/devices/system/cpu/cpu0/cache/index0/type
# Data (L1D)

cat /sys/devices/system/cpu/cpu0/cache/index0/size
# 32K (32KB L1 D-cache)

cat /sys/devices/system/cpu/cpu0/cache/index0/ways_of_associativity
# 4 (4-way set associative)

cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size
# 64 (64-byte cache lines)

cat /sys/devices/system/cpu/cpu0/cache/index0/number_of_sets
# 128 (128 sets)

# Verify: 128 sets × 4 ways × 64B = 32KB ✓

cat /sys/devices/system/cpu/cpu0/cache/index0/shared_cpu_list
# 0 (this cache is private to CPU 0)
cat /sys/devices/system/cpu/cpu2/cache/index2/shared_cpu_list
# 0-3 (L2 shared by CPUs 0,1,2,3)

# L1 cache line size from CTR_EL0 (kernel exposes it)
cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size
# 64

# Check THP allocation (affects cache utilization)
cat /proc/meminfo | grep AnonHugePages
# AnonHugePages: 204800 kB  (200MB of 2MB huge pages in use)

# Memory bandwidth (stream benchmark)
stream   # Run STREAM benchmark
# Triad: ~40-100 GB/s on ARM64 server
# Compare with theoretical DRAM BW: lpddr5x = 68 GB/s, DDR5 = 100 GB/s
```

---

## 4. Kernel Cache Debugging

```
Cache-related kernel messages and debugging:

ECC errors:
  dmesg | grep -i "cache\|ecc\|correctable\|uncorrectable"
  EDAC (Error Detection and Correction): /sys/devices/system/edac/
  Correctable errors: hardware fixed, logged
  Uncorrectable errors: memory panic, reboot required

Cache flush tracing:
  echo 1 > /sys/kernel/debug/tracing/events/kmem/mm_flush_dcache_page/enable
  cat /sys/kernel/debug/tracing/trace
  (Shows when kernel flushes D-cache for specific pages)
  
ftrace for cache maintenance:
  trace-cmd record -e cache:* ./workload    (if cache tracepoints exist)
  
  Or: add kernel ftrace to cache functions:
  echo flush_icache_range > /sys/kernel/debug/tracing/set_ftrace_filter
  echo function > /sys/kernel/debug/tracing/current_tracer
  cat /sys/kernel/debug/tracing/trace

Cache aliasing detection (VM debug):
  CONFIG_DCACHE_WORD_ACCESS: enables word-level D-cache operations
  CONFIG_ARM64_SW_TTBR0_PAN: software PAN (may disable some optimizations)
  
KASAN for cache-related memory bugs:
  dmesg | grep "BUG: KASAN"
  KASAN shadow byte checks catch use-after-free (which may be via stale TLB/cache)
  
  Note: KASAN does NOT catch stale TLB entries directly
  (stale TLB bypasses the fault that KASAN intercepts)
  
  KFENCE (lightweight KASAN alternative):
    Quarantines freed objects → detects stale cache accesses to freed memory
    CONFIG_KFENCE: enable
    kfence.sample_interval=100: tune sensitivity

Cache coherency debugging (DMA issues):
  Add pr_debug to dma_map_single(), dma_unmap_single():
    Logs VA, PA, size, direction for every DMA mapping
  Check: is DC CVAC called? Is DSB ISH issued?
  
  Useful: flush_cache_all() added before suspect DMA:
    If adding flush_cache_all() fixes the DMA bug → missing cache maintenance
    (Then find the exact missing DC CVAC/CIVAC and fix properly)
```

---

## 5. ARM Streamline and DS-5 Profiling

```
ARM Streamline (DS-5, now ARM Development Studio):
  Hardware PMU sampling for ARM64
  Visualizes: cache miss timelines, per-function attribution
  
  Setup: streamline-agent on device, GUI on host
  Connect: via TCP or USB
  Profile: ARM PMU events sampled at configurable frequency
  
  Key views:
    Timeline: per-core cache events over time
    Code view: annotated source with cache miss hotspots
    Call paths: which call chain leads to cache misses
  
  Available on: Android devices, Linux ARM64 servers
  License: ARM DS-5 (commercial) or Arm Performance Studio (newer)

Android GPU/CPU profiling:
  Android Studio Profiler:
    CPU section: shows instructions per cycle, memory stalls
  
  Simpleperf (Android's perf):
    adb shell simpleperf stat -e cache-misses ./workload
    adb shell simpleperf record -e l1d-cache-miss:u ./workload
    adb shell simpleperf report --sort=dso,symbol
  
  Mali GPU profiler:
    GPU cache miss rates for compute/graphics workloads
    Useful for CPU-GPU data sharing optimization

Linux perf PMU event discovery:
  perf list | grep -i cache
  perf list | grep -i L1
  # Shows available hardware events for your CPU
  
  ARM Neoverse N2 events:
    perf list | grep neoverse
    Shows: micro-architecture specific events for server workloads
    
  Create custom event aliases (for convenience):
    perf config alias.dtlbwalk=r0034
    perf config alias.l1dmiss=r0003
    perf stat -e dtlbwalk,l1dmiss ./workload
```

---

## 6. Interview Questions & Answers

**Q1: A network driver's DMA operations work correctly on a development board but corrupt data on production hardware. Both are ARM64. How would you diagnose?**

This is almost certainly a DMA cache coherency issue, and the hardware difference may be: the production board uses a different ARM64 SoC with a different cache topology (larger L3, different coherency domain) or different DMA controller (non-coherent vs coherent).

**Diagnostic steps**:

1. Check: is DMA direction correct? `DMA_FROM_DEVICE` when device writes, `DMA_TO_DEVICE` when CPU writes. Incorrect direction means wrong cache maintenance.

2. Check: is `dma_map_single()` called BEFORE starting DMA? And `dma_unmap_single()` called AFTER DMA completes? Many bugs: the driver calls `dma_map` but then writes to the buffer before DMA starts, or reads from the buffer before calling `dma_unmap`.

3. Check CPU alignment: is the DMA buffer cache-line-aligned (64 bytes)? Misaligned buffer: `DC IVAC` on `DMA_FROM_DEVICE` discards a partial cache line that includes non-DMA data → corruption of adjacent fields.

4. Temporarily add `flush_cache_all()` before the DMA start. If this fixes it: missing `dma_map_single()` or wrong direction.

5. Enable DMA API debug: `CONFIG_DMA_API_DEBUG=y` + `dma_debug_add_bus()`. This traces all DMA operations and detects: accessing mapped buffer without sync, double-map, unmap without prior map.

6. Use `perf record -e cache-misses ./driver_test` to profile cache miss patterns during DMA operations.

---

## 7. Quick Reference: Cache Debug Checklist

| Symptom | Likely Cause | Fix |
|---|---|---|
| DMA data corruption | Missing dma_map/cache flush | Add dma_map_single with correct direction |
| Stale data after DMA | Missing dma_unmap or sync_for_cpu | Add dma_unmap before CPU access |
| Code change not visible | JIT missing IC IVAU | flush_icache_range() after code write |
| Performance 10× slower on SMP | False sharing | perf c2c + __cacheline_aligned |
| ECC errors in dmesg | Memory/cache hardware fault | EDAC diagnosis, replace DIMM |
| BUG: KASAN use-after-free | Stale pointer via old cache | KASAN/KFENCE + code audit |

| perf Alias | ARM Event | Measures |
|---|---|---|
| `cache-misses` | L3 miss | LLC miss count |
| `cache-references` | L3 accesses | LLC access count |
| `L1-dcache-load-misses` | L1D_CACHE_REFILL | L1 D miss count |
| `L1-icache-load-misses` | L1I_CACHE_REFILL | L1 I miss count |
| `LLC-load-misses` | L3D_CACHE_REFILL | L3 miss (→ DRAM) |
