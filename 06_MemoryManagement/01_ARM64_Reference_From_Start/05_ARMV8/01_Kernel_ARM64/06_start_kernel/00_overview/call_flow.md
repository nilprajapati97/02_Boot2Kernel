# `start_kernel()` — Complete Call Flow

This document lists every function called by `start_kernel()` in source order, with the approximate line number in `init/main.c`, IRQ state at the time of the call, and a one-line description.

**Source:** `init/main.c`, lines 874–1109

---

## Legend

- `[D]` = IRQs Disabled at time of call
- `[E]` = IRQs Enabled at time of call
- `[*]` = Conditional call (depends on config or runtime value)

---

## Phase 1 — Early CPU & Task Setup

```
 #  Line   IRQ  Function                           Description
 1   879   [D]  set_task_stack_end_magic()          Write stack overflow sentinel in init_task
 2   880   [D]  smp_setup_processor_id()            Identify and record boot CPU ID
 3   881   [D]  debug_objects_early_init()          Bootstrap object lifecycle tracking
 4   882   [D]  init_vmlinux_build_id()             Record ELF build-ID of vmlinux image
 5   884   [D]  cgroup_init_early()                 Pre-allocate root cgroup, init subsystem list
 6   886   [D]  local_irq_disable()                 <<< EXPLICITLY DISABLE IRQs >>>
 7   890   [D]  boot_cpu_init()                     Set boot CPU in online/present/active cpumasks
 8   891   [D]  page_address_init()                 Initialize highmem page_address hash table
```

## Phase 2 — Architecture Setup

```
 #  Line   IRQ  Function                           Description
 9   893   [D]  early_security_init()               Initialize LSM framework, order security modules
10   894   [D]  setup_arch(&command_line)           LARGEST CALL: arch memory map, CPU features
11   895   [D]  setup_boot_config()                 Parse bootconfig blob from initrd if present
12   896   [D]  setup_command_line()                Allocate & copy saved/static command line buffers
13   897   [D]  setup_nr_cpu_ids()                  Compute nr_cpu_ids from possible CPU mask
14   898   [D]  setup_per_cpu_areas()               Replicate per-CPU sections for all possible CPUs
15   899   [D]  smp_prepare_boot_cpu()              Arch: copy GDT/TSS to per-CPU area (x86)
16   900   [D]  boot_cpu_hotplug_init()             Initialize CPU hotplug state machine
```

## Phase 3 — Parameter Parsing

```
 #  Line   IRQ  Function                           Description
17   904   [D]  jump_label_init()                  Patch NOP sites for static keys/branches
18   905   [D]  parse_early_param()                Scan cmdline, call early_param() handlers
19   906   [D]  parse_args("Booting kernel", ...)  Parse full cmdline, call __param handlers
20   910   [D]  print_unknown_bootoptions()         Log cmdline params not consumed by kernel
21   911   [D]* parse_args("Setting init args", .) Parse args after "--" separator for init
22   913   [D]* parse_args("Setting extra init args") Parse bootconfig init.* args
23   917   [D]  random_init_early()                Seed CRNG from cmdline hash (pre-allocator)
```

## Phase 4 — Core Memory & Exception Handling

```
 #  Line   IRQ  Function                           Description
24   923   [D]  setup_log_buf(0)                   Allocate printk ring buffer (initial size)
25   924   [D]  vfs_caches_init_early()             Create early dentry and inode slab caches
26   925   [D]  sort_main_extable()                 Sort exception table for binary search
27   926   [D]  trap_init()                         Install CPU exception handlers (IDT on x86)
28   927   [D]  mm_core_init()                      Initialize page allocator, zones, SLUB/SLAB
29   928   [D]  poking_init()                       Allocate VMAs for live kernel code patching
30   929   [D]  ftrace_init()                       Scan mcount call sites, build ftrace index
```

## Phase 5 — Tracing Infrastructure

```
 #  Line   IRQ  Function                           Description
31   932   [D]  early_trace_init()                 Initialize trace ring buffer for trace_printk
```

## Phase 6 — Scheduler

```
 #  Line   IRQ  Function                           Description
32   940   [D]  sched_init()                       Initialize per-CPU runqueues, CFS/RT/DL classes
```

## Phase 7 — Data Structures, Workqueues & Housekeeping

```
 #  Line   IRQ  Function                           Description
33   943   [D]  radix_tree_init()                  Pre-allocate radix_tree_node slab pool
34   944   [D]  maple_tree_init()                  Pre-allocate maple_node pool for VMA management
35   950   [D]  housekeeping_init()                Set up CPU isolation (nohz_full/isolcpus)
36   957   [D]  workqueue_init_early()             Create early workqueue infrastructure
```

## Phase 8 — RCU & Tracing

```
 #  Line   IRQ  Function                           Description
37   959   [D]  rcu_init()                         Initialize Tree RCU: GP kthread, callbacks
38   962   [D]  trace_init()                       Initialize ftrace event ring buffers
39   964   [D]* initcall_debug_enable()            Register initcall tracepoint probes [DEBUG]
40   967   [D]  context_tracking_init()            Initialize user/kernel boundary tracking
```

## Phase 9 — Interrupts & Timers

```
 #  Line   IRQ  Function                           Description
41   969   [D]  early_irq_init()                   Allocate IRQ descriptor array (irq_desc[])
42   970   [D]  init_IRQ()                         Configure APIC/PIC, install arch IRQ handlers
43   971   [D]  tick_init()                        Register clock event device notifier chains
44   972   [D]  rcu_init_nohz()                    Wire RCU into dyntick/nohz tick suppression
45   973   [D]  init_timers()                      Initialize per-CPU 5-level timer wheels
46   974   [D]  srcu_init()                        Initialize SRCU (sleepable RCU) workqueue
47   975   [D]  hrtimers_init()                    Initialize per-CPU high-resolution timer rb-tree
48   976   [D]  softirq_init()                     Initialize per-CPU softirq vectors and tasklets
49   977   [D]  timekeeping_init()                 Initialize wall clock, xtime, clocksource
50   978   [D]  time_init()                        Arch: register hardware clocksource (TSC, HPET)
```

## Phase 10 — Randomness & Memory Safety

```
 #  Line   IRQ  Function                           Description
51   981   [D]  random_init()                      Full CRNG init using timer jitter entropy
52   984   [D]  kfence_init()                      Arm KFENCE guard pages for heap safety net
53   985   [D]  boot_init_stack_canary()           Set stack canary for boot CPU task
```

## Phase 11 — Performance & IPI

```
 #  Line   IRQ  Function                           Description
54   987   [D]  perf_event_init()                  Initialize PMU contexts, SW/HW event types
55   988   [D]  profile_init()                     Initialize kernel oprofile/profiling buffers
56   989   [D]  call_function_init()               Initialize IPI call-function mechanism
```

## IRQ Enable Point

```
     993   ===  local_irq_enable()                <<< IRQs NOW ENABLED >>>
```

## Phase 12 — Post-IRQ Memory & Console

```
 #  Line   IRQ  Function                           Description
57   995   [E]  kmem_cache_init_late()             Finish SLAB/SLUB (enable cache sharing)
58  1001   [E]  console_init()                     Register all console drivers (TTY, serial)
59  1005   [E]  lockdep_init()                     Initialize lock dependency tracking graph
60  1012   [E]  locking_selftest()                 Validate lock ordering / IRQ-safe rules
```

## Phase 13 — NUMA, ACPI, Calibration

```
 #  Line   IRQ  Function                           Description
61  1022   [E]  setup_per_cpu_pageset()            Set up per-CPU hot/cold page lists
62  1023   [E]  numa_policy_init()                 Initialize default NUMA memory policy
63  1024   [E]  acpi_early_init()                  Scan ACPI namespace tables (DSDT/SSDT)
64  1025   [E]* late_time_init()                   Arch optional: late clocksource setup
65  1027   [E]  sched_clock_init()                 Stabilize scheduler clock after calibration
66  1028   [E]  calibrate_delay()                  Measure loops_per_jiffy (BogoMIPS)
67  1030   [E]  arch_cpu_finalize_init()           Apply CPU mitigations, microcode, topology
```

## Phase 14 — Process & Security Infrastructure

```
 #  Line   IRQ  Function                           Description
68  1032   [E]  pid_idr_init()                     Initialize PID namespace IDR allocator
69  1033   [E]  anon_vma_init()                    Create anonymous VMA slab cache
70  1035   [E]* efi_enter_virtual_mode()           x86: Remap EFI runtime services to kernel VA
71  1038   [E]  thread_stack_cache_init()          Create thread stack slab pool
72  1039   [E]  cred_init()                        Create credential (UID/GID/caps) slab cache
73  1040   [E]  fork_init()                        Create task_struct slab, set max_threads
74  1041   [E]  proc_caches_init()                 Create /proc inode/dentry slab caches
75  1042   [E]  uts_ns_init()                      Initialize UTS (hostname) namespace
76  1043   [E]  key_init()                         Initialize kernel key management subsystem
77  1044   [E]  security_init()                    Initialize LSM (SELinux/AppArmor/Smack etc.)
78  1045   [E]  dbg_late_init()                    KDB/KGDB late debugger initialization
79  1046   [E]  net_ns_init()                      Initialize init_net (network namespace 0)
```

## Phase 15 — VFS & Filesystems

```
 #  Line   IRQ  Function                           Description
80  1047   [E]  vfs_caches_init()                  Finalize dentry/inode caches, VFS hash tables
81  1048   [E]  pagecache_init()                   Initialize page cache hash table
82  1049   [E]  signals_init()                     Create sigqueue slab cache
83  1050   [E]  seq_file_init()                    Initialize seq_file interface for /proc
84  1051   [E]  proc_root_init()                   Mount /proc filesystem root
85  1052   [E]  nsfs_init()                        Mount nsfs (namespace file system)
```

## Phase 16 — Control Groups & Accounting

```
 #  Line   IRQ  Function                           Description
86  1053   [E]  cpuset_init()                      Initialize CPU-set cgroup controller
87  1054   [E]  cgroup_init()                      Initialize cgroup v1/v2 hierarchy
88  1055   [E]  taskstats_init_early()             Initialize per-task statistics (netlink)
89  1056   [E]  delayacct_init()                   Initialize per-task scheduler delay accounting
```

## Phase 17 — Platform Finalization

```
 #  Line   IRQ  Function                           Description
90  1058   [E]  acpi_subsystem_init()              Finalize ACPI, register power management
91  1059   [E]  arch_post_acpi_subsys_init()       Arch cleanup after ACPI subsystem init
92  1060   [E]  kcsan_init()                       Initialize KCSAN concurrency sanitizer
```

## Final Transition

```
 #  Line   IRQ  Function                           Description
93  1063   [E]  arch_call_rest_init()              → rest_init() → idle loop (NEVER RETURNS)
```

---

## Summary Statistics

| Category | Count |
|----------|-------|
| Total direct calls | 93 |
| Calls with IRQs disabled | 56 |
| Calls with IRQs enabled | 37 |
| Conditional calls | 4 |
| Functions in `init/main.c` itself | 8 |
| Functions in `mm/` | 12 |
| Functions in `kernel/` | 28 |
| Functions in `arch/` | 5 |
| Functions in `fs/` | 6 |
| Functions in `drivers/` | 4 |
