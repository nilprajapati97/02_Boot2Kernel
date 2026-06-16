# TLB Debugging, Profiling, and Performance Analysis

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA, Performance Engineers

---

## 1. ARM PMU Events for TLB

```
ARM64 PMU events for TLB monitoring (architecture-defined, all ARM64 CPUs):

Event Name        | ID   | Description
------------------|------|------------------------------------------
L1I_TLB_REFILL    | 0x01 | L1 instruction TLB miss (PTW started)
L1I_TLB           | 0x26 | L1 instruction TLB access count
L1D_TLB_REFILL    | 0x05 | L1 data TLB miss (PTW started)
L1D_TLB           | 0x25 | L1 data TLB access count
L2D_TLB_REFILL    | 0x2F | L2 TLB miss (page table walk to memory)
L2D_TLB           | 0x2E | L2 TLB access count
DTLB_WALK         | 0x34 | Data TLB walk started (miss requiring PTW)
ITLB_WALK         | 0x35 | Instruction TLB walk started

Derived metrics:
  L1 dTLB miss rate = L1D_TLB_REFILL / L1D_TLB × 100%
  L2 TLB miss rate = L2D_TLB_REFILL / L2D_TLB × 100%
  PTW rate = (DTLB_WALK + ITLB_WALK) / total_instructions × 100%

Microarchitecture-specific events (Cortex-A78 / Neoverse N2):
  L1D_TLB_REFILL_LD  | 0xC6  | L1 dTLB miss on loads specifically
  L1D_TLB_REFILL_ST  | 0xC7  | L1 dTLB miss on stores specifically
  L2D_TLB_REFILL_LD  | 0x104 | L2 TLB miss on loads
  L2D_TLB_REFILL_ST  | 0x105 | L2 TLB miss on stores
  (Cortex-specific IDs; check TRM for your exact microarchitecture)
```

---

## 2. perf Commands for TLB Analysis

```bash
# Basic TLB miss counting
perf stat -e L1-dcache-load-misses,dTLB-load-misses,iTLB-load-misses,dTLB-store-misses \
          ./workload

# ARM64 PMU raw events (use r prefix for raw event IDs)
perf stat -e r0001,r0005,r002E,r002F,r0034,r0035 ./workload
# r0001=L1I_TLB_REFILL, r0005=L1D_TLB_REFILL, r002E=L2D_TLB,
# r002F=L2D_TLB_REFILL, r0034=DTLB_WALK, r0035=ITLB_WALK

# Monitor TLB misses systemwide (all processes, 10 seconds)
perf stat -a -e dTLB-load-misses,dTLB-store-misses sleep 10

# Per-process TLB profile with call graph
perf record -e dTLB-load-misses:u -g -p PID sleep 30
perf report --sort=dso,symbol --stdio | head -50

# TLB miss flame graph
perf record -e dTLB-load-misses:u -g ./workload
perf script | stackcollapse-perf.pl | flamegraph.pl > tlb_misses.svg

# Count TLBI instructions (kernel mode, using kernel PMU events)
# ARM Neoverse: TLB flush event
perf stat -e r0022 -a sleep 5   # TLB_FLUSH event on some implementations
```

---

## 3. /proc and /sys TLB Information

```bash
# Check CPU TLB sizes (from kernel CPUID parsing)
cat /sys/devices/system/cpu/cpu0/cache/index*/
# cache/index0/: L1 data cache
# cache/index1/: L1 instruction cache
# cache/index2/: L2 cache
# No direct TLB entry in /sys/devices/system/cpu/cpu0/tlb/ (not standard)

# /proc/cpuinfo TLB information
grep -A2 "TLB" /proc/cpuinfo
# ARM64: Shows TLB info if populated by kernel (arch/arm64/kernel/cpuinfo.c)
# Output: "DTLB size: 32-entry 4-way"  (if kernel populates this)

# /proc/vmstat — kernel TLB flush statistics
cat /proc/vmstat | grep tlb
# nr_tlb_remote_flush: cross-CPU TLB flush IPIs sent
# nr_tlb_remote_flush_received: TLB flush IPIs received
# nr_tlb_local_flush_all: full local TLB flushes
# nr_tlb_local_flush_one: single-page local TLB flushes
# High nr_tlb_remote_flush → lots of TLB shootdowns (mprotect/munmap workload)

# NUMA and TLB stats
numastat -c
# Shows NUMA TLB hit/miss statistics per node

# Check transparent hugepage state (affects TLB pressure)
cat /sys/kernel/mm/transparent_hugepage/enabled
# [always] madvise never → [always] = THP enabled → less TLB pressure

# Check hugepage count
cat /proc/meminfo | grep -i huge
# HugePages_Total, HugePages_Free, HugePages_Rsvd
# Hugepagesize: 2048 kB  (2MB huge pages on ARM64)

# Kernel crash debugging: look for TLB-related oops
dmesg | grep -i "tlb\|translation\|page fault\|SIGBUS"
```

---

## 4. Kernel Tracepoints for TLB

```bash
# Available TLB tracepoints
ls /sys/kernel/debug/tracing/events/tlb/
# tlb_flush — called on every TLB flush in Linux

# Enable TLB flush tracing
echo 1 > /sys/kernel/debug/tracing/events/tlb/tlb_flush/enable
cat /sys/kernel/debug/tracing/trace
# Output format:
#   <process>-PID [CPU] TIMESTAMP: tlb_flush: pages=1 reason=TLB_FLUSH_ON_TASK_SWITCH
#   <process>-PID [CPU] TIMESTAMP: tlb_flush: pages=512 reason=TLB_FLUSH_ON_UNMAP

# TLB flush reasons (include/trace/events/tlb.h):
#   TLB_FLUSH_ON_TASK_SWITCH = 0  ← context switch
#   TLB_FLUSH_ON_REMOTE_SHOOTDOWN = 1  ← mprotect/munmap remote flush
#   TLB_FLUSH_ON_LOCAL_MM_SHOOTDOWN = 2  ← local process flush
#   TLB_FLUSH_ON_TLB_FLUSH = 3  ← explicit kernel flush

# Filter: only show remote shootdowns (expensive ones)
echo 'reason == 1' > /sys/kernel/debug/tracing/events/tlb/tlb_flush/filter

# Count per reason
cat /sys/kernel/debug/tracing/trace | grep "reason=1" | wc -l
```

---

## 5. TLB Debugging via KASAN/KFENCE

```
TLB-related bugs to watch for:

1. Use-after-free via stale TLB:
   Page freed, PTE removed, TLB not flushed (missing TLBI)
   CPU accesses VA via stale TLB entry → accesses freed/reallocated page
   Symptom: data corruption without segfault (TLB hit bypasses PTE permission check)
   
   KASAN detection: shadow memory poisoning catches use-after-free
   But: if TLB stale entry has valid permissions → KASAN may not catch it
   KFENCE: quarantines freed memory → detects stale TLB access

2. Wrong ASID:
   Process A's TLB entries tagged with ASID=5
   After rollover: Process B allocated ASID=5
   If TLB not flushed: Process B sees Process A's TLB entries!
   
   This is why flush_context() is critical and must be correct.
   Kernel self-test: arch/arm64/mm/context.c has WARN_ON checks
   
3. Missing DSB after TLBI:
   TLBI issued without DSB → TLB invalidation may not complete
   Other CPUs may still use old TLB entry
   Hard to reproduce: timing-dependent, only seen under high load
   
   Debugging: 
     Add WARN_ON(!dsb_executed_after_tlbi) (compile-time check impossible)
     Code audit: every TLBI must be followed by DSB ISH before any dependent access
     
4. KPTI trampoline not global (nG=1 bug):
   If trampoline page accidentally set nG=1:
   After context switch to different ASID: trampoline TLB entry gone
   Exception entry → TLB miss on vector address → second-level handler
   Symptom: system hangs or panics at exception entry
   
5. Stage 2 VMID stale:
   KVM: VM memory remapped but TLBI IPAS2E1IS not issued
   Guest access: TLB hit → old HPA → wrong physical memory!
   Detection: KVM selftest suite, kvm-unit-tests

Tools for TLB bug investigation:
   perf mem record: record memory access latencies (high = TLB miss storm)
   perf c2c record: cache-to-cache sharing (related to coherency TLB effects)
   ftrace: trace mm_fault, tlb_flush, and related events together
```

---

## 6. Performance Optimization Checklist

```
TLB optimization checklist (in order of impact):

□ Check TLB miss rate:
  perf stat -e dTLB-load-misses,dTLB-load-misses:u ./workload
  Target: < 0.1% for dTLB, < 0.01% for iTLB
  
□ Enable THP for large workloads:
  madvise(buf, size, MADV_HUGEPAGE) for buffers > 1 MB
  Verify: /proc/PID/smaps → AnonHugePages count
  
□ Align allocations to 2MB for THP eligibility:
  mmap(NULL, size, PROT_RW, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)
  Mmap returns 2MB-aligned base (if kernel allocates huge page)
  
□ Use HugeTLB for critical paths:
  /proc/sys/vm/nr_hugepages: pre-allocate huge pages
  Application: mmap with MAP_HUGETLB
  
□ Check ASID rollover rate:
  grep tlb_remote_flush /proc/vmstat → high count = ASID pressure?
  (ASID rollover triggers full TLB flush → high cost)
  
□ Check TLB shootdown rate:
  grep nr_tlb_remote_flush /proc/vmstat
  High count → mprotect/munmap called frequently on large ranges
  Fix: batch page table operations, use larger mappings
  
□ Use 1GB pages for fixed large mappings (e.g., GPU VRAM):
  Requires kernel and application support
  1 TLB entry per GB → essentially eliminates TLB pressure for large contiguous regions
  
□ NUMA awareness:
  Ensure threads access pages on their local NUMA node
  Cross-NUMA access doesn't cause TLB misses but multiplies latency
```

---

## 7. Interview Questions & Answers

**Q1: How would you diagnose a performance regression suspected to be TLB-related?**

Start with `perf stat -e dTLB-load-misses,dTLB-store-misses,iTLB-load-misses ./workload` to confirm TLB misses are elevated. Check the miss RATE (misses / accesses, not raw count): > 1% for dTLB is a red flag. Then use `perf record -e dTLB-load-misses:u -g ./workload && perf report` to identify which functions are triggering most misses — the call graph shows whether misses are in the application's critical path (hot loops), allocator (fragmented allocations), or library code.

Next, check if the working set exceeds TLB reach: `L2 TLB entries × page_size`. For 1024 L2 entries × 4KB = 4MB reach. If the hot data > 4MB, you have TLB thrashing. Solution: `madvise(MADV_HUGEPAGE)` on the hot buffer to use 2MB pages → 1024 × 2MB = 2GB reach.

Also check `/proc/vmstat` for `nr_tlb_remote_flush` — high count means excessive TLB shootdowns, possibly from frequent `mprotect()`/`munmap()` on large ranges. Profile with `echo 1 > /sys/.../events/tlb/tlb_flush/enable` to confirm, then batch the mprotect operations or reduce mapping granularity.

---

## 8. Quick Reference: TLB PMU Events

| perf Alias | ARM Event ID | Description |
|---|---|---|
| `dTLB-load-misses` | L1D_TLB_REFILL (0x05) | L1 dTLB miss on loads |
| `dTLB-store-misses` | L1D_TLB_REFILL (0x05) | L1 dTLB miss on stores |
| `iTLB-load-misses` | L1I_TLB_REFILL (0x01) | L1 iTLB misses |
| `r002F` (raw) | L2D_TLB_REFILL (0x2F) | L2 TLB miss |
| `r0034` (raw) | DTLB_WALK (0x34) | Data PTW count |
| `r0035` (raw) | ITLB_WALK (0x35) | Instruction PTW count |

| /proc/vmstat Key | Indicates |
|---|---|
| `nr_tlb_remote_flush` | Cross-CPU TLB shootdowns |
| `nr_tlb_local_flush_all` | Full local TLB flushes |
| `nr_tlb_local_flush_one` | Single page local TLB flushes |
