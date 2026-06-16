# BPF and perf for Real-Time Memory Diagnosis

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

BPF and perf give safe, low-overhead visibility into live systems without recompiling the kernel.

For memory incidents, they enable:
- tracing live allocation and reclaim paths
- attributing page faults to specific workloads
- measuring translation miss rates during pressure events

---

## 2. ARM64 Hardware Detail

### 2.1 ARM64 hardware performance counters

ARM PMU exposes memory-relevant events:
- `L1D_CACHE_REFILL` — L1 data cache misses
- `L2D_CACHE_REFILL` — L2 misses
- `MEM_ACCESS` — total memory access count
- `DTLB_WALK` / `ITLB_WALK` — TLB miss counts
- `BUS_ACCESS` — traffic to main memory

### 2.2 CoreSight integration

ARM64 platforms with CoreSight support can trace memory access patterns at hardware level for post-mortem or offline profiling.

---

## 3. Linux Kernel Implementation

### 3.1 BPF-based tracing for memory incidents

Common bpftrace one-liners:

```bash
# Trace kmalloc allocation sites by size
bpftrace -e 'kprobe:__kmalloc { @size[ustack] = sum(arg0); }'

# Track direct reclaim entry by process
bpftrace -e 'kprobe:try_to_free_pages { @reclaim[comm] = count(); }'

# Observe page fault rate per cgroup
bpftrace -e 'software:major-faults:1 { @[cgroup] = count(); }'

# Monitor THP allocation fallbacks
bpftrace -e 'kprobe:__alloc_pages_slowpath { @[comm] = count(); }'
```

### 3.2 perf commands for ARM64 memory analysis

```bash
# TLB miss rates
perf stat -e DTLB_WALK,ITLB_WALK,MEM_ACCESS -p <pid> sleep 5

# LLC miss profile
perf stat -e cache-misses,cache-references -a sleep 10

# Memory latency flamegraph
perf mem record -a sleep 30 ; perf mem report

# NUMA access distribution
perf stat -e REMOTE_ACCESS -a sleep 10
```

### 3.3 Safe production practices

- use sampling (not full tracing) under high load
- avoid kprobes on ultra-hot paths without rate limiting
- prefer BPF ring buffers over perf_event_array for high-frequency events

---

## 4. Hardware-Software Interaction

ARM64 PMU data gives hardware ground truth; BPF provides software-level causality (which process, which code path, which cgroup). Combining both converts hardware signal into actionable software diagnosis.

---

## 5. Interview Q and A

Q1: Why prefer BPF over kernel logs for live diagnosis?  
BPF is low-overhead, targeted, and does not require kernel recompilation or restarts.

Q2: What ARM64 PMU event tells you about TLB miss rate?  
`DTLB_WALK` for data TLB and `ITLB_WALK` for instruction TLB.

Q3: How do you attribute page faults to a specific cgroup with BPF?  
Use the `cgroup` built-in in bpftrace on a `software:major-faults` event.

Q4: Why avoid full tracing on hot paths in production?  
Even BPF tracing at high frequency on hot paths adds measurable overhead.

Q5: What is the advantage of `perf mem` over `perf stat`?  
It profiles memory access latency distribution rather than just aggregate counts.

Q6: How do you correlate PMU events with PSI trends?  
Align PMU sample timestamps with PSI windows to identify which workload phase drives pressure.

---

## 6. Pitfalls and Gotchas

- Enabling full allocation tracing without sampling — can double allocator overhead.
- Using kprobes on `kmalloc` in interrupt-heavy paths without entry guards.
- Reading PMU event names from x86 documentation — ARM64 PMU names differ.
- Forgetting rate-limiting in bpftrace scripts running under high event frequency.

---

## 7. Quick Reference Table

| Tool | Use Case | ARM64 Specifics |
|---|---|---|
| `perf stat -e DTLB_WALK` | TLB miss rate | ARM PMU event name |
| `bpftrace kprobe:try_to_free_pages` | reclaim attribution | per-process/cgroup |
| `perf mem record` | access latency profiling | requires PMU mem events |
| CoreSight | hardware trace | platform-specific enablement |
