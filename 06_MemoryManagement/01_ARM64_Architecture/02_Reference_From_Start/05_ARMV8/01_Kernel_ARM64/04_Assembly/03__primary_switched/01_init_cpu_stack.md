---

## `init_cpu_task x4, x5, x6` — Deep Explanation from `__primary_switched`

---

### Context: What We Have Before This Line

```
x4 = &init_task       ← virtual address of PID 0's task_struct (from previous adr_l)
x5 = scratch (tmp1)
x6 = scratch (tmp2)
MMU = ON              ← kernel virtual addresses work
Stack = early_init_stack  ← a temporary boot stack, not a real task stack
current = unknown     ← no task is "running" yet
```

The CPU is executing real kernel code but **has no identity** — no task, no proper stack, no per-CPU data. This macro fixes all of that.

---
Explaining initialization of CPU task

The initialization function sets up the CPU task by allocating necessary resources and preparing internal structures for execution. It configures the task's context and state so the CPU can manage it efficiently, ensuring proper scheduling and execution flow. This process also handles dependencies and resource bindings needed before the task runs, enabling smooth integration with the system's workload management.

What it is, in technical terms:

init_cpu_task is the bootstrap context-binding step that turns a raw booting core into a fully identified Linux execution context.

What it does:

1. Task identity binding  
It assigns the currently running hardware core to a specific scheduler entity, the initial task descriptor (PID 0, swapper). After this, the kernel can answer “who is current?” on that CPU.

2. Kernel stack handoff  
It moves execution from the temporary early-boot stack to the permanent task stack associated with that initial task. This is critical for stable function calls, interrupts, and later context-switch assumptions.

3. Unwind and debug baseline establishment  
It creates the terminal frame metadata so stack unwinding has a valid stopping point. That makes early backtraces, panic diagnostics, and frame walking reliable instead of undefined.

4. Security context continuation  
If shadow call stack is enabled, it loads the task’s shadow-call-stack pointer, so return-address protection is consistent from this point onward.

5. Per-CPU namespace activation  
It programs the CPU-local offset register so per-CPU variables resolve to this core’s private memory instance. Without this, many core subsystems would read or write the wrong CPU-local state.

Why it must happen at this moment:

- The MMU has just been enabled and kernel virtual mapping is now usable.
- The CPU is about to enter generic kernel paths that assume valid current task, valid stack, valid unwind state, and valid per-CPU base.
- It is the boundary between architecture boot code and normal kernel execution semantics.

What happens to CPU and memory conceptually:

- CPU architectural state changes:
  - Current-task pointer register becomes meaningful
  - Stack pointer becomes task-owned instead of temporary bootstrap-owned
  - Per-CPU base register is initialized
  - Optional shadow stack register is initialized

- Memory interpretation changes:
  - The task structure becomes the authoritative runtime descriptor for this CPU
  - The task’s stack region becomes the active call-stack memory
  - Per-CPU variables become correctly addressable for this specific core
  - Unwinder metadata in stack memory defines a safe terminal frame

In one line: this step is where the boot CPU stops being “just a core executing startup instructions” and becomes “CPU 0 running the kernel as task 0 with proper stack, per-CPU state, and runtime invariants.”


### The Macro Expands To

```asm
init_cpu_task x4, x5, x6
↓ expands to:

msr     sp_el0, x4                      // line 1
ldr     x5, [x4, #TSK_STACK]            // line 2
add     sp, x5, #THREAD_SIZE            // line 3
sub     sp, sp, #PT_REGS_SIZE           // line 4
stp     xzr, xzr, [sp, #S_STACKFRAME]  // line 5
mov     x5, #FRAME_META_TYPE_FINAL      // line 6
str     x5, [sp, #S_STACKFRAME_TYPE]   // line 7
add     x29, sp, #S_STACKFRAME         // line 8
scs_load_current                        // line 9
adr_l   x5, __per_cpu_offset           // line 10
ldr     w6, [x4, #TSK_TI_CPU]          // line 11
ldr     x5, [x5, x6, lsl #3]           // line 12
msr     tpidr_el1, x5                  // line 13
```

---

## Line 1 — `msr sp_el0, x4`

### What

```
sp_el0 ← x4 = &init_task
```

### Why

On ARM64 at EL1 (kernel mode), `sp_el0` is **not used by hardware as a stack pointer**. Linux deliberately repurposes it as the **"current task" pointer**.

The C macro `current` is defined as:

```c
// arch/arm64/include/asm/assembler.h
.macro get_current_task, rd
    mrs  \rd, sp_el0      // READ sp_el0 → get current task_struct*
.endm
```

### What happens to CPU and code

**Before:** `current` returns garbage — `sp_el0` was never set  
**After:** `current` returns `&init_task` everywhere in the kernel — the CPU now has a task identity

This single instruction is the moment **the CPU "becomes" PID 0**.

---

## Lines 2–4 — Stack Setup

### Line 2: `ldr x5, [x4, #TSK_STACK]`

```
x5 = init_task.stack = &init_stack
```

`TSK_STACK = offsetof(struct task_struct, stack)` — from asm-offsets.c

`init_task.stack` was set at compile time:
```c
// init/init_task.c
struct task_struct init_task = {
    .stack = init_stack,   // ← points to 16 KB static array
    ...
};
```

So `x5` now holds the **base address of the 16 KB boot stack**.

---

### Line 3: `add sp, x5, #THREAD_SIZE`

```
sp = &init_stack + 16384   ← top of the 16 KB stack region
```

`THREAD_SIZE = (1 << 14) = 16384 bytes` — from memory.h

Stacks grow **downward** on ARM64. So you start `sp` at the **top** of the region.

---

### Line 4: `sub sp, sp, #PT_REGS_SIZE`

```
sp = sp - sizeof(struct pt_regs)   ← carve out space at top of stack
```

`PT_REGS_SIZE = sizeof(struct pt_regs)` — from asm-offsets.c

**Why carve this out?** Every task's kernel stack **reserves a `pt_regs` slot at the very top**. This is where the stack frame sentinel lives. It makes every task — including `init_task` — consistent with userspace tasks and kthreads.

**Memory picture after lines 2–4:**

```
init_stack memory region (16 KB):

High: init_stack + 16384
      ┌─────────────────────────┐
      │   struct pt_regs        │  ← sp points HERE (top of stack, reserved)
      │   [S_STACKFRAME]        │     size = PT_REGS_SIZE
      │   [S_STACKFRAME_TYPE]   │
      ├─────────────────────────┤
      │                         │
      │   usable kernel stack   │  ← grows downward from sp
      │   (function calls go    │
      │    here from now on)    │
      │                         │
Low:  init_stack (base)         │
      └─────────────────────────┘
```

**Before lines 2–4:** `sp` pointed to `early_init_stack` (a tiny temporary boot stack)  
**After:** `sp` points to the top of `init_stack` — the real permanent kernel stack for PID 0

---

## Lines 5–8 — Stack Frame Sentinel (Bottom-of-Stack Marker)

### Line 5: `stp xzr, xzr, [sp, #S_STACKFRAME]`

```
pt_regs.stackframe.fp = 0
pt_regs.stackframe.lr = 0
```

`S_STACKFRAME = offsetof(struct pt_regs, stackframe)` — writes two zero 64-bit values into the `frame_record_meta` field of the reserved `pt_regs`.

`fp=0, lr=0` is the **AAPCS64 standard sentinel** meaning: *"no more frames above this — stop unwinding here"*.

---

### Lines 6–7: `mov x5, #1` + `str x5, [sp, #S_STACKFRAME_TYPE]`

```
pt_regs.stackframe.type = FRAME_META_TYPE_FINAL (= 1)
```

From frame.h:
```c
#define FRAME_META_TYPE_FINAL  1  // "stop unwinding here, success"
```

This tells the **kernel stack unwinder** (used by `backtrace`, `panic`, `perf`): *"You have reached the bottom of init_task's stack. Stop here cleanly."*

Without this, stack traces would crash or show garbage at the end.

---

### Line 8: `add x29, sp, #S_STACKFRAME`

```
x29 (frame pointer) = &pt_regs.stackframe
```

`x29` is the **frame pointer register** on ARM64. Every function's stack frame has `{x29, x30}` saved at its base. This line sets `x29` to point at the sentinel frame record — anchoring the entire call chain to this known-final-frame location.

**After lines 5–8:** The stack has a clean, valid bottom marker. Any stack dump from now on will terminate correctly at this point.

---

## Line 9 — `scs_load_current`

### What (if `CONFIG_SHADOW_CALL_STACK` is enabled)

```asm
mrs  x18, sp_el0              // x18 = &init_task  (current)
ldr  x18, [x18, #TSK_TI_SCS_SP]  // x18 = init_task.thread_info.scs_sp
```

### Why

ARM64 Linux uses **`x18` as a dedicated Shadow Call Stack pointer**. The SCS is a **separate secret stack** that stores only return addresses. If an attacker overflows the main stack and overwrites a return address, the SCS still has the correct one — this defeats **ROP (Return-Oriented Programming) attacks**.

After this line, `x18` points to `init_task`'s shadow call stack. All `bl` instructions from here on will push return addresses there too.

If SCS is not compiled in, this is a **no-op**.

---

## Lines 10–13 — Per-CPU Data Setup

This is the most architecturally interesting part.

### What is "per-CPU data"?

On SMP (multi-core) systems, every CPU needs its **own private copy** of certain variables — e.g., run queues, interrupt counters, `current_task`. Linux maintains these as **per-CPU variables**.

All per-CPU copies live in memory at different base addresses. `__per_cpu_offset[N]` = byte offset to CPU N's data from the "prototype" section.

---

### Line 10: `adr_l x5, __per_cpu_offset`

```
x5 = address of __per_cpu_offset[]   ← base of the offset table
```

`__per_cpu_offset` is a global array: `unsigned long __per_cpu_offset[NR_CPUS]`

---

### Line 11: `ldr w6, [x4, #TSK_TI_CPU]`

```
w6 = init_task.thread_info.cpu
```

`TSK_TI_CPU = offsetof(struct task_struct, thread_info.cpu)` — from asm-offsets.c

`init_task.thread_info.cpu = 0` — the boot CPU is always CPU 0.

---

### Line 12: `ldr x5, [x5, x6, lsl #3]`

```
x5 = __per_cpu_offset[w6]
   = __per_cpu_offset[0]
   = byte offset to CPU 0's per-CPU data
```

`lsl #3` = multiply index by 8 (each entry is a 64-bit pointer).

---

### Line 13: `msr tpidr_el1, x5`  (`set_this_cpu_offset`)

```
tpidr_el1 = __per_cpu_offset[0]
```

`tpidr_el1` is an ARM64 **system register** — it's the CPU's dedicated per-CPU base register. Every time any kernel code does `this_cpu_read(var)` or `per_cpu(var, smp_processor_id())`, it internally does:

```
address = &var_prototype + tpidr_el1
```

**Before line 13:** `tpidr_el1` is unset — any `this_cpu_*` call would access wrong memory  
**After line 13:** CPU 0 has its full per-CPU data universe available

---

## Complete Before/After Picture

| What | Before `init_cpu_task` | After `init_cpu_task` |
|---|---|---|
| `current` (`sp_el0`) | garbage | `&init_task` (PID 0) |
| `sp` | `early_init_stack` (temp) | top of `init_stack` (16 KB real stack) |
| Frame pointer `x29` | meaningless | anchored to stack sentinel |
| Stack unwind | would crash/corrupt | terminates cleanly at sentinel |
| Shadow Call Stack `x18` | unset | `init_task.thread_info.scs_sp` |
| `tpidr_el1` (per-CPU) | unset | `__per_cpu_offset[0]` |
| `this_cpu_*` calls | undefined behavior | fully functional |

---

## One-Sentence Summary Per Line

```
msr sp_el0, x4          → CPU gains its identity: "I am running init_task (PID 0)"
ldr x5, [x4, #TSK_STACK] → read where init_task's stack lives
add sp, x5, THREAD_SIZE  → point sp to top of that 16 KB stack
sub sp, sp, PT_REGS_SIZE → carve out pt_regs space at top
stp xzr, xzr, ...        → write fp=0, lr=0: "stack ends here"
mov/str FRAME_META_FINAL  → mark it officially as the final frame for the unwinder
add x29, sp, S_STACKFRAME → frame pointer anchored to that sentinel
scs_load_current          → load shadow call stack from init_task (ROP protection)
adr_l x5, __per_cpu_offset → find the per-CPU offset table
ldr w6, [x4, TSK_TI_CPU] → read CPU number from init_task (= 0)
ldr x5, [x5, w6<<3]      → look up offset for CPU 0
msr tpidr_el1, x5         → CPU 0 now owns its per-CPU data region


/*
 * Set up the first task table, touch at your own risk!. Base=0,
 * limit=0x1fffff (=2MB)
 */
struct task_struct init_task __aligned(L1_CACHE_BYTES) = {
#ifdef CONFIG_THREAD_INFO_IN_TASK
	.thread_info	= INIT_THREAD_INFO(init_task),
	.stack_refcount	= REFCOUNT_INIT(1),
#endif
	.__state	= 0,
	.stack		= init_stack,
	.usage		= REFCOUNT_INIT(2),
	.flags		= PF_KTHREAD,
	.prio		= MAX_PRIO - 20,
	.static_prio	= MAX_PRIO - 20,
	.normal_prio	= MAX_PRIO - 20,
	.policy		= SCHED_NORMAL,
	.cpus_ptr	= &init_task.cpus_mask,
	.user_cpus_ptr	= NULL,
	.cpus_mask	= CPU_MASK_ALL,
	.max_allowed_capacity	= SCHED_CAPACITY_SCALE,
	.nr_cpus_allowed= NR_CPUS,
	.mm		= NULL,
	.active_mm	= &init_mm,
	.restart_block	= {
		.fn = do_no_restart_syscall,
	},
	.se		= {
		.group_node 	= LIST_HEAD_INIT(init_task.se.group_node),
	},
	.rt		= {
		.run_list	= LIST_HEAD_INIT(init_task.rt.run_list),
		.time_slice	= RR_TIMESLICE,
	},
	.tasks		= LIST_HEAD_INIT(init_task.tasks),
#ifdef CONFIG_SMP
	.pushable_tasks	= PLIST_NODE_INIT(init_task.pushable_tasks, MAX_PRIO),
#endif
#ifdef CONFIG_CGROUP_SCHED
	.sched_task_group = &root_task_group,
#endif
#ifdef CONFIG_SCHED_CLASS_EXT
	.scx		= {
		.dsq_list.node	= LIST_HEAD_INIT(init_task.scx.dsq_list.node),
		.sticky_cpu	= -1,
		.holding_cpu	= -1,
		.runnable_node	= LIST_HEAD_INIT(init_task.scx.runnable_node),
		.runnable_at	= INITIAL_JIFFIES,
		.ddsp_dsq_id	= SCX_DSQ_INVALID,
		.slice		= SCX_SLICE_DFL,
	},
#endif
	.ptraced	= LIST_HEAD_INIT(init_task.ptraced),
	.ptrace_entry	= LIST_HEAD_INIT(init_task.ptrace_entry),
	.real_parent	= &init_task,
	.parent		= &init_task,
	.children	= LIST_HEAD_INIT(init_task.children),
	.sibling	= LIST_HEAD_INIT(init_task.sibling),
	.group_leader	= &init_task,
	RCU_POINTER_INITIALIZER(real_cred, &init_cred),
	RCU_POINTER_INITIALIZER(cred, &init_cred),
	.comm		= INIT_TASK_COMM,
	.thread		= INIT_THREAD,
	.fs		= &init_fs,
	.files		= &init_files,
#ifdef CONFIG_IO_URING
	.io_uring	= NULL,
#endif
	.signal		= &init_signals,
	.sighand	= &init_sighand,
	.nsproxy	= &init_nsproxy,
	.pending	= {
		.list = LIST_HEAD_INIT(init_task.pending.list),
		.signal = {{0}}
	},
	.blocked	= {{0}},
	.alloc_lock	= __SPIN_LOCK_UNLOCKED(init_task.alloc_lock),
	.journal_info	= NULL,
	INIT_CPU_TIMERS(init_task)
	.pi_lock	= __RAW_SPIN_LOCK_UNLOCKED(init_task.pi_lock),
	.blocked_lock	= __RAW_SPIN_LOCK_UNLOCKED(init_task.blocked_lock),
	.timer_slack_ns = 50000, /* 50 usec default slack */
	.thread_pid	= &init_struct_pid,
	.thread_node	= LIST_HEAD_INIT(init_signals.thread_head),
#ifdef CONFIG_AUDIT
	.loginuid	= INVALID_UID,
	.sessionid	= AUDIT_SID_UNSET,
#endif
#ifdef CONFIG_PERF_EVENTS
	.perf_event_mutex = __MUTEX_INITIALIZER(init_task.perf_event_mutex),
	.perf_event_list = LIST_HEAD_INIT(init_task.perf_event_list),
#endif
#ifdef CONFIG_PREEMPT_RCU
	.rcu_read_lock_nesting = 0,
	.rcu_read_unlock_special.s = 0,
	.rcu_node_entry = LIST_HEAD_INIT(init_task.rcu_node_entry),
	.rcu_blocked_node = NULL,
#endif
#ifdef CONFIG_TASKS_RCU
	.rcu_tasks_holdout = false,
	.rcu_tasks_holdout_list = LIST_HEAD_INIT(init_task.rcu_tasks_holdout_list),
	.rcu_tasks_idle_cpu = -1,
	.rcu_tasks_exit_list = LIST_HEAD_INIT(init_task.rcu_tasks_exit_list),
#endif
#ifdef CONFIG_TASKS_TRACE_RCU
	.trc_reader_nesting = 0,
#endif
#ifdef CONFIG_CPUSETS
	.mems_allowed_seq = SEQCNT_SPINLOCK_ZERO(init_task.mems_allowed_seq,
						 &init_task.alloc_lock),
#endif
#ifdef CONFIG_RT_MUTEXES
	.pi_waiters	= RB_ROOT_CACHED,
	.pi_top_task	= NULL,
#endif
	INIT_PREV_CPUTIME(init_task)
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	.vtime.seqcount	= SEQCNT_ZERO(init_task.vtime_seqcount),
	.vtime.starttime = 0,
	.vtime.state	= VTIME_SYS,
#endif
#ifdef CONFIG_NUMA_BALANCING
	.numa_preferred_nid = NUMA_NO_NODE,
	.numa_group	= NULL,
	.numa_faults	= NULL,
#endif
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	.kasan_depth	= 1,
#endif
#ifdef CONFIG_KCSAN
	.kcsan_ctx = {
		.scoped_accesses	= {LIST_POISON1, NULL},
	},
#endif
#ifdef CONFIG_TRACE_IRQFLAGS
	.softirqs_enabled = 1,
#endif
#ifdef CONFIG_LOCKDEP
	.lockdep_depth = 0, /* no locks held yet */
	.curr_chain_key = INITIAL_CHAIN_KEY,
	.lockdep_recursion = 0,
#endif
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	.ret_stack		= NULL,
	.tracing_graph_pause	= ATOMIC_INIT(0),
#endif
#if defined(CONFIG_TRACING) && defined(CONFIG_PREEMPTION)
	.trace_recursion = 0,
#endif
#ifdef CONFIG_LIVEPATCH
	.patch_state	= KLP_TRANSITION_IDLE,
#endif
#ifdef CONFIG_SECURITY
	.security	= NULL,
#endif
#ifdef CONFIG_SECCOMP_FILTER
	.seccomp	= { .filter_count = ATOMIC_INIT(0) },
#endif
#ifdef CONFIG_SCHED_MM_CID
	.mm_cid		= { .cid = MM_CID_UNSET, },
#endif
};
EXPORT_SYMBOL(init_task);