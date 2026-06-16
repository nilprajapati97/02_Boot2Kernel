# Character Driver — Linux Kernel

Progressive 12-level guide to Linux character device driver development, from basic registration through production-grade data-plane design.

---

## Root Reference Documents

| File | Content |
|---|---|
| [00_CharDriver_Overview.Md](00_CharDriver.Md) | End-to-end overview: registration, file_operations, device node, cleanup, interview points |
| [01_CharDriver_Init_Registration_Flow.Md](01_CharDriver_Init_Registration_Flow.Md) | `alloc_chrdev_region` → `cdev_init` → `cdev_add` → `device_create` flow |
| [02_CharDriver_EndToEnd_Reference.Md](02_CharDriver_EndToEnd_Reference.Md) | Full reference: kernel concepts, runtime path, complete buffered driver, pitfalls, debugging |
| [03_VFS_Syscall_Driver_Dispatch.Md](03_VFS_Syscall_Driver_Dispatch.Md) | VFS layer: how `open/read/write/ioctl` syscalls dispatch to `file_operations` |
| [00_CharDriverFlow.jpeg](00_CharDriverFlow.jpeg) | Registration flow diagram |

---

## Progressive Level Directories

Each level contains a kernel driver implementation (`01_Implementation/`) and a deep design document (`02_Design/`).

| # | Directory | Key Concepts | Driver | Design |
|---|---|---|---|---|
| 01 | [01_Basic_CharDriver_Registration_VFS](01_Basic_CharDriver_Registration_VFS/) | `alloc_chrdev_region`, `cdev`, VFS flow | [basic_chardriver.c](01_Basic_CharDriver_Registration_VFS/01_Implementation/basic_chardriver.c) | [Design](01_Basic_CharDriver_Registration_VFS/02_Design/Design_Basic_CharDriver.Md) |
| 02 | [02_Full_CharDriver_IOCTL_Poll_IRQ](02_Full_CharDriver_IOCTL_Poll_IRQ/) | `ioctl`, `poll`/`epoll`, ring buffer, `wait_queue`, `request_irq` | [full_driver.c](02_Full_CharDriver_IOCTL_Poll_IRQ/01_Implementation/full_driver.c) | [Design](02_Full_CharDriver_IOCTL_Poll_IRQ/02_Design/Design_Full_Driver.Md) |
| 03 | [03_Integrated_Production_CharDriver](03_Integrated_Production_CharDriver/) | `container_of`, threaded IRQ, bidirectional flow | [integrated_driver.c](03_Integrated_Production_CharDriver/01_Implementation/integrated_driver.c) | [Design](03_Integrated_Production_CharDriver/02_Design/Design_Integrated_Driver.Md) |
| 04 | [04_Platform_Driver_DMA_DeviceTree](04_Platform_Driver_DMA_DeviceTree/) | `platform_driver`, Device Tree, `dma_alloc_coherent`, `devm_*` | [platform_dma_driver.c](04_Platform_Driver_DMA_DeviceTree/01_Implementation/platform_dma_driver.c) | [Design](04_Platform_Driver_DMA_DeviceTree/02_Design/Design_Platform_DMA.Md) |
| 05 | [05_Ultra_Level_SMP_LockFree_ScatterGather](05_Ultra_Level_SMP_LockFree_ScatterGather/) | `smp_wmb/rmb`, SPSC ring, RCU, SG-DMA, per-CPU | [ultra_smp_lockfree_driver.c](05_Ultra_Level_SMP_LockFree_ScatterGather/01_Implementation/ultra_smp_lockfree_driver.c) | [Design](05_Ultra_Level_SMP_LockFree_ScatterGather/02_Design/Design_Ultra_Level.Md) |
| 06 | [06_Principal_SoC_SystemArchitecture_Power](06_Principal_SoC_SystemArchitecture_Power/) | Clock/regulator/pinctrl, Runtime PM, cache alignment | [principal_soc_driver.c](06_Principal_SoC_SystemArchitecture_Power/01_Implementation/principal_soc_driver.c) | [Design](06_Principal_SoC_SystemArchitecture_Power/02_Design/Design_Principal_SoC.Md) |
| 07 | [07_Master_Platform_DMA_Workqueue_SMP](07_Master_Platform_DMA_Workqueue_SMP/) | DMA Engine, async DMA + completion, ordered workqueue | [master_platform_driver.c](07_Master_Platform_DMA_Workqueue_SMP/01_Implementation/master_platform_driver.c) | [Design](07_Master_Platform_DMA_Workqueue_SMP/02_Design/Design_Master_Level.Md) |
| 08 | [08_Debug_Scheduler_Ftrace_Perf](08_Debug_Scheduler_Ftrace_Perf/) | `debugfs`, `seq_file`, latency histogram, ftrace, perf, `WARN` | [debug_instrumented_driver.c](08_Debug_Scheduler_Ftrace_Perf/01_Implementation/debug_instrumented_driver.c) | [Design](08_Debug_Scheduler_Ftrace_Perf/02_Design/Design_Debug_Scheduler.Md) |
| 09 | [09_Architect_CFS_eBPF_CrashDump](09_Architect_CFS_eBPF_CrashDump/) | CFS vruntime, eBPF kprobe, vmcore analysis, shrinker | [architect_ebpf_driver.c](09_Architect_CFS_eBPF_CrashDump/01_Implementation/architect_ebpf_driver.c) | [Design](09_Architect_CFS_eBPF_CrashDump/02_Design/Design_Architect_Level.Md) |
| 10 | [10_Elite_HighThroughput_PerCPU_Pipeline](10_Elite_HighThroughput_PerCPU_Pipeline/) | Per-CPU rings, NAPI polling, interrupt coalescing, hrtimer | [elite_percpu_pipeline_driver.c](10_Elite_HighThroughput_PerCPU_Pipeline/01_Implementation/elite_percpu_pipeline_driver.c) | [Design](10_Elite_HighThroughput_PerCPU_Pipeline/02_Design/Design_Elite_System.Md) |
| 11 | [11_Beyond_Elite_MultiQueue_ZeroCopy_Epoll](11_Beyond_Elite_MultiQueue_ZeroCopy_Epoll/) | Multi-queue, zero-copy `mmap`, `epoll`, CPU affinity | [beyond_elite_multiqueue_driver.c](11_Beyond_Elite_MultiQueue_ZeroCopy_Epoll/01_Implementation/beyond_elite_multiqueue_driver.c) | [Design](11_Beyond_Elite_MultiQueue_ZeroCopy_Epoll/02_Design/Design_Beyond_Elite.Md) |
| 12 | [12_Pinnacle_DataPlane_ControlPlane_5G](12_Pinnacle_DataPlane_ControlPlane_5G/) | Data plane + control plane, RCU config, batch IOCTL, eventfd, latency histogram | [pinnacle_dataplane_driver.c](12_Pinnacle_DataPlane_ControlPlane_5G/01_Implementation/pinnacle_dataplane_driver.c) | [Design](12_Pinnacle_DataPlane_ControlPlane_5G/02_Design/Design_Pinnacle_System.Md) |

---

[⬆️ Parent Directory](../README.md)
