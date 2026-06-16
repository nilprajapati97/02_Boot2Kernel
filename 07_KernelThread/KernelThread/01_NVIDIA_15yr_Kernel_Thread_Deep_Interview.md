# 01 — NVIDIA: 15-Year Experience System Design Deep Interview — Kernel Threads

> **Target**: Principal/Staff/Distinguished Engineer interviews at NVIDIA (GPU Driver, CUDA Runtime, Tegra BSP, Autonomous Vehicles, DGX Infrastructure)
> **Level**: 15+ years — You are expected to design kernel threading architectures for GPU driver stacks, CUDA kernel submission, real-time scheduling for autonomous driving, and multi-GPU coordination across NVLink fabric.

---

## 📌 Interview Focus Areas

| Domain | What NVIDIA Expects at 15yr Level |
|--------|----------------------------------|
| **GPU Driver kthreads** | GPU channel scheduler, fault recovery threads, power management daemons |
| **Work Queues vs kthreads** | When to use `kthread_create` vs `alloc_workqueue` vs `tasklet` for GPU work |
| **Real-Time Scheduling** | SCHED_FIFO/SCHED_DEADLINE for autonomous driving sensor fusion pipelines |
| **Multi-GPU Coordination** | Cross-GPU synchronization threads, NVLink fabric monitoring |
| **Kernel Thread Lifecycle** | `kthread_create`, `kthread_stop`, `kthread_should_stop`, clean shutdown |
| **CPU Affinity & Isolation** | Pinning GPU interrupt threads, `isolcpus`, CPU shielding for latency |
| **RCU & Per-CPU kthreads** | RCU callback offloading, softirq threads, ksoftirqd tuning |

---

## 🎨 System Design 1: Design the GPU Driver Kernel Thread Architecture

### Context
NVIDIA's GPU driver (nvidia.ko / nouveau) manages multiple kernel threads for different responsibilities — channel scheduling, fault handling, power management, and GPU-to-GPU communication. At 15 years, you must design this entire threading architecture.

### GPU Driver Thread Architecture

```mermaid
flowchart TD
    subgraph UserSpace["👤 User Space"]
        CUDA["CUDA Application"]
        VULKAN["Vulkan/OpenGL App"]
        NVML["nvidia-smi / NVML"]
    end
    
    subgraph KernelDriver["⚙️ nvidia.ko Kernel Driver"]
        subgraph Submission["📤 Work Submission Threads"]
            CHAN_SCHED["gpu/channel_sched<br/>kthread: per-GPU<br/>SCHED_FIFO prio 50<br/>Processes GPU command ringbuffer"]
            PUSHBUF["gpu/pushbuf_worker<br/>workqueue: WQ_HIGHPRI<br/>DMA maps pushbuffers<br/>Prepares GPU command packets"]
        end
        
        subgraph Fault["🔴 Fault Handling Threads"]
            MMU_FAULT["gpu/mmu_fault<br/>kthread: per-GPU<br/>SCHED_FIFO prio 90<br/>Handles GPU page faults"]
            RECOVERY["gpu/recovery<br/>kthread: per-GPU<br/>SCHED_NORMAL<br/>Full GPU reset & recovery"]
        end
        
        subgraph Power["⚡ Power Management"]
            PM_DAEMON["gpu/pm_daemon<br/>kthread: per-GPU<br/>SCHED_NORMAL<br/>DVFS, thermal throttle"]
            IDLE["gpu/idle_monitor<br/>delayed_work<br/>Checks GPU idle → suspend"]
        end
        
        subgraph NVLink["🔗 NVLink Fabric"]
            NVLINK_MON["gpu/nvlink_monitor<br/>kthread: global<br/>Link health, training"]
            P2P_MIGR["gpu/p2p_migration<br/>workqueue: WQ_UNBOUND<br/>Cross-GPU page migration"]
        end
    end
    
    subgraph Hardware["🖥️ GPU Hardware"]
        GPU0["GPU 0<br/>80GB HBM3"]
        GPU1["GPU 1<br/>80GB HBM3"]
        NVSWITCH["NVSwitch<br/>900 GB/s"]
    end
    
    CUDA --> CHAN_SCHED
    CUDA --> PUSHBUF
    
    CHAN_SCHED --> GPU0
    MMU_FAULT --> GPU0
    PM_DAEMON --> GPU0
    NVLINK_MON --> NVSWITCH
    P2P_MIGR --> GPU1

    style CHAN_SCHED fill:#e74c3c,stroke:#c0392b,color:#fff
    style PUSHBUF fill:#f39c12,stroke:#e67e22,color:#fff
    style MMU_FAULT fill:#c0392b,stroke:#922b21,color:#fff
    style RECOVERY fill:#8e44ad,stroke:#6c3483,color:#fff
    style PM_DAEMON fill:#2ecc71,stroke:#27ae60,color:#fff
    style NVLINK_MON fill:#3498db,stroke:#2980b9,color:#fff
    style P2P_MIGR fill:#9b59b6,stroke:#8e44ad,color:#fff
```

### GPU Channel Scheduling Sequence

```mermaid
sequenceDiagram
    participant APP as CUDA App<br/>[User Space]
    participant IOCTL as nvidia.ko<br/>ioctl handler
    participant RING as Channel Ringbuffer<br/>[shared memory]
    participant KSCHED as gpu/channel_sched<br/>[kthread, SCHED_FIFO]
    participant GPU as GPU Engine<br/>[Hardware]
    participant FENCE as Fence/Semaphore<br/>[GPU Memory]

    APP->>IOCTL: ioctl(NV_SUBMIT_WORK,<br/>{channel, pushbuf, fence_id})
    IOCTL->>RING: Write GP_ENTRY to<br/>channel ringbuffer
    IOCTL->>KSCHED: wake_up_process(channel_sched)
    
    Note over KSCHED: Wakes from wait_event()
    
    KSCHED->>KSCHED: Scan all active channels<br/>Select highest priority work<br/>(TSG group scheduling)
    
    KSCHED->>GPU: Program PBDMA:<br/>GP_PUT → new ringbuf entry
    KSCHED->>GPU: Kick GPU engine:<br/>write HOST doorbell register
    
    GPU->>GPU: Execute compute kernel<br/>(CUDA grid launch)
    
    GPU->>FENCE: Write fence value<br/>(completion semaphore)
    GPU->>KSCHED: Interrupt: PBDMA_DONE
    
    KSCHED->>KSCHED: Process completion<br/>Update channel state
    KSCHED->>APP: Signal fence fd<br/>(eventfd / sync_file)
    
    Note over APP: cuStreamSynchronize()<br/>returns ✅
```

### Deep Q&A

---

#### ❓ Q1: Design the `gpu/channel_sched` kernel thread. What scheduling policy, why per-GPU, and how do you handle priority inversion between CUDA streams?

**A:**

```mermaid
flowchart TD
    A["gpu/channel_sched<br/>kthread_create()"] --> B{Event Loop}
    
    B -->|"wait_event_interruptible<br/>work_pending || kthread_should_stop"| C["Wake up"]
    
    C --> D["Scan TSG runlist<br/>(Time Slice Groups)"]
    D --> E{Select next<br/>channel to run}
    
    E -->|"Priority: HIGH<br/>(CUDA_STREAM_NON_BLOCKING)"| F["Schedule GPU<br/>timeslice = 10ms"]
    E -->|"Priority: NORMAL<br/>(regular CUDA stream)"| G["Schedule GPU<br/>timeslice = 33ms"]
    E -->|"Priority: LOW<br/>(background compute)"| H["Schedule GPU<br/>timeslice = 100ms"]
    
    F --> I["Program PBDMA:<br/>Load channel context<br/>Set GP_PUT register"]
    G --> I
    H --> I
    
    I --> J["Kick GPU doorbell"]
    J --> K{Completion?}
    
    K -->|"Interrupt: DONE"| L["Process fence<br/>Signal user space"]
    K -->|"Timeout: 5sec"| M["Channel stuck!<br/>→ GPU recovery thread"]
    K -->|"Preempt: higher prio"| N["Save channel state<br/>Context switch on GPU"]
    
    L --> B
    M --> O["Wake gpu/recovery kthread"]
    N --> E

    style A fill:#3498db,stroke:#2980b9,color:#fff
    style F fill:#e74c3c,stroke:#c0392b,color:#fff
    style G fill:#f39c12,stroke:#e67e22,color:#fff
    style H fill:#2ecc71,stroke:#27ae60,color:#fff
    style M fill:#c0392b,stroke:#922b21,color:#fff
```

**Implementation:**

```c
/* GPU channel scheduler kthread */

struct gpu_channel_sched {
    struct task_struct *thread;
    struct gpu_device *gpu;
    spinlock_t runlist_lock;
    struct list_head runlist[GPU_SCHED_PRIORITY_COUNT]; /* HIGH, NORMAL, LOW */
    wait_queue_head_t work_wq;
    bool work_pending;
};

static int gpu_channel_sched_fn(void *data)
{
    struct gpu_channel_sched *sched = data;
    struct sched_param param = { .sched_priority = 50 };
    
    /* SCHED_FIFO: Must not be preempted by normal tasks.
     * GPU submission latency directly impacts CUDA kernel launch time.
     * Priority 50: above normal kthreads, below fault handling (90). */
    sched_setscheduler(current, SCHED_FIFO, &param);
    
    /* Pin to CPU next to GPU's NUMA node for cache locality */
    set_cpus_allowed_ptr(current, 
                         cpumask_of_node(dev_to_node(&sched->gpu->pdev->dev)));
    
    while (!kthread_should_stop()) {
        wait_event_interruptible(sched->work_wq,
                                 sched->work_pending || 
                                 kthread_should_stop());
        
        if (kthread_should_stop())
            break;
        
        spin_lock(&sched->runlist_lock);
        sched->work_pending = false;
        
        /* Priority scheduling: scan HIGH first */
        for (int prio = GPU_SCHED_HIGH; prio <= GPU_SCHED_LOW; prio++) {
            struct gpu_channel *chan;
            list_for_each_entry(chan, &sched->runlist[prio], link) {
                if (chan->has_pending_work) {
                    spin_unlock(&sched->runlist_lock);
                    gpu_submit_channel(sched->gpu, chan);
                    goto next_cycle;
                }
            }
        }
        spin_unlock(&sched->runlist_lock);
next_cycle:
        ;
    }
    
    return 0;
}

/* Priority inversion mitigation */
void gpu_handle_priority_inversion(struct gpu_channel_sched *sched,
                                    struct gpu_channel *blocked_high,
                                    struct gpu_channel *blocking_low)
{
    /*
     * Problem: High-priority CUDA stream waits for a semaphore
     * that will be released by a low-priority stream, but a
     * medium-priority stream keeps preempting the low-priority one.
     *
     * Solution: Priority inheritance on GPU channels
     */
    
    /* Temporarily boost blocking channel to HIGH priority */
    int orig_prio = blocking_low->sched_priority;
    
    spin_lock(&sched->runlist_lock);
    list_move(&blocking_low->link, &sched->runlist[GPU_SCHED_HIGH]);
    blocking_low->sched_priority = GPU_SCHED_HIGH;
    blocking_low->inherited_from = blocked_high;
    spin_unlock(&sched->runlist_lock);
    
    /* When low channel releases semaphore, restore original priority */
    blocking_low->on_semaphore_release = restore_priority_callback;
    blocking_low->orig_priority = orig_prio;
}
```

**Key design decisions:**
| Decision | Choice | Reasoning |
|----------|--------|-----------|
| Per-GPU vs global thread | **Per-GPU** | Avoid contention between GPUs; each GPU has independent engines |
| SCHED_FIFO vs SCHED_DEADLINE | **SCHED_FIFO** | GPU submission is event-driven, not periodic — DEADLINE needs known period |
| Priority 50 | **Mid-FIFO range** | Above normal kthreads, below fault handler (90), below watchdog (99) |
| CPU affinity | **NUMA-local** | Minimizes PCIe/NVLink submission latency (cache-warm) |

---

#### ❓ Q2: The GPU hangs during a CUDA kernel execution. Design the fault recovery thread architecture — how do you detect, isolate, and recover without affecting other GPU contexts?

**A:**

```mermaid
sequenceDiagram
    participant WD as GPU Watchdog<br/>[timer, 5sec]
    participant FAULT as gpu/mmu_fault<br/>[kthread, FIFO 90]
    participant RECOV as gpu/recovery<br/>[kthread, NORMAL]
    participant SCHED as gpu/channel_sched
    participant GPU as GPU Hardware
    participant APP1 as CUDA App 1<br/>[faulting]
    participant APP2 as CUDA App 2<br/>[healthy]

    Note over WD: Timer fires every 1 sec<br/>Checks last fence update time

    WD->>WD: Channel 7: no fence progress<br/>for 5 seconds → TIMEOUT
    WD->>FAULT: wake_up(mmu_fault_thread)

    FAULT->>GPU: Read NV_PGRAPH_TRAPPED_STATUS
    FAULT->>FAULT: Error: ILLEGAL_OPCODE<br/>on Channel 7, Context: App1

    alt Per-Channel Recovery (Preferred)
        FAULT->>SCHED: Remove Channel 7 from runlist
        FAULT->>GPU: Preempt Channel 7<br/>(write PREEMPT register)
        GPU->>FAULT: Channel 7 preempted ✅
        FAULT->>GPU: Reset Channel 7 engine<br/>(per-channel reset)
        FAULT->>APP1: Signal error fd:<br/>CUDA_ERROR_LAUNCH_FAILED
        
        Note over APP2: App 2 UNAFFECTED ✅<br/>Continues on other channels
    end
    
    alt Full GPU Recovery (Last Resort)
        Note over FAULT: Per-channel reset failed!<br/>GPU is unresponsive
        FAULT->>RECOV: wake_up(recovery_thread)
        
        RECOV->>SCHED: Pause all channels
        RECOV->>GPU: Full GPU reset<br/>(NV_PMC_ENABLE toggle)
        RECOV->>GPU: Reload firmware<br/>(PMU, SEC2, GSP)
        RECOV->>SCHED: Restore healthy channels<br/>(reload context from shadow)
        RECOV->>APP1: CUDA_ERROR_LAUNCH_FAILED
        RECOV->>APP2: Resume context ✅<br/>(transparent recovery)
    end
```

**Fault recovery implementation:**

```c
/* GPU fault recovery thread */

static int gpu_mmu_fault_thread(void *data)
{
    struct gpu_device *gpu = data;
    struct sched_param param = { .sched_priority = 90 };
    
    /* Highest priority kthread — must respond instantly to GPU faults */
    sched_setscheduler(current, SCHED_FIFO, &param);
    
    while (!kthread_should_stop()) {
        wait_event_interruptible(gpu->fault_wq,
                                 gpu->fault_pending || 
                                 kthread_should_stop());
        if (kthread_should_stop())
            break;
        
        gpu->fault_pending = false;
        
        /* Read fault info from GPU registers */
        u32 fault_status = gpu_read(gpu, NV_PGRAPH_FECS_FAULT_STATUS);
        u32 fault_addr = gpu_read(gpu, NV_PGRAPH_FECS_FAULT_ADDR);
        int channel_id = FIELD_GET(FAULT_CHANNEL_MASK, fault_status);
        
        struct gpu_channel *chan = &gpu->channels[channel_id];
        
        dev_err(gpu->dev, "GPU fault: ch=%d addr=0x%08x type=%s\n",
                channel_id, fault_addr,
                gpu_fault_type_str(fault_status));
        
        /* Attempt per-channel recovery first */
        int ret = gpu_try_channel_recovery(gpu, chan);
        if (ret == 0) {
            /* Channel recovered — signal error to user */
            gpu_channel_signal_error(chan, GPU_ERROR_LAUNCH_FAILED);
            continue;
        }
        
        /* Per-channel failed → escalate to full GPU recovery */
        dev_err(gpu->dev, "Channel recovery failed, full GPU reset\n");
        wake_up_process(gpu->recovery_thread);
    }
    
    return 0;
}

static int gpu_try_channel_recovery(struct gpu_device *gpu,
                                     struct gpu_channel *chan)
{
    /* Step 1: Remove channel from scheduling runlist */
    gpu_sched_remove_channel(gpu->sched, chan);
    
    /* Step 2: Preempt the channel on GPU */  
    gpu_write(gpu, NV_PBDMA_PREEMPT(chan->pbdma), chan->id);
    
    /* Step 3: Wait for preemption (max 100ms) */
    int ret = gpu_wait_preempt(gpu, chan, msecs_to_jiffies(100));
    if (ret == -ETIMEDOUT)
        return -EIO;  /* GPU stuck → need full reset */
    
    /* Step 4: Per-engine reset */
    u32 engine_mask = chan->bound_engines;
    gpu_write(gpu, NV_PMC_ENGINE_RESET, engine_mask);
    udelay(10);
    gpu_write(gpu, NV_PMC_ENGINE_RESET, 0);
    
    /* Step 5: Channel is now clean for re-use (but user must re-submit) */
    chan->state = GPU_CHANNEL_ERROR;
    return 0;
}
```

---

#### ❓ Q3: Design the kernel threading model for NVIDIA DRIVE (autonomous vehicles). You have camera, LiDAR, radar, and IMU sensors feeding into a perception pipeline. What kthread architecture guarantees < 10ms sensor-to-inference latency?

**A:**

```mermaid
flowchart TD
    subgraph Sensors["📡 Sensors (Hardware)"]
        CAM["Camera ISP<br/>8 cameras × 8MP<br/>30 fps"]
        LIDAR["LiDAR<br/>128 beams<br/>10 Hz rotation"]
        RADAR["Radar<br/>77 GHz<br/>20 Hz update"]
        IMU["IMU<br/>6-axis<br/>1000 Hz"]
    end
    
    subgraph KThreads["⚙️ Kernel Threads (SCHED_DEADLINE)"]
        CAM_T["cam_capture<br/>DEADLINE<br/>runtime=2ms<br/>period=33ms"]
        LID_T["lidar_capture<br/>DEADLINE<br/>runtime=1ms<br/>period=100ms"]
        RAD_T["radar_capture<br/>DEADLINE<br/>runtime=500µs<br/>period=50ms"]
        IMU_T["imu_capture<br/>FIFO prio=99<br/>period=1ms<br/>Highest priority"]
    end
    
    subgraph Fusion["🧠 Sensor Fusion (RT kthread)"]
        SYNC["sensor_sync<br/>DEADLINE<br/>runtime=3ms<br/>period=33ms<br/>Timestamp correlation"]
    end
    
    subgraph GPU_Submit["🖥️ GPU Submission"]
        DNN_SUB["dnn_submit<br/>FIFO prio=80<br/>Submit DNN inference<br/>to GPU DLA engine"]
    end
    
    CAM --> CAM_T
    LIDAR --> LID_T
    RADAR --> RAD_T
    IMU --> IMU_T
    
    CAM_T --> SYNC
    LID_T --> SYNC
    RAD_T --> SYNC
    IMU_T --> SYNC
    
    SYNC --> DNN_SUB
    DNN_SUB --> GPU0["Orin GPU / DLA<br/>Perception inference"]

    style CAM_T fill:#e74c3c,stroke:#c0392b,color:#fff
    style LID_T fill:#3498db,stroke:#2980b9,color:#fff
    style RAD_T fill:#f39c12,stroke:#e67e22,color:#fff
    style IMU_T fill:#c0392b,stroke:#922b21,color:#fff
    style SYNC fill:#9b59b6,stroke:#8e44ad,color:#fff
    style DNN_SUB fill:#2ecc71,stroke:#27ae60,color:#fff
```

**Timing analysis diagram:**

```mermaid
sequenceDiagram
    participant IMU as imu_capture<br/>[FIFO 99, 1ms]
    participant CAM as cam_capture<br/>[DEADLINE, 33ms]
    participant LIDAR as lidar_capture<br/>[DEADLINE, 100ms]
    participant SYNC as sensor_sync<br/>[DEADLINE, 33ms]
    participant DNN as dnn_submit<br/>[FIFO 80]
    participant DLA as GPU DLA Engine

    Note over IMU,DLA: Target: Sensor-to-inference < 10ms

    Note over IMU: T=0ms: IMU interrupt
    IMU->>IMU: Read 6-axis data (50µs)
    IMU->>SYNC: Push IMU sample to<br/>lockless SPSC ring

    Note over CAM: T=0ms: Camera VSYNC interrupt
    CAM->>CAM: Start ISP pipeline (1ms)
    CAM->>CAM: DMA frame to DRAM (1ms)
    CAM->>SYNC: Signal: frame ready + timestamp

    Note over LIDAR: T=2ms: LiDAR point cloud ready
    LIDAR->>LIDAR: DMA point cloud (500µs)
    LIDAR->>SYNC: Signal: pointcloud + timestamp

    Note over SYNC: T=3ms: All sensors available
    SYNC->>SYNC: Timestamp correlation<br/>(align IMU, cam, LiDAR<br/>using HW PTP timestamps)
    SYNC->>SYNC: Pack fused sensor buffer (1ms)
    
    Note over SYNC: T=4ms: Fused data ready
    SYNC->>DNN: Wake up DNN submit thread

    DNN->>DLA: Submit inference job<br/>(DMA-mapped fused buffer)
    
    Note over DLA: T=5ms: DLA starts inference
    DLA->>DLA: Neural network forward pass (4ms)
    
    Note over DLA: T=9ms: Detection results ready ✅
    DLA->>DNN: Interrupt: inference complete
    DNN->>DNN: Signal user-space planner

    Note over IMU,DLA: ⏱️ Total: 9ms < 10ms budget ✅
```

**SCHED_DEADLINE setup:**

```c
/* NVIDIA DRIVE: Real-time sensor capture threads */

struct sensor_capture_thread {
    struct task_struct *thread;
    struct sensor_device *sensor;
    ktime_t period;
    ktime_t runtime;
    ktime_t deadline;
};

static struct sensor_capture_thread *create_rt_sensor_thread(
    const char *name,
    int (*fn)(void *),
    void *data,
    u64 runtime_ns,
    u64 deadline_ns,
    u64 period_ns,
    int cpu)
{
    struct sensor_capture_thread *st;
    struct task_struct *t;
    
    st = kzalloc(sizeof(*st), GFP_KERNEL);
    
    t = kthread_create(fn, data, "drive/%s", name);
    if (IS_ERR(t))
        return ERR_CAST(t);
    
    /* Pin to specific CPU — sensor threads must not migrate */
    kthread_bind(t, cpu);
    
    /* Configure SCHED_DEADLINE for guaranteed CPU time */
    struct sched_attr attr = {
        .size = sizeof(attr),
        .sched_policy = SCHED_DEADLINE,
        .sched_runtime  = runtime_ns,   /* Max CPU time per period */
        .sched_deadline = deadline_ns,  /* Must finish by this */
        .sched_period   = period_ns,    /* Repetition period */
        .sched_flags    = SCHED_FLAG_RECLAIM, /* Donate unused time */
    };
    sched_setattr(t, &attr);
    
    wake_up_process(t);
    
    st->thread = t;
    return st;
}

/* Init all sensor threads for DRIVE Orin platform */
int drive_init_sensor_threads(struct drive_platform *plat)
{
    int isolated_cpus[] = {4, 5, 6, 7}; /* isolcpus=4-7 in cmdline */
    
    /* Camera: 2ms runtime, 33ms period (30 fps) */
    plat->cam_thread = create_rt_sensor_thread(
        "cam_capture", cam_capture_fn, plat->cam,
        2 * NSEC_PER_MSEC,   /* runtime */
        5 * NSEC_PER_MSEC,   /* deadline (some slack) */
        33 * NSEC_PER_MSEC,  /* period */
        isolated_cpus[0]);
    
    /* LiDAR: 1ms runtime, 100ms period (10 Hz) */
    plat->lidar_thread = create_rt_sensor_thread(
        "lidar_capture", lidar_capture_fn, plat->lidar,
        1 * NSEC_PER_MSEC,
        3 * NSEC_PER_MSEC,
        100 * NSEC_PER_MSEC,
        isolated_cpus[1]);
    
    /* Sensor fusion: 3ms runtime, 33ms period */
    plat->sync_thread = create_rt_sensor_thread(
        "sensor_sync", sensor_sync_fn, plat->fuser,
        3 * NSEC_PER_MSEC,
        8 * NSEC_PER_MSEC,
        33 * NSEC_PER_MSEC,
        isolated_cpus[2]);
    
    /* IMU: SCHED_FIFO (not DEADLINE — 1KHz is too fast for EDF) */
    plat->imu_thread = kthread_create(imu_capture_fn, plat->imu,
                                       "drive/imu_capture");
    kthread_bind(plat->imu_thread, isolated_cpus[3]);
    struct sched_param imu_param = { .sched_priority = 99 };
    sched_setscheduler(plat->imu_thread, SCHED_FIFO, &imu_param);
    wake_up_process(plat->imu_thread);
    
    return 0;
}
```

**CPU isolation for deterministic latency:**

```bash
# Kernel cmdline for NVIDIA DRIVE Orin
isolcpus=4-7                    # Reserve CPUs 4-7 for RT threads
nohz_full=4-7                   # Disable timer ticks on isolated CPUs
rcu_nocbs=4-7                   # Offload RCU callbacks from RT CPUs
irqaffinity=0-3                 # All IRQs on CPUs 0-3
nosoftlockup                    # Disable softlockup detector on RT CPUs
processor.max_cstate=1          # Disable deep C-states (latency)
```

---

#### ❓ Q4: Design a workqueue architecture for GPU power management. The GPU DVFS (Dynamic Voltage and Frequency Scaling) must respond within 1ms to load changes, but thermal throttling can be slower. How do you structure the work items?

**A:**

```mermaid
flowchart TD
    subgraph Triggers["⚡ PM Triggers"]
        LOAD["GPU Load Change<br/>(utilization counter)"]
        TEMP["Temperature Alert<br/>(thermal sensor IRQ)"]
        IDLE["GPU Idle<br/>(no work for 100ms)"]
        RESUME["System Resume<br/>(from suspend)"]
    end
    
    subgraph FastPath["🔴 Fast Path: WQ_HIGHPRI"]
        WQ_FAST["gpu_pm_fast_wq<br/>WQ_HIGHPRI / WQ_CPU_INTENSIVE<br/>max_active = 1"]
        DVFS["dvfs_work:<br/>Set GPU core clock<br/>Set memory clock<br/>Set voltage rail<br/>Latency: < 1ms"]
    end
    
    subgraph SlowPath["🟢 Slow Path: Normal WQ"]
        WQ_SLOW["gpu_pm_slow_wq<br/>WQ_UNBOUND<br/>max_active = 2"]
        THERM["thermal_work:<br/>Read all temp sensors<br/>Calculate throttle level<br/>Update cooling device<br/>Latency: < 50ms OK"]
        SUSPEND["suspend_work:<br/>Save GPU state<br/>Gate clocks<br/>Power off rails<br/>Latency: < 500ms OK"]
    end
    
    subgraph DelayedPath["⏰ Delayed Work"]
        IDLE_W["idle_check:<br/>delayed_work<br/>delay = 100ms<br/>Check if GPU still idle<br/>→ queue suspend_work"]
    end
    
    LOAD -->|"< 1ms"| WQ_FAST
    WQ_FAST --> DVFS
    
    TEMP -->|"< 50ms"| WQ_SLOW
    WQ_SLOW --> THERM
    
    IDLE -->|"100ms delay"| IDLE_W
    IDLE_W --> SUSPEND
    
    RESUME -->|"< 1ms"| WQ_FAST

    style WQ_FAST fill:#e74c3c,stroke:#c0392b,color:#fff
    style DVFS fill:#e74c3c,stroke:#c0392b,color:#fff
    style WQ_SLOW fill:#2ecc71,stroke:#27ae60,color:#fff
    style THERM fill:#f39c12,stroke:#e67e22,color:#fff
    style IDLE_W fill:#3498db,stroke:#2980b9,color:#fff
```

**Implementation:**

```c
/* GPU Power Management workqueue architecture */

struct gpu_pm {
    struct gpu_device *gpu;
    
    /* Fast path: DVFS (latency-critical) */
    struct workqueue_struct *fast_wq;
    struct work_struct dvfs_work;
    
    /* Slow path: thermal, suspend (latency-tolerant) */
    struct workqueue_struct *slow_wq;
    struct work_struct thermal_work;
    struct work_struct suspend_work;
    
    /* Delayed: idle detection */
    struct delayed_work idle_check_work;
    
    /* State */
    atomic_t current_perf_level;
    int target_freq_mhz;
    int current_temp_c;
    bool idle;
};

int gpu_pm_init(struct gpu_pm *pm, struct gpu_device *gpu)
{
    pm->gpu = gpu;
    
    /*
     * Fast WQ: WQ_HIGHPRI ensures PM work runs before normal kworkers.
     * max_active=1: serialize DVFS transitions (voltage sequencing).
     * WQ_CPU_INTENSIVE: tells scheduler this work may use a lot of CPU. 
     */
    pm->fast_wq = alloc_workqueue("gpu_pm_fast",
                                   WQ_HIGHPRI | WQ_CPU_INTENSIVE,
                                   1 /* max_active */);
    
    /*
     * Slow WQ: WQ_UNBOUND for thermal — no need for CPU locality,
     * and we don't want to interfere with latency-sensitive CPUs.
     * max_active=2: thermal + suspend can run simultaneously.
     */
    pm->slow_wq = alloc_workqueue("gpu_pm_slow",
                                   WQ_UNBOUND | WQ_FREEZABLE,
                                   2 /* max_active */);
    
    INIT_WORK(&pm->dvfs_work, gpu_dvfs_work_fn);
    INIT_WORK(&pm->thermal_work, gpu_thermal_work_fn);
    INIT_WORK(&pm->suspend_work, gpu_suspend_work_fn);
    INIT_DELAYED_WORK(&pm->idle_check_work, gpu_idle_check_fn);
    
    return 0;
}

/* Fast path: GPU load changed → update clocks immediately */
static void gpu_dvfs_work_fn(struct work_struct *work)
{
    struct gpu_pm *pm = container_of(work, struct gpu_pm, dvfs_work);
    int target = pm->target_freq_mhz;
    int current = gpu_get_clock(pm->gpu, GPU_CLK_CORE);
    
    if (target == current)
        return;
    
    /* Voltage must be raised BEFORE frequency (or GPU may crash) */
    if (target > current) {
        int voltage = dvfs_table_get_voltage(target);
        regulator_set_voltage(pm->gpu->vdd_gpu, voltage, voltage);
        udelay(50); /* Voltage settling time */
    }
    
    clk_set_rate(pm->gpu->core_clk, target * 1000000UL);
    clk_set_rate(pm->gpu->mem_clk, 
                 dvfs_table_get_memclk(target) * 1000000UL);
    
    /* Voltage can be lowered AFTER frequency */
    if (target < current) {
        int voltage = dvfs_table_get_voltage(target);
        regulator_set_voltage(pm->gpu->vdd_gpu, voltage, voltage);
    }
    
    atomic_set(&pm->current_perf_level, target);
    trace_gpu_dvfs(pm->gpu->id, current, target); /* ftrace event */
}

/* Usage from GPU interrupt handler */
irqreturn_t gpu_utilization_irq(int irq, void *data)
{
    struct gpu_pm *pm = data;
    int util = gpu_read_utilization(pm->gpu); /* 0-100% */
    
    /* Simple governor: map utilization to frequency */
    if (util > 85)
        pm->target_freq_mhz = GPU_MAX_FREQ;
    else if (util > 50)
        pm->target_freq_mhz = GPU_MID_FREQ;
    else
        pm->target_freq_mhz = GPU_MIN_FREQ;
    
    /* Queue on HIGH priority workqueue — runs within 1ms */
    queue_work(pm->fast_wq, &pm->dvfs_work);
    
    return IRQ_HANDLED;
}
```

---

#### ❓ Q5: How do you safely stop a kthread that might be blocked on a GPU hardware register read? `kthread_stop()` can deadlock if the thread never checks `kthread_should_stop()`.

**A:**

```mermaid
flowchart TD
    A["kthread_stop(thread)<br/>Sets should_stop = true<br/>Wakes thread"] --> B{Thread state?}
    
    B -->|"In wait_event()"| C["✅ wake_up → check<br/>kthread_should_stop() → exit"]
    
    B -->|"Blocked on<br/>GPU register read<br/>(MMIO polling)"| D["🔴 DEADLOCK!<br/>Thread spinning on HW<br/>Never checks should_stop"]
    
    B -->|"In interruptible sleep"| E["✅ Signal wakes thread<br/>→ check should_stop → exit"]
    
    D --> F{Solutions}
    
    F -->|"1"| G["Use poll with timeout<br/>readl_poll_timeout()"]
    F -->|"2"| H["Interruptible polling<br/>with signal check"]
    F -->|"3"| I["Hardware abort +<br/>timeout mechanism"]

    style A fill:#3498db,stroke:#2980b9,color:#fff
    style C fill:#2ecc71,stroke:#27ae60,color:#fff
    style D fill:#e74c3c,stroke:#c0392b,color:#fff
    style E fill:#2ecc71,stroke:#27ae60,color:#fff
    style G fill:#2ecc71,stroke:#27ae60,color:#fff
```

**Safe kthread with hardware timeout:**

```c
/* WRONG: Can deadlock on kthread_stop() */
static int gpu_monitor_bad(void *data)
{
    struct gpu_device *gpu = data;
    
    while (!kthread_should_stop()) {
        /* This can spin forever if GPU is hung! */
        u32 val;
        do {
            val = readl(gpu->mmio + GPU_STATUS_REG);
        } while (!(val & STATUS_READY));  /* ← DEADLOCK RISK */
        
        process_gpu_status(val);
    }
    return 0;
}

/* CORRECT: Safe kthread with bounded waits */
static int gpu_monitor_good(void *data)
{
    struct gpu_device *gpu = data;
    
    while (!kthread_should_stop()) {
        u32 val;
        int ret;
        
        /* Solution 1: readl_poll_timeout — bounded MMIO poll */
        ret = readl_poll_timeout(gpu->mmio + GPU_STATUS_REG,
                                  val,
                                  val & STATUS_READY,
                                  100,        /* 100µs between polls */
                                  5000000);   /* 5 sec total timeout */
        
        if (ret == -ETIMEDOUT) {
            dev_warn(gpu->dev, "GPU status timeout — checking stop\n");
            /* Loop back → kthread_should_stop() checked */
            continue;
        }
        
        if (kthread_should_stop())
            break;
        
        process_gpu_status(val);
        
        /* Solution 2: Use wait_event with timeout for non-MMIO waits */
        ret = wait_event_interruptible_timeout(
            gpu->status_wq,
            gpu->status_updated || kthread_should_stop(),
            msecs_to_jiffies(1000));
        
        if (kthread_should_stop())
            break;
    }
    
    /* Clean shutdown: release any resources held */
    gpu_channel_cleanup(gpu);
    return 0;
}

/* Safe kthread_stop wrapper with fallback */
void gpu_kthread_safe_stop(struct task_struct **thread_ptr)
{
    struct task_struct *t = *thread_ptr;
    if (!t)
        return;
    
    /* First: try normal stop */
    int ret = kthread_stop(t);
    if (ret == -EINTR) {
        /* Thread might be stuck — force it */
        dev_warn(gpu->dev, "kthread didn't stop cleanly\n");
        /*
         * Don't force-kill kernel threads!
         * Fix the root cause: ensure all waits are bounded.
         */
    }
    
    *thread_ptr = NULL;
}
```

---

#### ❓ Q6: Design the NVLink fabric monitoring thread architecture for a DGX system with 8 GPUs. How do you detect link degradation, handle retraining, and report to user space without disrupting active GPU-to-GPU transfers?

**A:**

```mermaid
flowchart TD
    subgraph DGX["🖥️ DGX H100 — 8 GPUs"]
        G0["GPU 0"] --- NV01["NVLink 0-1<br/>900 GB/s"]
        G1["GPU 1"]
        G0 --- NV02["NVLink 0-2"]
        G2["GPU 2"]
        G1 --- NV13["NVLink 1-3"]
        G3["GPU 3"]
        G4["GPU 4"] --- NV45["NVLink 4-5"]
        G5["GPU 5"]
        G6["GPU 6"] --- NV67["NVLink 6-7"]
        G7["GPU 7"]
    end
    
    subgraph Monitor["⚙️ NVLink Monitor Thread"]
        MON["nvlink/monitor<br/>kthread (NORMAL)<br/>Polls link status every 1s"]
        
        MON --> CHECK["Read NV_LINK_STATUS<br/>for all 18 links"]
        CHECK --> HEALTH{Link Health?}
        
        HEALTH -->|"✅ Active,<br/>no errors"| GOOD["Log metrics<br/>Continue polling"]
        HEALTH -->|"⚠️ CRC errors<br/>increasing"| DEGRADE["Link degradation!<br/>Attempt retrain"]
        HEALTH -->|"🔴 Link down"| DOWN["Fatal: Link failure<br/>Notify topology manager"]
    end
    
    DEGRADE --> RETRAIN["nvlink/retrain<br/>workqueue: per-link<br/>1. Quiesce traffic<br/>2. Reset PHY<br/>3. Retrain link<br/>4. Resume traffic"]
    
    DOWN --> NOTIFY["Sysfs/uevent notification<br/>nvidia-persistenced handles<br/>Route traffic via alternate path"]

    style MON fill:#3498db,stroke:#2980b9,color:#fff
    style DEGRADE fill:#f39c12,stroke:#e67e22,color:#fff
    style DOWN fill:#e74c3c,stroke:#c0392b,color:#fff
    style RETRAIN fill:#9b59b6,stroke:#8e44ad,color:#fff
    style GOOD fill:#2ecc71,stroke:#27ae60,color:#fff
```

**NVLink retraining sequence:**

```mermaid
sequenceDiagram
    participant MON as nvlink/monitor<br/>[kthread]
    participant WQ as nvlink/retrain<br/>[workqueue]
    participant GPU_A as GPU 0<br/>[Local]
    participant LINK as NVLink PHY<br/>[Hardware]
    participant GPU_B as GPU 1<br/>[Remote]
    participant SCHED as Channel Scheduler

    MON->>LINK: Read NV_LINK_STATUS
    LINK->>MON: CRC error count: 1000/sec<br/>(threshold: 100/sec)
    
    Note over MON: ⚠️ Link degrading!
    MON->>WQ: queue_work(retrain_work)
    
    WQ->>SCHED: Pause P2P channels<br/>using links 0-1
    SCHED->>GPU_A: Drain pending P2P DMA
    GPU_A->>SCHED: DMA queue drained ✅
    
    WQ->>LINK: Write NV_LINK_CTRL = SAFE_MODE
    Note over LINK: Link enters low-speed mode
    
    WQ->>LINK: Write NV_LINK_PHY_RESET = 1
    Note over LINK: PHY retraining...<br/>(takes ~50ms)
    
    WQ->>LINK: Poll NV_LINK_STATUS<br/>for ACTIVE state
    
    alt Retrain Success
        LINK->>WQ: Status: ACTIVE ✅<br/>Speed: NVLink4.0 full
        WQ->>SCHED: Resume P2P channels
        WQ->>MON: Link recovered
        Note over GPU_A,GPU_B: Transfers resume<br/>Transparently ✅
    else Retrain Failed (3 attempts)
        LINK->>WQ: Status: FAULT ❌
        WQ->>MON: Link permanently down
        MON->>MON: Update topology:<br/>Route via alternate links
        MON->>MON: kobject_uevent(KOBJ_CHANGE)<br/>→ nvidia-smi shows degraded
    end
```

```c
/* NVLink fabric monitor kthread */

struct nvlink_monitor {
    struct task_struct *thread;
    struct gpu_device *gpus[8];
    struct nvlink_status links[18]; /* NVSwitch connected */
    struct workqueue_struct *retrain_wq;
    struct work_struct retrain_work[18];
};

static int nvlink_monitor_thread(void *data)
{
    struct nvlink_monitor *mon = data;
    
    while (!kthread_should_stop()) {
        /* Poll all links every 1 second */
        for (int i = 0; i < mon->num_links; i++) {
            u32 status = nvlink_read_status(mon, i);
            u32 crc_errors = nvlink_read_crc_count(mon, i);
            u32 replay_count = nvlink_read_replay_count(mon, i);
            
            /* Export to sysfs for nvidia-smi */
            mon->links[i].error_rate = crc_errors;
            mon->links[i].replays = replay_count;
            
            /* Check degradation thresholds */
            if (crc_errors > NVLINK_CRC_THRESHOLD) {
                dev_warn(mon->gpus[0]->dev,
                         "NVLink %d: CRC rate %u/sec (threshold=%u)\n",
                         i, crc_errors, NVLINK_CRC_THRESHOLD);
                
                mon->retrain_link_id = i;
                queue_work(mon->retrain_wq, &mon->retrain_work[i]);
            }
        }
        
        /* Interruptible sleep — responds to kthread_stop() */
        if (wait_event_interruptible_timeout(
                mon->poll_wq,
                kthread_should_stop(),
                msecs_to_jiffies(1000)) != 0) {
            if (kthread_should_stop())
                break;
        }
    }
    return 0;
}
```

---

#### ❓ Q7: Compare `kthread_create` vs `alloc_workqueue` vs `create_singlethread_workqueue` for GPU driver design. When do you pick each?

**A:**

| Aspect | `kthread_create` | `alloc_workqueue` (CMWQ) | Threaded IRQ |
|--------|-----------------|--------------------------|--------------|
| **Use case** | Persistent daemon, custom event loop | Fire-and-forget work items | Bottom-half of interrupt |
| **Scheduling control** | Full: SCHED_FIFO, SCHED_DEADLINE, affinity | Limited: WQ_HIGHPRI for priority | SCHED_FIFO (kernel-managed) |
| **Lifecycle** | Manual: create → run → stop | Automatic: queue → run → done | IRQ handler lifecycle |
| **GPU example** | Channel scheduler, NVLink monitor | DVFS, thermal throttle | GPU page fault, completion |
| **Concurrency** | One thread, your code manages | Configurable `max_active` | One per IRQ line |
| **CPU affinity** | `kthread_bind()` / `set_cpus_allowed` | `WQ_UNBOUND` or CPU-bound | Per-IRQ affinity |
| **Pros** | Maximum control, custom scheduling | Simple, automatic thread pooling | Low latency from IRQ |
| **Cons** | Must manage thread lifecycle, sync | Less control over scheduling | Limited to interrupt context work |

```mermaid
flowchart TD
    Q["Need a kernel execution<br/>context for GPU work?"] --> Q1{Persistent<br/>event loop?}
    
    Q1 -->|Yes| Q2{Need RT<br/>scheduling?}
    Q2 -->|"Yes: SCHED_FIFO<br/>or SCHED_DEADLINE"| K["kthread_create()<br/>Channel scheduler<br/>Sensor capture"]
    Q2 -->|No| Q3{Custom<br/>sleep/wake<br/>pattern?}
    Q3 -->|Yes| K
    Q3 -->|No| WQ1["alloc_workqueue()"]
    
    Q1 -->|"No: fire-and-forget"| Q4{Latency<br/>critical?}
    Q4 -->|"< 1ms"| WQ2["WQ_HIGHPRI workqueue<br/>DVFS, recovery"]
    Q4 -->|"> 1ms OK"| WQ3["Normal workqueue<br/>Thermal, suspend"]
    
    Q4 -->|"From IRQ<br/>context"| TH["request_threaded_irq()<br/>GPU page fault handler"]

    style K fill:#e74c3c,stroke:#c0392b,color:#fff
    style WQ1 fill:#f39c12,stroke:#e67e22,color:#fff
    style WQ2 fill:#3498db,stroke:#2980b9,color:#fff
    style WQ3 fill:#2ecc71,stroke:#27ae60,color:#fff
    style TH fill:#9b59b6,stroke:#8e44ad,color:#fff
```

---

[Next: 02 — Qualcomm 15yr Deep →](02_Qualcomm_15yr_Kernel_Thread_Deep_Interview.md) | [Back to Index →](../ReadMe.Md)
