# `start_kernel()` — Complete Linux Kernel Boot Flow

## Overview

| Attribute       | Value                                      |
|-----------------|---------------------------------------------|
| **Function**    | `start_kernel(void)`                        |
| **Source File** | `init/main.c`                               |
| **Line**        | ~1017 (kernel 6.x)                          |
| **Called From** | Architecture-specific entry point (e.g., `x86_64_start_kernel()` → `x86_64_start_reservations()`) |
| **Returns**     | Never — transitions to `rest_init()` which loops forever in idle |
| **Prototype**   | `asmlinkage __visible void __init __no_sanitize_address __noreturn __no_stack_protector start_kernel(void)` |

---

## Purpose

`start_kernel()` is the **first architecture-independent C function** executed during Linux kernel boot.

At this point:
- The bootloader (GRUB/U-Boot/EFI) has loaded the kernel image into memory
- Architecture-specific assembly prologue has:
  - Set up a temporary stack
  - Enabled paging (at minimum identity-mapped)
  - Cleared BSS segment
  - Called the arch C entry which then calls `start_kernel()`

`start_kernel()` is responsible for initializing **every major kernel subsystem** in the correct dependency order, ultimately creating the first two kernel threads (`kernel_init` and `kthreadd`) via `rest_init()`, then becoming the idle thread (PID 0).

---

## Boot Flow — Simplified ASCII Diagram

```
Power On
   │
   ▼
BIOS/UEFI Firmware
   │  (POST, hardware enumeration)
   ▼
Bootloader (GRUB2 / U-Boot / EFI stub)
   │  (loads vmlinuz, initrd, command line into RAM)
   ▼
kernel/head_64.S  (arch/x86/kernel/head_64.S)
   │  (page table setup, GDT/IDT, early stack, BSS clear)
   ▼
x86_64_start_kernel()   [arch/x86/kernel/head64.c]
   │  (early paging finalize, copy_bootdata)
   ▼
x86_64_start_reservations()
   │
   ▼
start_kernel()          ← YOU ARE HERE  [init/main.c:1017]
   │
   ├─ [Phase 1]  Very Early Init  (stack canary, cpu id, debug, cgroup early)
   ├─ [Phase 2]  CPU & Arch Init  (boot_cpu_init, setup_arch)
   ├─ [Phase 3]  Cmdline & CPU    (setup_per_cpu_areas, parse_early_param)
   ├─ [Phase 4]  Memory Init      (trap_init, mm_core_init, ftrace_init)
   ├─ [Phase 5]  Scheduler Init   (sched_init)
   ├─ [Phase 6]  Workqueue & RCU  (workqueue_init_early, rcu_init)
   ├─ [Phase 7]  IRQ & Timers     (init_IRQ, timekeeping_init, random_init)
   ├─ [Phase 8]  Security         (kfence_init, perf_event_init)
   ├─ [Phase 9]  Console & Debug  (console_init, lockdep_init)
   ├─ [Phase 10] Platform/Policy  (numa_policy_init, calibrate_delay)
   ├─ [Phase 11] Process Init     (fork_init, cred_init, security_init)
   ├─ [Phase 12] VFS & FS         (vfs_caches_init, proc_root_init)
   ├─ [Phase 13] Cgroups          (cgroup_init, mem_cgroup_init)
   ├─ [Phase 14] ACPI & Final     (acpi_subsystem_init, kcsan_init)
   └─ [Phase 15] rest_init()
            │
            ├── kernel_init thread (PID 1) → /sbin/init
            ├── kthreadd thread    (PID 2) → creates kernel threads
            └── cpu_idle()         (PID 0) ← start_kernel becomes this
```

---

## Complete Function Call List (94 calls, in order)

```c
void start_kernel(void)
{
    // ── PHASE 1: VERY EARLY ───────────────────────────────────────
    set_task_stack_end_magic(&init_task);       // stack overflow guard
    smp_setup_processor_id();                   // set processor ID
    debug_objects_early_init();                 // debug obj tracking
    init_vmlinux_build_id();                    // build ID init
    cgroup_init_early();                        // cgroup early setup
    local_irq_disable();                        // disable interrupts
    early_boot_irqs_disabled = true;

    // ── PHASE 2: CPU & ARCH ───────────────────────────────────────
    boot_cpu_init();                            // mark boot CPU online
    page_address_init();                        // highmem page tracking
    pr_notice("%s", linux_banner);              // print kernel version
    setup_arch(&command_line);                  // arch-specific setup ★ BIGGEST
    mm_core_init_early();                       // early mm setup
    jump_label_init();                          // static branch infra
    static_call_init();                         // static call patching
    early_security_init();                      // early LSM init

    // ── PHASE 3: CMDLINE & CPU SETUP ─────────────────────────────
    setup_boot_config();                        // process boot config
    setup_command_line(command_line);           // save cmdline
    setup_nr_cpu_ids();                         // number of CPUs
    setup_per_cpu_areas();                      // per-CPU data areas
    smp_prepare_boot_cpu();                     // arch boot CPU hooks
    early_numa_node_init();                     // NUMA node mappings
    boot_cpu_hotplug_init();                    // CPU hotplug init
    parse_early_param();                        // early params
    parse_args(...)                             // kernel args
    random_init_early(command_line);            // early RNG seed

    // ── PHASE 4: MEMORY INIT ─────────────────────────────────────
    setup_log_buf(0);                           // printk ring buffer
    vfs_caches_init_early();                    // early dcache/icache
    sort_main_extable();                        // sort exception table
    trap_init();                                // exception handlers ★
    mm_core_init();                             // buddy/slab/vmalloc ★
    maple_tree_init();                          // maple tree for VMAs
    poking_init();                              // live kernel patching
    ftrace_init();                              // function tracer
    early_trace_init();                         // early tracing

    // ── PHASE 5: SCHEDULER ───────────────────────────────────────
    sched_init();                               // process scheduler ★
    radix_tree_init();                          // radix tree

    // ── PHASE 6: WORKQUEUE & RCU ─────────────────────────────────
    housekeeping_init();                        // CPU isolation
    workqueue_init_early();                     // early workqueue
    rcu_init();                                 // RCU subsystem ★
    kvfree_rcu_init();                          // kvfree_rcu
    trace_init();                               // trace events

    // ── PHASE 7: IRQ & TIMERS ─────────────────────────────────────
    context_tracking_init();                    // kernel/user tracking
    early_irq_init();                           // IRQ descriptors
    init_IRQ();                                 // arch IRQ controller ★
    tick_init();                                // tick device
    rcu_init_nohz();                            // RCU nohz mode
    timers_init();                              // timer wheel
    srcu_init();                                // sleepable RCU
    hrtimers_init();                            // high-res timers
    softirq_init();                             // softirq vectors
    vdso_setup_data_pages();                    // VDSO data
    timekeeping_init();                         // wall clock ★
    time_init();                                // arch clocks
    random_init();                              // full RNG init

    // ── PHASE 8: SECURITY & PROTECTION ───────────────────────────
    kfence_init();                              // KFENCE memory checker
    boot_init_stack_canary();                   // stack canary
    perf_event_init();                          // perf subsystem
    profile_init();                             // kernel profiling
    call_function_init();                       // IPI call function
    local_irq_enable();                         // ★ RE-ENABLE INTERRUPTS
    kmem_cache_init_late();                     // late slab init

    // ── PHASE 9: CONSOLE & DEBUG ──────────────────────────────────
    console_init();                             // console drivers
    lockdep_init();                             // lock dependency
    locking_selftest();                         // lock self-tests

    // ── PHASE 10: PLATFORM & POLICY ──────────────────────────────
    setup_per_cpu_pageset();                    // per-CPU page sets
    numa_policy_init();                         // NUMA memory policy
    acpi_early_init();                          // early ACPI
    sched_clock_init();                         // scheduler clock
    calibrate_delay();                          // calibrate udelay
    arch_cpu_finalize_init();                   // final CPU setup

    // ── PHASE 11: NAMESPACE & PROCESS ────────────────────────────
    pid_idr_init();                             // PID allocator
    anon_vma_init();                            // anonymous VMA
    thread_stack_cache_init();                  // thread stack cache
    cred_init();                                // credentials
    fork_init();                                // fork/task infra ★
    proc_caches_init();                         // task/mm caches
    uts_ns_init();                              // UTS namespace
    time_ns_init();                             // time namespace
    key_init();                                 // key management
    security_init();                            // full LSM init

    // ── PHASE 12: VFS & FILESYSTEM ────────────────────────────────
    dbg_late_init();                            // kernel debugger
    net_ns_init();                              // network namespace
    vfs_caches_init();                          // full VFS caches ★
    pagecache_init();                           // page cache
    signals_init();                             // signal handling
    seq_file_init();                            // seq_file
    proc_root_init();                           // /proc filesystem
    nsfs_init();                                // namespace fs
    pidfs_init();                               // PID filesystem

    // ── PHASE 13: CGROUPS & RESOURCE ─────────────────────────────
    cpuset_init();                              // cpuset cgroup
    mem_cgroup_init();                          // memory cgroup
    cgroup_init();                              // full cgroup v2 ★
    taskstats_init_early();                     // task statistics
    delayacct_init();                           // delay accounting

    // ── PHASE 14: ACPI & FINAL ────────────────────────────────────
    acpi_subsystem_init();                      // full ACPI
    arch_post_acpi_subsys_init();               // post-ACPI arch hook
    kcsan_init();                               // concurrency sanitizer

    // ── PHASE 15: TRANSITION ──────────────────────────────────────
    rest_init();                                // spawn PID 1 & 2; become idle
}
```

---

## Phase Summary Table

| Phase | Name                      | Functions | IRQ State | Key Achievement              |
|-------|---------------------------|-----------|-----------|-------------------------------|
| 1     | Very Early Init           | 6         | Disabled  | Stack guard, CPU ID, cgroup early |
| 2     | CPU & Arch Init           | 7+        | Disabled  | Hardware map, paging, CPU online  |
| 3     | Cmdline & CPU Setup       | 8         | Disabled  | Per-CPU areas, command line parsed |
| 4     | Memory Init               | 10        | Disabled  | Buddy allocator, slab, trap handlers |
| 5     | Scheduler Init            | 2         | Disabled  | Runqueues ready, tasks can be created |
| 6     | Workqueue & RCU           | 5         | Disabled  | RCU readers safe, deferred work queued |
| 7     | IRQ & Timers              | 13        | → Enabled | Timekeeping, interrupts, RNG ready   |
| 8     | Security & Protection     | 6         | Enabled   | KFENCE, canary, perf, slab late init |
| 9     | Console & Debug           | 3         | Enabled   | Console output, lockdep active       |
| 10    | Platform & Policy         | 6         | Enabled   | NUMA policy, ACPI early, calibration |
| 11    | Namespace & Process       | 10        | Enabled   | fork() ready, credentials, security  |
| 12    | VFS & Filesystem          | 8+        | Enabled   | /proc mounted, file system usable    |
| 13    | Cgroups & Resource        | 5         | Enabled   | Resource control active              |
| 14    | ACPI & Final              | 3         | Enabled   | Full ACPI, KCSAN sanitizer           |
| 15    | rest_init                 | 1         | Enabled   | PID 1 (init) + PID 2 (kthreadd)     |

---

## Critical Kernel Invariants Established

### 1. Memory Management Chain
```
memblock  →  boot_alloc  →  buddy allocator  →  slab/slub  →  vmalloc
  (setup_arch)     (early)     (mm_core_init)     (mm_core_init)  (mm_core_init)
```

### 2. Interrupt State Transitions
```
start_kernel() entry: IRQs DISABLED (from arch assembly)
  │
  └─► local_irq_disable()  [explicit disable at Phase 1]
  │
  └─► [All critical setup with IRQs off]
  │
  └─► local_irq_enable()  [Phase 8 — after softirq_init + perf setup]
  │
  └─► [All user-space facing setup with IRQs ON]
```

### 3. PID Lifecycle
```
PID 0: idle thread = start_kernel() itself (after rest_init returns)
PID 1: kernel_init thread → execve("/sbin/init") → userspace init
PID 2: kthreadd → creates all subsequent kernel threads
```

---

## Data Flow: Command Line

```
BIOS/bootloader
    │ passes cmdline string
    ▼
setup_arch(&command_line)         ← fills command_line from boot_params
    │
setup_command_line(command_line)  ← saves to saved_command_line[], static_command_line[]
    │
parse_early_param()               ← __setup() macros with "early" flag
    │
parse_args("Booting kernel", ...) ← remaining __param[] entries
    │
parse_args("Setting init args")   ← after "--" separator
    │
init process                      ← receives leftover as argv
```

---

## Key Data Structures Involved

### `init_task` — The Idle Thread's task_struct
```c
// init/init_task.c
struct task_struct init_task = {
    .state        = 0,                     // TASK_RUNNING
    .stack        = init_stack,            // statically allocated stack
    .mm           = NULL,                  // kernel threads have no mm
    .active_mm    = &init_mm,              // but borrow init_mm
    .pid          = 0,                     // PID 0 = idle
    .comm         = "swapper",
    .thread_info  = INIT_THREAD_INFO(init_task),
    // ... 100+ fields
};
```

### `init_mm` — The Kernel Page Table
```c
// mm/init-mm.c
struct mm_struct init_mm = {
    .mm_rb      = RB_ROOT,
    .pgd        = swapper_pg_dir,   // arch-defined kernel PGD
    .mm_users   = ATOMIC_INIT(2),
    .mm_count   = ATOMIC_INIT(1),
    .mmap_lock  = __RWSEM_INITIALIZER(init_mm.mmap_lock),
};
```

---

## Dependency Graph (Simplified)

```
set_task_stack_end_magic
        │
boot_cpu_init ──► setup_arch ──────────┐
                      │                │
                      ▼                ▼
               mm_core_init_early   trap_init
                      │
                      ▼
                 mm_core_init ──────► slab allocator
                      │
                      ▼
                 sched_init ─────────► workqueue_init_early
                      │                        │
                      ▼                        ▼
                 rcu_init ──────────────► trace_init
                      │
                      ▼
               early_irq_init ──► init_IRQ ──► timekeeping_init
                                                     │
                                                     ▼
                                               random_init
                                                     │
                                                     ▼
                                              console_init
                                                     │
                                                     ▼
                                              fork_init ──► vfs_caches_init
                                                                   │
                                                                   ▼
                                                             cgroup_init
                                                                   │
                                                                   ▼
                                                             rest_init()
```

---

## Interview Q&A — NVIDIA / Google / Qualcomm Level

### Q1: What is the very first C function executed during Linux kernel boot and how does control reach it?
**A:**  
The first architecture-independent C function is `start_kernel()` in `init/main.c`. Control reaches it through a chain:
1. CPU starts executing firmware (BIOS/UEFI)
2. Bootloader (GRUB2) loads compressed kernel `vmlinuz` and executes its decompressor
3. `startup_64` (arch/x86/kernel/head_64.S) sets up initial page tables, GDT, temporary stack
4. Calls `x86_64_start_kernel()` → `x86_64_start_reservations()` → `start_kernel()`

Before `start_kernel()`:  interrupts disabled, paging enabled (identity mapped), BSS zeroed.

---

### Q2: Why are interrupts disabled at the start of `start_kernel()` and when/why are they re-enabled?
**A:**  
Interrupts are disabled because the IDT (Interrupt Descriptor Table) is not yet populated with real handlers — any interrupt would cause a triple fault and reboot. The interrupt controllers (APIC, PIC) are also not configured.

They are re-enabled at `local_irq_enable()` (Phase 8), after:
- `trap_init()` — IDT fully populated
- `init_IRQ()` — interrupt controller initialized
- `timekeeping_init()` — clock sources ready
- `softirq_init()` — softirq vectors populated
- `hrtimers_init()` — high-res timers ready

Without these being ready first, enabling interrupts would mean interrupts arrive with no handlers, corrupting kernel state.

---

### Q3: What is PID 0 and how does `start_kernel()` become it?
**A:**  
PID 0 is the **idle thread** (also called `swapper`). It is **not** created by `fork()` — it is the process that `start_kernel()` itself runs as. The `init_task` structure is statically allocated in BSS/data section at compile time. `start_kernel()` executes in the context of this `init_task`. After calling `rest_init()`, which spawns PID 1 and PID 2, `start_kernel()` never returns and the idle thread enters `cpu_idle()` — an infinite loop that executes `HLT` or `MWAIT` instructions when no other tasks need the CPU.

---

### Q4: Explain the memory allocator bootstrap problem in `start_kernel()`.
**A:**  
The kernel faces a chicken-and-egg problem: to set up the memory allocator, it needs memory — but there's no allocator yet.  
Solution: **Multi-stage bootstrap**:
1. `memblock` allocator (set up by `setup_arch`) — simple linear allocator from a list of free memory regions
2. `page_address_init` and `mm_core_init_early` — early page tracking
3. `trap_init` — needed before page faults work correctly
4. `mm_core_init()` — initializes the buddy allocator (zone-based page allocator), then slab allocator (kmalloc), then vmalloc

After `mm_core_init()`, `kmalloc()` and `kfree()` work. Before this, only `memblock_alloc()` is safe.

---

### Q5: What does `setup_arch()` do and why is it the most complex call in `start_kernel()`?
**A:**  
`setup_arch()` is architecture-specific (e.g., `arch/x86/kernel/setup.c`). It:
1. Parses E820/EFI memory map to build `memblock` — the early memory allocator's database
2. Parses boot command line for hardware hints
3. Parses device trees (ARM) or ACPI tables (x86)
4. Initializes early I/O mappings (`early_ioremap`)
5. Sets up final page tables (`paging_init`) and enables the real memory map
6. Initializes NUMA topology (which CPUs are close to which memory)
7. Does CPU-specific setup (MSR reads, feature detection via CPUID on x86)
8. Sets up kernel's virtual address space layout

It is the largest because hardware is extremely varied — the x86 version alone is ~1500 lines.

---

### Q6: What is the significance of calling `sched_init()` before `workqueue_init_early()` and `rcu_init()`?
**A:**  
`sched_init()` initializes the **per-CPU runqueues** (`struct rq`), the idle task, and the scheduling classes. Workqueues need to know about CPU topology and task scheduling to correctly assign workers to CPUs. RCU callbacks are processed by per-CPU softirq mechanisms that depend on the scheduler. If `sched_init()` were called after workqueues, the workqueue code would have no valid runqueue to target workers at, causing NULL pointer dereferences.

---

### Q7: What happens if the initrd overlaps with a reserved memory region?
**A:**  
Between `locking_selftest()` and `setup_per_cpu_pageset()`, there is a check:
```c
if (initrd_start && !initrd_below_start_ok &&
    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
    pr_crit("initrd overwritten...");
    initrd_start = 0;
}
```
If the initrd's start page falls below `min_low_pfn` (the lowest usable RAM page), it means the initrd was overwritten during memory init. The kernel disables the initrd and prints a `CRIT` warning. This typically means the bootloader placed the initrd in a memory region the kernel marked as reserved or unusable.

---

### Q8: How does `rest_init()` transition `start_kernel()` into the idle thread?
**A:**  
`rest_init()` (in `init/main.c`):
1. Calls `kernel_thread(kernel_init, NULL, CLONE_FS)` → creates PID 1 (`kernel_init`)
2. Pins that thread to the init task's core set
3. Calls `kernel_thread(kthreadd, NULL, CLONE_FS|CLONE_FILES)` → creates PID 2 (`kthreadd`)
4. Completes `kthreadd_done` completion
5. Calls `schedule_preempt_disabled()` → yields CPU to PID 1 to let it run
6. Returns to `start_kernel()` (after `rest_init()` returns)
7. `start_kernel()` returns but the call to `cpu_startup_entry(CPUHP_ONLINE)` (within `rest_init`) starts the idle loop

After this, PID 0 (`swapper`) runs the idle loop (`cpu_idle_loop()`) forever.

---

### Q9: Why does `console_init()` come so late in `start_kernel()`?
**A:**  
`console_init()` requires:
- `kmalloc()` — to allocate console driver structures (available after `mm_core_init`)
- `workqueue` — some console drivers may schedule work
- `irqs_enabled` — tty and serial drivers need safe IRQ context

It comes after `kmem_cache_init_late()` because slab caches are fully operational then. The kernel uses a "logbuf" mechanism (`setup_log_buf`, `printk()`) to buffer all `pr_*` output before `console_init()` — the log is flushed to the console once it's initialized.

---

### Q10: What is the role of `jump_label_init()` and why must it come before LSMs?
**A:**  
`jump_label_init()` initializes the **static branch** infrastructure — a performance-critical mechanism that converts branch conditions into either NOPs or JMPs in the kernel text at runtime, eliminating the branch prediction overhead.

LSMs (Linux Security Modules) like SELinux use static branches extensively for `security_hook_*` functions — they use `static_branch_likely()` and `static_branch_unlikely()` to enable/disable security checks at near-zero cost. If LSMs initialize before `jump_label_init()`, their static branches would be in an unknown state and the patch operations to enable them would corrupt kernel text. Hence the call order: `jump_label_init()` → `static_call_init()` → `early_security_init()`.

---

## Common Bugs and Pitfalls

| Bug | Description | Historical Fix |
|-----|-------------|----------------|
| Stack overflow in early boot | Stack too small in arch assembly for `start_kernel` locals | Increased `THREAD_SIZE` in affected arches |
| initrd overwritten | Bootloader places initrd below `min_low_pfn` | Bootloader updated to honor `E820_TYPE_RESERVED` entries |
| Double initrd init | `vfs_caches_init_early()` called without locking | Fixed by explicit ordering in `start_kernel` |
| RNG not seeded early | Early `kmalloc()` used uninitialized memory for keys | Added `random_init_early()` before first big allocations |
| Lockdep false positives | `locking_selftest()` generates expected violations | Wrapped in `lockdep_reset()` before and after test |
| `console_init()` before slab | Some drivers called `kmalloc` inside `console_init` | Moved `console_init` to after `kmem_cache_init_late` |

---

## References

- `init/main.c` — Full `start_kernel()` body
- `Documentation/admin-guide/kernel-parameters.txt` — All `__setup()` and `__param` entries
- `Documentation/core-api/boot-time-mm.rst` — memblock and early allocator
- `Documentation/scheduler/sched-design-CFS.rst` — CFS scheduler design
- `Documentation/RCU/` — RCU internals
- `Documentation/security/lsm.rst` — LSM framework

---

*Document part of: `start_kernel` Deep Boot Documentation Series*  
*Architecture focus: x86-64 primary, with ARM64/RISC-V notes where applicable*
