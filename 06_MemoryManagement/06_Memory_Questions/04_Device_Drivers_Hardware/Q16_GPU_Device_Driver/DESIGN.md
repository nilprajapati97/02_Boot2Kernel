# Q16 — Design a GPU Device Driver Architecture (User Space + Kernel Space Interaction)

---

## 1. Problem Statement

A GPU device driver is the most complex type of Linux driver. It must bridge:
- User-space graphics/compute APIs (CUDA, OpenGL, Vulkan) to hardware registers.
- Multiple address spaces (CPU virtual, GPU virtual, VRAM physical).
- Asynchronous execution (GPU runs independent of CPU).
- Shared resources across multiple processes and VMs.
- Power management, fault recovery, and security isolation.

Design a complete GPU driver architecture covering the user-kernel interface, memory management, command submission, interrupt handling, and fault recovery.

---

## 2. Requirements

### 2.1 Functional Requirements
- Userspace ↔ kernel interface: `ioctl`, `mmap`, `poll`/`epoll`.
- GPU virtual address space management per-process (per-VM page tables).
- VRAM allocation and migration (VRAM ↔ system RAM for oversubscription).
- Command buffer submission to GPU execution engines.
- Synchronization: GPU fences, semaphores for ordering across engines.
- DRM (Direct Rendering Manager) integration for display output.
- GPU context isolation: one process's GPU access cannot affect another's.

### 2.2 Non-Functional Requirements
- Command submission overhead: < 5 µs from `ioctl` to GPU execution start.
- GPU fault isolation: one context faulting should not crash other contexts.
- Memory security: GPU pages zeroed before mapping to a new context.
- Preemption: long-running GPU kernels must be preemptible.

---

## 3. Constraints & Assumptions

- Linux DRM subsystem (`drivers/gpu/drm/`).
- NVIDIA-like GPU: multiple engines (compute, copy, video decode), separate VRAM.
- PCIe interconnect with GPUDirect RDMA capability.
- GPU has its own MMU (GPU memory management unit) separate from CPU MMU.
- KMD (Kernel Mode Driver) + UMD (User Mode Driver) split (like NVIDIA's architecture).

---

## 4. Architecture Overview

```
  User Space
  ┌──────────────────────────────────────────────────────────────────┐
  │  CUDA/OpenCL/Vulkan App                                         │
  │         │                                                        │
  │  UMD (libcuda.so / libvulkan.so)                                │
  │    │ Encodes GPU commands into command buffers                   │
  │    │ Manages CPU-side caches, JIT compilation                   │
  │    │ ioctl(DRM_IOCTL_SUBMIT) → submits to KMD                  │
  └────┼────────────────────────────────────────────────────────────┘
       │  syscall boundary
  ┌────▼────────────────────────────────────────────────────────────┐
  │  Kernel Mode Driver (KMD)                                       │
  │                                                                  │
  │  ┌──────────────┐  ┌───────────────┐  ┌────────────────────┐  │
  │  │  File Ops    │  │  GPU VM Mgmt  │  │  Command Scheduler │  │
  │  │  open/close  │  │  GPU page     │  │  drm_gpu_scheduler │  │
  │  │  ioctl/mmap  │  │  tables, VRAM │  │  per-engine queues │  │
  │  └──────────────┘  └───────────────┘  └────────────────────┘  │
  │  ┌──────────────┐  ┌───────────────┐  ┌────────────────────┐  │
  │  │  Memory Mgr  │  │  Fence/Sync   │  │  Interrupt Handler │  │
  │  │  TTM / GEM   │  │  dma_fence    │  │  MSI-X per engine  │  │
  │  └──────────────┘  └───────────────┘  └────────────────────┘  │
  └──────────────────────────────────────────────────────────────────┘
       │  MMIO BAR + DMA
  ┌────▼────────────────────────────────────────────────────────────┐
  │  GPU Hardware                                                   │
  │  [Compute Engine 0..N] [Copy Engine 0..M] [Video Decode]       │
  │  [GPU MMU] [VRAM] [PCI-e interface]                             │
  └──────────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 DRM File Private (per-process GPU context)

```c
struct my_gpu_file_priv {
    struct drm_file   *file;        /* DRM file handle */
    struct my_gpu_vm  *vm;          /* GPU virtual address space */
    struct list_head   bo_list;     /* buffer objects owned by this process */
    struct idr         context_idr; /* GPU execution context handles */
    struct mutex       lock;
};
```

### 5.2 Buffer Object (GPU memory allocation — GEM/TTM)

```c
struct my_gpu_bo {
    struct ttm_buffer_object tbo;     /* TTM base object (eviction, migration) */
    struct drm_gem_object    gem;     /* DRM GEM base (refcount, mmap) */

    /* Physical location */
    enum my_gpu_domain {
        MY_GPU_DOMAIN_VRAM,           /* in GPU VRAM */
        MY_GPU_DOMAIN_GTT,            /* in system RAM, GPU-accessible via GTT */
        MY_GPU_DOMAIN_CPU,            /* CPU-only, not GPU-accessible */
    } domain;

    /* GPU virtual mapping */
    u64               gpu_va;        /* GPU virtual address (0 if not mapped) */
    struct my_gpu_vm_mapping *vm_mapping;

    /* DMA fence (last GPU operation on this BO) */
    struct dma_fence *fence;         /* GPU is done when signaled */
    struct dma_resv  *resv;          /* reservation object (shared/exclusive fences) */
};
```

### 5.3 GPU Virtual Memory (per-process GPU address space)

```c
struct my_gpu_vm {
    struct my_gpu_device    *dev;
    struct amdgpu_vm_pt      root;     /* root page directory (GPU-accessible) */
    struct drm_gpu_scheduler *sched;

    /* Page table update command buffers */
    struct my_gpu_bo        *pdb;      /* page directory buffer (VRAM) */
    struct my_gpu_bo        *ptb;      /* page table buffer (VRAM) */

    struct rb_root_cached    va;       /* rb-tree of GPU VA ranges */
    struct mutex             lock;
};
```

### 5.4 GPU Execution Context + Job

```c
struct my_gpu_context {
    u32                      hw_id;      /* hardware context ID */
    struct my_gpu_vm        *vm;
    struct drm_gpu_scheduler *sched;
    struct drm_sched_entity  entity;     /* submission entity */
};

struct my_gpu_job {
    struct drm_sched_job     base;       /* DRM scheduler base */
    struct my_gpu_context   *ctx;
    struct my_gpu_bo        *cmd_bo;     /* command buffer BO */
    u64                      cmd_offset; /* offset into cmd_bo */
    u32                      cmd_size;
    struct dma_fence        *done_fence; /* signaled when job completes */
    /* Dependencies */
    struct xarray            deps;       /* fences to wait for before submit */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 ioctl Interface Design

The primary UMD↔KMD interface is `ioctl`. Key design principles:
1. **No blocking in ioctl:** Return immediately; GPU work is asynchronous.
2. **Fence-based synchronization:** Return a fence FD; user waits on it with `poll()`.
3. **Input validation:** Never trust user-provided GPU addresses or sizes.

```c
/* Example: ioctl to submit a command buffer */
struct drm_my_gpu_submit {
    __u32  ctx_id;          /* execution context */
    __u64  cmd_addr;        /* GPU VA of command buffer */
    __u32  cmd_size;        /* command buffer size */
    __u32  num_deps;        /* number of input fences */
    __u64  deps_ptr;        /* user pointer to array of sync file FDs */
    __s32  out_fence_fd;    /* OUTPUT: sync file FD for this job's completion */
};

int my_gpu_ioctl_submit(struct drm_device *dev, void *data,
                        struct drm_file *file_priv)
{
    struct drm_my_gpu_submit *args = data;
    struct my_gpu_job *job;

    /* 1. Validate context */
    ctx = idr_find(&file_priv->context_idr, args->ctx_id);
    if (!ctx) return -EINVAL;

    /* 2. Build job */
    job = kzalloc(sizeof(*job), GFP_KERNEL);
    drm_sched_job_init(&job->base, &ctx->entity, NULL);

    /* 3. Import dependency fences from user-provided sync file FDs */
    for (i = 0; i < args->num_deps; i++) {
        fence = sync_file_get_fence(dep_fds[i]);
        drm_sched_job_add_dependency(&job->base, fence);
    }

    /* 4. Create output sync file */
    done_fence = &job->base.s_fence->finished;
    sync_file = sync_file_create(done_fence);
    fd_install(args->out_fence_fd, sync_file->file);

    /* 5. Submit to DRM scheduler (non-blocking) */
    drm_sched_entity_push_job(&job->base);

    return 0;
}
```

### 6.2 GPU Memory Management — TTM (Translation Table Manager)

TTM handles GPU buffer lifecycle: allocation, eviction, migration between domains.

```
Allocation request: MY_GPU_DOMAIN_VRAM
    ttm_bo_init() → places BO in VRAM via ttm_mem_type_manager
    If VRAM full:
        ttm_bo_evict_first() → evict cold BO to GTT (system RAM)
        GPU page table updated: BO now at GTT address
        New BO placed in freed VRAM slot

Eviction priority: LRU list per TTM pool
    → evict BO with oldest last-use timestamp
    → but: never evict a BO with pending GPU fence (still in use)
```

### 6.3 GPU Page Table Management

```
GPU MMU has its own page table format (different from CPU x86-64 PT).
GPU VA space: per-context, 40-bit typically.

Page table levels:
    PDE4 (root) → PDE3 → PDE2 → PDE1 → PTE → GPU PA (VRAM or GTT PA)

PTE format:
    [63:12] physical page frame number (VRAM offset or system RAM PA via BAR)
    [11:8]  caching mode (uncached, write-combine, cached)
    [7:4]   access rights (read, write, execute)
    [0]     valid bit

GPU page table update requires:
    1. Allocate new page table buffer (in VRAM or GTT)
    2. Write PTE values into buffer (CPU writes to mapped BO)
    3. Submit GPU update command: GPU MMU reads new PTE values and invalidates TLB
```

### 6.4 Command Submission Pipeline

```
UMD builds command buffer → ioctl(SUBMIT) →
    KMD validates command buffer address (must be in mapped BO) →
    DRM scheduler entity queues job →
    Scheduler resolves dependencies (wait for input fences) →
    Pushes job to HW ringbuffer:
        ring->tail pointer written with command buffer address
        doorbell register written (MMIO write) →
    GPU fetch unit reads commands from ringbuffer →
    GPU executes → writes completion semaphore →
    MSI-X interrupt → fence signaled → waiting tasks woken
```

### 6.5 Preemption — GPU Context Switching

For multi-process GPU sharing, long-running kernels must be preemptible:

**Mid-thread preemption (NVIDIA Volta+, AMD RDNA2+):**
1. Scheduler detects timeout (e.g., 100 ms execution without yield).
2. Issues preemption signal to GPU via MMIO.
3. GPU saves full thread state (registers, stack, PC) to VRAM.
4. New context loaded; resumes execution.
5. Original context resumes when re-scheduled.

**Coarse preemption (older GPUs):**
1. Wait for current draw/dispatch call to complete.
2. Save minimal context (push buffer state).
3. Switch context.

### 6.6 GPU Fault Handling

```
GPU page fault (access to unmapped VA):
    → GPU fault interrupt fires
    → Read fault address, context ID from GPU registers
    → Determine cause: buffer not mapped, wrong permissions, VM context error

On-demand paging (NVIDIA SVM / AMD HMM):
    → Fault → GPU fault handler → CPU page table walk
    → If page present in CPU VA: map to GPU page table → retry
    → If not present: trigger CPU page fault → allocate page → map to GPU

Unrecoverable fault (bad pointer, wild write):
    → Kill the GPU context (not the whole GPU)
    → Send SIGBUS to owning process
    → Other GPU contexts continue unaffected
    → Recover GPU state via soft reset of faulted engine
```

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| UMD/KMD split | Yes | Monolithic kernel driver | UMD in userspace: faster iteration, no kernel recompile |
| Async ioctl + fence FD | Yes | Blocking ioctl | Non-blocking: GPU and CPU overlap execution |
| DRM scheduler | Yes | Custom submit queue | DRM scheduler: standard infrastructure, priority, fairness |
| TTM for buffer mgmt | Yes | Custom allocator | TTM: eviction, migration, coherency handling all built in |
| Per-process GPU VM | Yes | Shared GPU address space | Isolation: one process cannot access another's GPU memory |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| DRM core | `drivers/gpu/drm/drm_drv.c` | `drm_dev_alloc()`, `drm_dev_register()` |
| DRM GEM | `drivers/gpu/drm/drm_gem.c` | `drm_gem_object_init()`, `drm_gem_mmap()` |
| TTM | `drivers/gpu/drm/ttm/` | `ttm_bo_init()`, `ttm_bo_evict()` |
| DRM scheduler | `drivers/gpu/drm/scheduler/sched_main.c` | `drm_sched_entity_push_job()`, `drm_sched_main()` |
| DMA fence | `drivers/dma-buf/dma-fence.c` | `dma_fence_add_callback()`, `dma_fence_signal()` |
| Sync file | `drivers/dma-buf/sync_file.c` | `sync_file_create()`, `sync_file_get_fence()` |
| DMA reservation | `drivers/dma-buf/dma-resv.c` | `dma_resv_lock()`, `dma_resv_add_fence()` |
| AMDGPU reference | `drivers/gpu/drm/amd/amdgpu/` | `amdgpu_cs_ioctl()`, `amdgpu_vm_update_ptes()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 GPU Hang (Engine Timeout)
```bash
dmesg | grep "GPU HANG\|ring.*timeout"
# DRM scheduler timeout triggers engine reset
# nvidia-smi (NVIDIA) or amdgpu_top (AMD) shows GPU state
# Debug: enable DRM scheduler timeout traces
echo 1 > /sys/module/drm/parameters/debug
```

### 9.2 VRAM OOM (Out of Memory)
```bash
# TTM eviction fails: all BOs have active fences
# Fix: ensure all GPU jobs complete before exit (fence wait in cleanup)
# Debug: cat /sys/kernel/debug/dri/0/amdgpu_vram_mm
```

### 9.3 GPU Page Fault
```bash
# dmesg: "GPU fault at VA 0x... context X"
# Decode VA in userspace: addr2line or driver-specific debug tools
# nvidia-debugdump -D   (NVIDIA only)
```

---

## 10. Performance Considerations

- **Command buffer batching:** Submit large command buffers (MB range) instead of many small ioctls. Each ioctl has ~1 µs overhead; batch 1000 commands = 1000× amortization.
- **Doorbell caching:** Doorbell page (MMIO ring tail write) mapped into user space — UMD writes doorbell directly without ioctl for very low-latency submission.
- **VRAM hot objects:** Frequently accessed textures/weights stay in VRAM; cold objects evicted to GTT. TTM LRU controls this automatically.
- **GPU TLB invalidation:** Changing GPU page tables requires TLB shootdown on GPU — expensive. Batch all VA mapping updates before submitting a single TLB invalidation command.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. UMD/KMD split — UMD builds command buffers, KMD validates and submits.
2. DRM scheduler: `drm_sched_entity`, `drm_sched_job`, fence-based dependency resolution.
3. TTM memory management: VRAM → GTT eviction under memory pressure.
4. GPU VM page tables: per-process, GPU-format PTEs, GPU TLB invalidation.
5. Sync file FDs for cross-driver/cross-process GPU synchronization.
6. `dma_resv` for shared buffer reservation (prevents read-write conflicts).
7. GPU preemption: mid-thread (state save) vs coarse (draw-call boundary).
8. Doorbell write from user space — key optimization for low-latency submission.
