# Q01 — Design a Linux Process Scheduler Optimized for GPU-Heavy Workloads

---

## 1. Problem Statement

Modern GPU-heavy workloads (ML training, inferencing, CUDA kernels, rendering pipelines) expose a fundamental mismatch in the Linux CFS scheduler: CFS was designed for interactive/CPU-bound fairness, not for tasks whose runtime is overwhelmingly dictated by GPU execution. A GPU-bound process spends most of its time blocked in DRM/i915/NVIDIA wait queues while its CPU-side thread is periodically woken to submit new work. CFS penalizes such short-burst CPU consumers by treating them identically to fully CPU-bound tasks. The result is scheduling latency jitter on the CPU side that stalls GPU pipeline submission queues.

**Goal:** Design a scheduler extension or replacement policy that:
- Minimizes CPU-side dispatch latency for GPU command submission threads.
- Co-schedules CPU threads with their associated GPU execution contexts.
- Avoids starving non-GPU workloads.
- Works within (or cleanly extends) the Linux scheduling framework.

---

## 2. Requirements

### 2.1 Functional Requirements
- Prioritize threads that have GPU work pending in flight (GPU-runnable state awareness).
- Track GPU context completion events and immediately reschedule the associated CPU thread.
- Support heterogeneous workloads: mixed GPU-heavy + latency-sensitive + background tasks on the same machine.
- Integrate with DRM scheduler (`drm_gpu_scheduler`) and NVIDIA's kernel-mode driver command queues.
- Expose a scheduling policy selectable via `sched_setscheduler()` or cgroup attribute.

### 2.2 Non-Functional Requirements
- GPU submission latency target: < 10 µs from GPU fence signal to CPU thread on-CPU.
- No regression in CPU-only workload throughput (< 2% degradation).
- Scalable to 512-core NUMA systems.
- No unbounded priority inversion for non-GPU tasks.

---

## 3. Constraints & Assumptions

- Kernel version: Linux 6.x with `SCHED_EXT` (sched_ext BPF scheduler framework, merged in 6.12).
- GPU: NVIDIA Ampere/Hopper or AMD RDNA — driver exposes completion fences via DRM sync objects.
- Each GPU context is associated with exactly one or more CPU threads (the submitter threads).
- Hugepages and NUMA topology are available for data placement decisions.

---

## 4. Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                     User Space                               │
│  [CUDA Runtime]  [OpenCL]  [Vulkan]  [Custom ML Framework]  │
│        │               │        │                            │
│        └───────────────┴────────┘                            │
│                        │  syscall / ioctl                    │
└────────────────────────┼─────────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────────┐
│                   Kernel Space                               │
│                                                              │
│  ┌─────────────────────────────────────────────────────┐    │
│  │           GPU-Aware Scheduler (sched_ext BPF)       │    │
│  │                                                      │    │
│  │  ┌──────────────┐   ┌──────────────────────────┐    │    │
│  │  │ GPU Context   │   │  CPU Run Queue           │    │    │
│  │  │ Tracking Map  │◄──│  (per-CPU, RB-tree)      │    │    │
│  │  │ (BPF hashmap) │   └──────────────────────────┘    │    │
│  │  └──────┬───────┘                                    │    │
│  │         │ fence_cb                                   │    │
│  │  ┌──────▼───────────────────────────────────────┐   │    │
│  │  │        DRM Fence Completion Notifier         │   │    │
│  │  │  drm_sched_fence → sched_ext ops→enqueue     │   │    │
│  │  └──────────────────────────────────────────────┘   │    │
│  └─────────────────────────────────────────────────────┘    │
│                                                              │
│  ┌──────────────────────┐  ┌───────────────────────────┐    │
│  │   DRM GPU Scheduler  │  │  NVIDIA/AMD KMD           │    │
│  │  drm_gpu_scheduler   │  │  (command ring, fences)   │    │
│  └──────────────────────┘  └───────────────────────────┘    │
└──────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 GPU Context Descriptor (kernel-side)
```c
struct gpu_sched_ctx {
    u64                 ctx_id;           /* DRM file-private context ID */
    struct task_struct *submitter;        /* CPU thread owning this ctx */
    atomic_t            pending_fences;  /* # GPU ops in-flight */
    u64                 last_submit_ns;  /* ktime of last GPU submit */
    u64                 avg_gpu_runtime; /* EWMA of GPU execution time */
    struct list_head    node;            /* linked into per-task list */
    /* cache-line pad to avoid false sharing across CPUs */
    u8                  __pad[CACHE_LINE_SIZE - sizeof(...)];
};
```

### 5.2 Per-Task GPU Affinity Metadata (extends task_struct via task_struct::android_vendor_data or sched_ext task_local)
```c
struct gpu_task_ext {
    u32  gpu_priority;          /* boosted scheduling priority */
    bool gpu_runnable;          /* fence signaled, not yet on CPU */
    u64  fence_signal_ts;       /* timestamp of last fence signal */
    u32  gpu_ctx_count;         /* number of associated GPU contexts */
    /* pinned wakeup CPU for locality */
    int  preferred_cpu;
};
```

### 5.3 BPF Maps (sched_ext program)
```c
/* Key: tgid, Value: gpu_task_ext */
BPF_MAP_TYPE_HASH: gpu_task_map (max 64k entries)

/* Key: fence_seqno, Value: task tgid */
BPF_MAP_TYPE_HASH: fence_task_map (max 256k entries)

/* Per-CPU queue for GPU-runnable tasks */
BPF_MAP_TYPE_QUEUE: gpu_ready_queue[NR_CPUS]
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Scheduling Classes (existing Linux hierarchy)
```
SCHED_DEADLINE > SCHED_FIFO/RR > SCHED_FAIR (CFS) > SCHED_IDLE
```
GPU submitter threads must **not** use `SCHED_FIFO` unconditionally — that causes starvation. Instead, we add a **dynamic priority boost** within `SCHED_FAIR` when `gpu_runnable == true`, equivalent to what the kernel already does for waking tasks via `ENQUEUE_WAKEUP` + `sched_feat(NEXT_BUDDY)`.

### 6.2 Fence Completion → Immediate Wakeup Path

The critical path is: GPU interrupt → DRM fence signal → CPU submitter thread scheduled.

**Current (broken) path:**
```
GPU IRQ → tasklet → dma_fence_signal() → wake_up_all(&fence->cb_list)
       → normal CFS wakeup → task placed in runqueue → scheduled next tick
```

**Proposed path with GPU-aware scheduler:**
```
GPU IRQ → tasklet → dma_fence_signal()
       → fence_cb registered by our sched layer
       → scx_bpf_kick_cpu(preferred_cpu)    ← force immediate reschedule
       → GPU-ready queue head position       ← bypasses FIFO order
       → submitter thread on-CPU in < 10 µs
```

Implementation: register a `dma_fence_cb` during `ioctl(DRM_IOCTL_SYNCOBJ_WAIT)` that calls `scx_bpf_dispatch()` with a high-priority DSQ (dispatch queue).

### 6.3 CPU Affinity & Cache Locality

GPU command submission writes to **PCIe MMIO BARs** (or NVLink). The optimal CPU for submission is the one topologically closest to the GPU's PCIe root port. At init time:
1. Walk `pci_dev` topology via `pci_bus_get_device()` → find NUMA node of GPU.
2. Store `preferred_cpu = cpumask_first(cpumask_of_node(gpu_numa_node))`.
3. Pin the submitter thread's `preferred_cpu` as the wake-target.

### 6.4 EWMA-Based GPU Runtime Estimation

To avoid over-boosting tasks that have burst-submitted then gone idle:
```
avg_gpu_runtime = (7 * avg_gpu_runtime + current_runtime) >> 3
```
If `avg_gpu_runtime` drops below threshold (e.g., GPU idle for > 5ms), demote back to normal CFS priority. This prevents priority inversion on workloads that phase-shift between GPU and CPU.

### 6.5 Multi-GPU Context Handling

A single task may have N GPU contexts (e.g., CUDA MPS server). We maintain a list of `gpu_sched_ctx` per task. The task is considered GPU-runnable if **any** fence in any context has signaled and not yet been processed. `pending_fences` is an atomic counter: incremented on submit, decremented on fence signal.

---

## 7. Trade-off Analysis

| Decision | Option A (chosen) | Option B (rejected) | Reason |
|---|---|---|---|
| Scheduling policy | sched_ext BPF boost in SCHED_FAIR | SCHED_FIFO with ceiling | FIFO causes starvation; BPF is safe & dynamic |
| Wakeup mechanism | `scx_bpf_kick_cpu` from IRQ context | Standard `wake_up_process` | Avoids missed wakeup race; forces IPI |
| CPU affinity | NUMA-aware, GPU-adjacent CPU | Unrestricted | PCIe DMA latency is NUMA-sensitive |
| Priority decay | EWMA-based demotion | Hard timer reset | EWMA adapts to workload phase transitions |
| Context tracking | BPF hashmap | kernel-space linked list | BPF hashmap is lockless, per-CPU safe |

**Latency vs Throughput:**
- Bias toward low latency for GPU submitter threads (maximize GPU utilization).
- Background CPU tasks get lower priority; acceptable since GPU is the bottleneck.

---

## 8. Real Linux Kernel References

| Component | Source Location | Notes |
|---|---|---|
| CFS scheduler | `kernel/sched/fair.c` | `enqueue_task_fair()`, `pick_next_task_fair()` |
| sched_ext framework | `kernel/sched/ext.c` | BPF ops: `scx_ops_enqueue`, `scx_ops_dispatch` |
| DRM GPU scheduler | `drivers/gpu/drm/scheduler/sched_main.c` | `drm_sched_entity`, `drm_sched_fence` |
| DMA fence | `drivers/dma-buf/dma-fence.c` | `dma_fence_add_callback()`, `dma_fence_signal()` |
| NUMA topology | `include/linux/topology.h` | `cpumask_of_node()`, `cpu_to_node()` |
| Task struct | `include/linux/sched.h` | `struct task_struct`, `se.vruntime` |
| PCIe NUMA node | `drivers/pci/pci.c` | `pci_dev->dev.numa_node` |
| Wakeup preemption | `kernel/sched/fair.c:wakeup_preempt_entity()` | controls when newly woken task preempts current |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Priority Inversion
**Scenario:** GPU submitter holds a mutex; normal task waits on it but GPU submitter is not being scheduled.
**Detection:** `lockdep` + `CONFIG_PREEMPT_RT`; look for priority inversion warnings.
**Fix:** PI-mutex for locks held across GPU submission.

### 9.2 Missed Fence Wakeup
**Scenario:** Race between fence signal and task going to sleep — signal fires before `dma_fence_add_callback()` registers.
**Detection:** High GPU utilization but low CPU submission rate; `perf trace` shows long sleep on DRM syscall.
**Fix:** `dma_fence_add_callback()` returns `-ENOENT` if already signaled — handle inline, do not sleep.

### 9.3 CPU Hotplug
**Scenario:** `preferred_cpu` is offlined.
**Fix:** Register `cpu_notifier`; on `CPU_DEAD`, reselect `preferred_cpu` from same NUMA node.

### 9.4 Debug Tooling
```bash
# Trace fence signal → wakeup latency
sudo bpftrace -e 'kprobe:dma_fence_signal { @ts[arg0] = nsecs; }
                  kprobe:try_to_wake_up   { @lat = nsecs - @ts[arg0]; print(@lat); }'

# Watch sched_ext dispatch queue depth
cat /sys/kernel/sched_ext/stats/dsq_nr_queued

# ftrace: scheduler latency
echo 'sched_wakeup sched_switch' > /sys/kernel/debug/tracing/set_event
cat /sys/kernel/debug/tracing/trace_pipe
```

---

## 10. Performance Considerations

- **Cache-line alignment:** `gpu_sched_ctx` padded to 64 bytes — prevents false sharing when multiple CPUs update `pending_fences` of different contexts.
- **Lock-free fast path:** BPF hashmaps use per-CPU map-in-map; the fence callback runs in softirq context — no sleeping locks.
- **IPI cost:** `scx_bpf_kick_cpu()` sends an IPI. On a 512-core system, limit IPI to cases where `preferred_cpu != current_cpu` and GPU latency delta exceeds threshold.
- **vruntime manipulation:** Boosted tasks get `vruntime -= boost_delta` on enqueue — they are placed at the head of the CFS red-black tree without requiring a new scheduling class.
- **NUMA balancing interference:** Disable automatic NUMA balancing (`kernel.numa_balancing=0`) for GPU submitter threads — NUMA migration of a GPU-submitter increases PCIe traversal cost.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. You know `drm_gpu_scheduler` internals, not just "CFS has nice values."
2. You understand the fence → wakeup → schedule pipeline end-to-end.
3. You cite real trade-offs: sched_ext vs SCHED_FIFO vs cgroup CPU shares.
4. You mention `sched_ext` (merged in 6.12) as the right extension point — shows you track upstream.
5. You bring up NUMA topology and PCIe root complex locality for GPU-adjacent CPU selection.
6. You address starvation prevention explicitly — NVIDIA reviewers always probe for this.
7. You know how to measure: `bpftrace`, `perf sched`, `trace-cmd`, `/proc/schedstat`.
