# MPAM — Memory Partitioning and Monitoring

Category: Performance Engineering and Benchmarking  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

MPAM (Memory Partitioning and Monitoring) is ARM's hardware mechanism for partitioning shared memory system resources — primarily cache capacity and memory bandwidth — among concurrent workloads.

ARM equivalent of Intel RDT (Resource Director Technology).

Goals:
- isolate latency-critical workloads from noisy neighbors
- enforce QoS at the hardware level
- monitor per-workload memory resource usage

---

## 2. ARM64 Hardware Detail

### 2.1 MPAM resources

MPAM can partition and monitor:
- **cache portions** (CPOR — Cache Portion partitioning)
- **memory bandwidth** (MBA — Memory Bandwidth Allocation)
- **cache occupancy monitoring** (CSU)
- **memory bandwidth monitoring** (MBWU)

### 2.2 PARTID and PMG

Each memory request carries:
- **PARTID** — partition identifier (e.g., 0-63 or 0-127)
- **PMG** — performance monitoring group

System components (caches, memory controllers) consult PARTID for resource limits and PMG for monitoring.

### 2.3 MPAM enablement requirements

- CPU implements ARMv8.4-A MPAM extension
- system components (LLC, memory controller) implement MPAM
- firmware exposes MPAM via ACPI MPAM table
- Linux kernel built with `CONFIG_ARM64_MPAM`

---

## 3. Linux Kernel Implementation

### 3.1 resctrl filesystem interface

MPAM is exposed via the `resctrl` pseudo-filesystem (same interface as Intel RDT):

```bash
# Mount resctrl
mount -t resctrl resctrl /sys/fs/resctrl

# Show available resources
ls /sys/fs/resctrl/info/
# L3/ MB/

# Create a partition for critical workload
mkdir /sys/fs/resctrl/critical

# Allocate 80% of L3 cache to critical group
echo "L3:0=ffff0;1=ffff0" > /sys/fs/resctrl/critical/schemata

# Allocate 70% bandwidth
echo "MB:0=70;1=70" > /sys/fs/resctrl/critical/schemata

# Assign PID to partition
echo <pid> > /sys/fs/resctrl/critical/tasks
```

### 3.2 Monitoring usage

```bash
# Create monitoring group
mkdir /sys/fs/resctrl/mon_groups/web

# Assign PIDs to monitoring group
echo <pid> > /sys/fs/resctrl/mon_groups/web/tasks

# Read cache occupancy (bytes)
cat /sys/fs/resctrl/mon_groups/web/mon_data/mon_L3_00/llc_occupancy

# Read memory bandwidth (bytes/sec)
cat /sys/fs/resctrl/mon_groups/web/mon_data/mon_L3_00/mbm_total_bytes
```

### 3.3 Practical partitioning strategy

1. **identify critical workloads** — latency SLO targets
2. **measure current cache/bandwidth usage** with monitoring before partitioning
3. **allocate generously** to critical workloads (50-80% headroom)
4. **constrain noisy neighbors** to a smaller share
5. **validate** with workload tail-latency measurement
6. **iterate** — too tight a limit can hurt the critical workload

---

## 4. Hardware-Software Interaction

MPAM moves QoS enforcement from software policy (cgroups, schedulers) to hardware. Benefits:
- enforcement happens at every memory access, not periodically
- no software overhead for enforcement
- hard isolation guarantees that cgroups cannot provide

Limitations:
- requires hardware support (newer Neoverse cores)
- granularity limited by hardware partition count
- partitioning cache reduces total available capacity for non-partitioned workloads

---

## 5. Interview Q and A

Q1: What is the relationship between MPAM and Intel RDT?  
Functional equivalents. MPAM is ARM's implementation with similar capabilities exposed via the same Linux `resctrl` interface.

Q2: What does PARTID identify?  
A partition that determines resource limits (cache portion, bandwidth) applied to memory requests.

Q3: Why monitor before partitioning?  
To establish baseline usage; partitioning blindly can starve workloads or waste capacity.

Q4: Can MPAM partition L1/L2 caches?  
Typically only shared caches (LLC). Private L1/L2 are inherently per-core.

Q5: What happens if a workload exceeds its bandwidth allocation?  
Memory requests are throttled at the controller, increasing observed latency for that workload.

Q6: When is MPAM not the right solution?  
For workloads that need bursty access to full cache/bandwidth — partitioning would degrade them.

---

## 6. Pitfalls and Gotchas

- Assuming MPAM is available on all ARMv8.4+ — also requires system component support and firmware exposure.
- Over-partitioning: leaving only 10% for "everything else" starves background work.
- Forgetting to assign PIDs to partitions — they stay in the default group.
- Treating MPAM as a fix for poorly written software — it isolates but does not optimize.

---

## 7. Quick Reference Table

| Capability | Resource | Linux Interface |
|---|---|---|
| cache partitioning | LLC portion | `resctrl/schemata L3:` |
| bandwidth allocation | memory bandwidth % | `resctrl/schemata MB:` |
| cache occupancy monitoring | per-partition LLC bytes | `mon_data/.../llc_occupancy` |
| bandwidth monitoring | per-partition MB/s | `mon_data/.../mbm_total_bytes` |
