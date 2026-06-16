```text
start_kernel()
│
├─ 1. Prepare first task and CPU
│   ├─ set_task_stack_end_magic(&init_task)
│   └─ smp_setup_processor_id()
│
├─ 2. Very early kernel setup
│   ├─ debug init
│   ├─ cgroup early init
│   └─ disable interrupts
│
├─ 3. Architecture setup
│   ├─ boot_cpu_init()
│   ├─ setup_arch()
│   ├─ setup command line
│   ├─ setup CPU count
│   └─ setup per-CPU areas
│
├─ 4. Memory setup
│   ├─ early memory init
│   ├─ page allocator init
│   ├─ slab/cache init
│   └─ NUMA/page sets
│
├─ 5. Core kernel subsystems
│   ├─ scheduler init
│   ├─ RCU init
│   ├─ IRQ init
│   ├─ timers/timekeeping init
│   ├─ softirq init
│   └─ workqueue early init
│
├─ 6. Enable interrupts
│   ├─ early_boot_irqs_disabled = false
│   └─ local_irq_enable()
│
├─ 7. Console and debugging
│   ├─ console_init()
│   ├─ lockdep_init()
│   └─ locking_selftest()
│
├─ 8. Process/kernel object setup
│   ├─ pid_idr_init()
│   ├─ cred_init()
│   ├─ fork_init()
│   ├─ signals_init()
│   └─ namespace init
│
├─ 9. Filesystem/network/security setup
│   ├─ vfs_caches_init()
│   ├─ proc_root_init()
│   ├─ security_init()
│   ├─ net_ns_init()
│   └─ cgroup_init()
│
└─ 10. Start real kernel threads/userspace path
    └─ rest_init()
```
