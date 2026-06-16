# `start_kernel()` — Boot Phases Narrative

This document tells the story of `start_kernel()` as a sequence of phases, explaining *why* things happen in the order they do, and what the kernel can and cannot do at each point.

---

## The Core Constraint: IRQs Must Stay Disabled Until the Kernel Is Ready

The kernel enters `start_kernel()` with interrupts disabled. This is not optional — if a timer interrupt fires before the timer infrastructure is initialized, or an NMI fires before the exception table is sorted, the kernel will crash with no diagnostics. Every function called before `local_irq_enable()` at ~line 993 must work without any interrupt, context switch, or preemption.

This constraint shapes the entire structure of `start_kernel()`.

---

## Phase 1: Bare Minimum Setup (lines 879–891)

**IRQ state:** Disabled (not yet explicitly set — inherited from arch assembly)  
**Memory:** Only `memblock` static regions; no allocations yet  
**Printk:** Only `earlycon`-level output

The very first actions protect the kernel from itself:

1. **`set_task_stack_end_magic(&init_task)`** — The boot CPU runs on `init_task`'s stack. This writes a canary value at the bottom of that stack so that stack overflows during boot will be detected rather than silently corrupting memory.

2. **`smp_setup_processor_id()`** — Records which CPU is the boot CPU. On most architectures this is CPU 0, but on some systems (e.g., some ARM boards), the boot CPU may be CPU 1 or higher.

3. **`debug_objects_early_init()`** — Linux has a powerful "OBJECT_DEBUG" infrastructure that validates object lifecycles (initialized → used → destroyed). This bootstraps that system with a statically allocated pool.

4. **`init_vmlinux_build_id()`** — Records the vmlinux ELF build-ID, used for crash dump correlation.

5. **`cgroup_init_early()`** — The cgroup subsystem needs its root `cgroup` struct available before almost anything else, because many subsystems register themselves as cgroup controllers. This pre-allocates the root and sets up subsystem ordering.

6. **`local_irq_disable()` + `early_boot_irqs_disabled = true`** — Now interrupts are explicitly disabled with a recorded flag. The `early_boot_irqs_disabled` flag is used by `might_sleep()` and other checkers to know that normal locking rules do not yet apply.

7. **`boot_cpu_init()`** — Sets the boot CPU in the four key CPU bitmasks: `cpu_possible_mask`, `cpu_present_mask`, `cpu_online_mask`, and `cpu_active_mask`. Without this, `for_each_online_cpu()` loops would not include the boot CPU.

8. **`page_address_init()`** — On systems with high memory (32-bit with > 4 GB RAM), Linux uses a hash table to map `struct page *` pointers to virtual addresses. This initializes that hash table.

---

## Phase 2: Architecture Initialization (lines 893–900)

**IRQ state:** Disabled  
**Memory:** memblock (reads arch memory map to populate it)

This is the largest and most complex phase. `setup_arch()` is different for every architecture and does whatever is needed to establish the kernel's view of the machine.

**`early_security_init()`** comes first because `setup_arch()` may need to make security decisions (e.g., setting up secure boot measurement roots). This initializes the ordered list of Linux Security Modules.

**`setup_arch(&command_line)`** on x86-64 does, among other things:
- Reads the e820 memory map from `boot_params` (populated by BIOS/bootloader)
- Calls `memblock_add()` for each RAM region
- Reserves memory for the kernel image, initrd, ACPI tables, etc.
- Detects CPU type (Intel/AMD/...), calls `early_cpu_init()`
- Sets up the early kernel page tables with 4-level or 5-level paging
- Initializes the NUMA topology
- Parses ACPI SRAT/SLIT tables for memory locality

After `setup_arch()`, `memblock` is fully populated and the kernel knows where all RAM is.

**`setup_command_line()`** allocates two copies of the command line using `memblock_alloc()`:
- `saved_command_line` — Never modified; accessible via `/proc/cmdline`
- `static_command_line` — Parsed in-place (parameters are NUL-terminated in-place)

**`setup_per_cpu_areas()`** replicates the kernel's `.data..percpu` section for each possible CPU. This is how per-CPU variables work: each CPU gets its own copy at a different base address, and `this_cpu_read()` uses the CPU's base offset to access its own copy without any locking.

---

## Phase 3: Parameter Parsing (lines 904–917)

**IRQ state:** Disabled  
**Memory:** memblock

**`jump_label_init()`** must happen before any `static_key_*()` calls. Linux uses static keys (also called jump labels) to implement near-zero-cost conditional code. At compile time, code that uses `static_branch_unlikely()` compiles to a NOP; at runtime, if the key is enabled, the NOP is patched to a JMP. This function scans the `.jump_table` section and applies the initial patches.

**`parse_early_param()`** scans the command line for parameters registered with `early_param("name", handler)`. Examples: `earlycon=`, `earlyprintk=`, `mem=`, `nokaslr`. These must be processed before general parameter parsing because they affect how the kernel sets up memory and console.

**`parse_args("Booting kernel", ...)`** processes the remaining kernel parameters using the `__param` section (parameters registered with `module_param()`, `core_param()`, etc.).

**`random_init_early()`** seeds the CRNG using the command line string as a source of uniqueness. This ensures that even before hardware entropy is available, different kernel boots with different parameters produce different random numbers.

---

## Phase 4: Core Memory & Exceptions (lines 923–929)

**IRQ state:** Disabled  
**Memory:** memblock → buddy allocator (after `mm_core_init()`)

This is the most consequential phase for memory management.

**`setup_log_buf(0)`** allocates the printk ring buffer. Until this call, printk messages are stored in a tiny static fallback buffer. After this, the full ring buffer is available.

**`vfs_caches_init_early()`** creates the dentry and inode slab caches. These are needed before `mm_core_init()` because the page allocator initialization code itself creates files in the VFS.

**`sort_main_extable()`** sorts the kernel's exception table. The exception table maps fault addresses to recovery code — e.g., when `copy_from_user()` takes a page fault because the user pointer is invalid, the exception table tells the fault handler to jump to recovery code that returns `-EFAULT`. The table must be sorted for binary search.

**`trap_init()`** installs the CPU's exception handlers. On x86, this sets up the IDT (Interrupt Descriptor Table) with handlers for:
- `#DE` (divide by zero)
- `#PF` (page fault) → `do_page_fault()`
- `#GP` (general protection fault)
- `#NM` (device not available / FPU)
- `NMI` (non-maskable interrupt)
- `#DB` (debug exception)
- And all others

**`mm_core_init()`** is the point of transition from `memblock` to the buddy allocator. It:
1. Frees all `memblock`-managed free pages to the buddy allocator
2. Initializes the per-zone page lists
3. Bootstraps SLAB/SLUB (the slab allocator needs the page allocator, but the page allocator needs `kmalloc` for zone metadata — a chicken-and-egg solved with static boot caches)
4. Initializes page extension tracking

After `mm_core_init()`, `kmalloc()`, `kzalloc()`, and `vmalloc()` all work.

---

## Phase 5–8: Tracing, Scheduling, Data Structures, RCU (lines 932–967)

**IRQ state:** Disabled  
**Memory:** buddy + slab

**`ftrace_init()`** scans all `mcount` call sites in the kernel image and records them. This is a one-time table build. Later, when function tracing is enabled, these sites are patched from NOP to `call __fentry__`.

**`sched_init()`** initializes the per-CPU runqueues and all scheduler classes. After this call, the kernel has a functioning scheduler in data structure terms — but because IRQs are still disabled and there's only one task (`init_task`), no actual scheduling happens yet.

**`workqueue_init_early()`** creates the early workqueue infrastructure. Work items can be *queued* after this call, but they won't *execute* until kthreads exist (after `workqueue_init()` in `kernel_init_freeable()`).

**`rcu_init()`** initializes Tree RCU. RCU (Read-Copy-Update) is the kernel's most important synchronization primitive for read-heavy data structures. The grace period tracking kthread is created here.

---

## Phase 9: Interrupt & Timer Infrastructure (lines 969–978)

**IRQ state:** Still Disabled (we're setting up the handlers, not enabling them)  
**Memory:** buddy + slab

This phase installs all the machinery needed for the system to become interrupt-driven, but does not yet turn interrupts on.

**`early_irq_init()`** allocates the `irq_desc[]` array — one descriptor per IRQ number — using `memblock`. Each descriptor tracks the IRQ's handler, status, chip, and thread.

**`init_IRQ()`** is arch-specific. On x86, it:
- Programs the Local APIC to accept and route interrupts
- Programs the IO-APIC to map hardware IRQ lines to APIC vectors
- Falls back to 8259 PIC for legacy systems
- Installs the timer interrupt handler (but doesn't start the timer yet)

**`timekeeping_init()`** initializes the kernel's timekeeping infrastructure:
- `xtime` — The wall clock (seconds + nanoseconds since Unix epoch)
- `timekeeper` — The struct that tracks clock state
- Selects the best available clocksource

**`time_init()`** is arch-specific and registers the actual hardware clocksource. On x86, this may use the TSC (Time Stamp Counter), HPET (High Precision Event Timer), or ACPI PM timer.

---

## The IRQ Enable Point (line 993)

```c
early_boot_irqs_disabled = false;
local_irq_enable();
```

This is one of the most significant lines in the entire boot sequence. After ~56 function calls with interrupts disabled, the kernel is now ready to handle hardware events. From this point:
- The timer interrupt will fire regularly
- IRQ handlers will run
- Preemption becomes possible (once `preempt_count` allows it)
- `might_sleep()` checks become active

---

## Phase 12: Console Online (lines 995–1012)

**IRQ state:** Enabled

**`kmem_cache_init_late()`** completes the SLUB/SLAB initialization that requires IRQs (it sets up per-CPU slab caches that use IRQ-disabled critical sections).

**`console_init()`** is the moment the kernel first gets a real console. Before this, messages go only to `earlycon`. After this, all registered TTY console drivers are available. The printk subsystem flushes its backlog of early messages to the console here.

**`lockdep_init()`** initializes the lock dependency validator, which tracks the order in which locks are acquired and validates it against historical patterns to detect deadlock risks.

---

## Phases 13–16: Process Infrastructure (lines 1022–1060)

**IRQ state:** Enabled

These phases build up the infrastructure needed for the first process (PID 1 / `init`) to run:

- **`calibrate_delay()`** measures `loops_per_jiffy` — the number of empty loop iterations per timer tick. This is used by `udelay()` and `mdelay()` for busy-wait delays.
- **`fork_init()`** creates the `task_struct` slab cache and calculates `max_threads` based on available memory.
- **`vfs_caches_init()`** completes VFS initialization with proper hash table sizes.
- **`cgroup_init()`** completes cgroup v1/v2 initialization and registers the cgroup filesystem.

---

## Phase 17: Transition to Rest (line 1063)

**`arch_call_rest_init()`** calls `rest_init()`, which:
1. Creates the `kernel_init` thread (PID 1) — this becomes the `init` process
2. Creates the `kthreadd` thread (PID 2) — manages all kernel threads
3. Sets `system_state = SYSTEM_SCHEDULING`
4. Calls `cpu_startup_entry(CPUHP_ONLINE)` — the boot CPU enters the idle loop forever

`start_kernel()` never returns.

---

## The Big Picture: Why This Order?

The ordering of `start_kernel()` follows a strict dependency graph:

```
memblock ──────────────────────────────────► page allocator
                                                   │
page allocator ────────────────────────────► slab allocator
                                                   │
slab allocator ─────────┬──────────────────► kmalloc works
                        │
trap_init (IDT) ────────┼──────────────────► exceptions safe
                        │
timer infrastructure ───┼──────────────────► local_irq_enable() safe
                        │
IRQs enabled ───────────┼──────────────────► console_init safe
                        │
fork_init ──────────────┼──────────────────► user processes possible
                        │
vfs_caches_init ─────────────────────────── filesystems possible
```

Every function in `start_kernel()` moves the system one step further along this dependency chain.
