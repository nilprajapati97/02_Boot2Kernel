# `init_cpu_task` — Overview Interview Q&A

---

## Q1: In one sentence, what does `init_cpu_task` do?

**A:** It establishes the minimum CPU context (current task pointer, kernel stack,
frame sentinel, shadow call stack, per-CPU offset) that every kernel C function
assumes is valid.

---

## Q2: Why must `init_cpu_task` run before calling any C function?

**A:** C functions compiled for the Linux kernel make assumptions that are ONLY
true after `init_cpu_task` runs:

1. `current` works (needs `sp_el0` = task pointer)
2. Stack is valid and 16KB (needs stack switch)
3. `this_cpu_read()` works (needs `tpidr_el1` = per-CPU offset)
4. SCS is tracking return addresses (needs `x18` = SCS pointer)
5. Stack traces work (needs frame sentinel at `fp=0`)

Violating any of these assumptions causes undefined behavior ranging from wrong
values to immediate kernel panic.

---

## Q3: What would a Qualcomm interviewer likely ask about `init_cpu_task`?

**A:** Common interview angles:

1. **"What is sp_el0 and why does Linux use it for `current`?"**
   — Answer: sp_el0 is architecturally unused at EL1; Linux repurposes it as a
   dedicated "current task" register for zero-memory-traffic access.

2. **"Why is tpidr_el1 used instead of a global variable for per-CPU data?"**
   — Answer: `tpidr_el1` is a per-CPU register — each CPU has its own private copy.
   A global variable would require a lock or atomic operation; `tpidr_el1` gives
   each CPU a private base pointer with zero synchronization overhead.

3. **"What happens on secondary CPU boot — does init_cpu_task run again?"**
   — Answer: Yes, the same macro runs on every CPU. On secondary CPUs, the `tsk`
   parameter points to the idle task for that CPU (allocated by `cpu_up`), not `init_task`.

4. **"Explain the frame sentinel — why fp=0 instead of some magic value?"**
   — Answer: `fp=0` is the industry convention for "bottom of call stack." The
   ARM64 unwinder follows fp chains and terminates when it encounters NULL. A magic
   value would require the unwinder to know the specific value — NULL is universal.

5. **"Could you implement `current` with a global variable instead?"**
   — Answer: On a uniprocessor, yes. On SMP, you'd need a per-CPU global OR
   an indexed global keyed on CPU ID. Both require extra instructions. `sp_el0`
   gives the same result in a single `mrs` instruction without any indexing.

---

## Q4: What are the NVIDIA GPU driver implications of `init_cpu_task`?

**A:** Not directly — `init_cpu_task` is a CPU-only kernel primitive. However:

**Indirect implications for GPU driver engineers:**
1. Per-CPU data (`tpidr_el1`) is used in interrupt handlers that service GPU
   interrupts. If `init_cpu_task` wasn't run on every CPU brought online, GPU
   interrupt handlers would malfunction when IRQs are processed on that CPU.

2. When CPUs are hotplugged (CPU bringup/teardown), the same `init_cpu_task` setup
   runs on newly brought-online CPUs. GPU driver workers bound to specific CPUs
   depend on this.

3. Shadow Call Stack (SCS) protects GPU driver kernel module code too. If SCS
   wasn't initialized properly, a ROP attack via a GPU command buffer exploit
   could bypass SCS protection.

---

## Q5: How does `init_cpu_task` relate to `cpu_init()`?

**A:** These are different and complementary:

| | `init_cpu_task` (macro) | `cpu_init()` (C function) |
|---|---|---|
| **Language** | Assembly | C |
| **When** | Very first thing in `__primary_switched` | Called from `start_kernel` |
| **Purpose** | Establish minimum runtime state | Initialize CPU subsystems (IRQ stacks, debug, ...) |
| **What it sets up** | sp_el0, sp, x29, tpidr_el1, x18 | IRQ/NMI/overflow stacks, VBAR for secondary vectors, CPU features |
| **Can use `current`?** | No — sets it up | Yes — `current` works |
| **Can use `this_cpu_read`?** | No — sets it up | Yes — tpidr_el1 is set |

`init_cpu_task` creates the FOUNDATION; `cpu_init()` builds on it.

---

## Q6: What is the relationship between `init_cpu_task` and `KASAN`?

**A:** `init_cpu_task` runs BEFORE KASAN is initialized. This creates a window
where KASAN shadow memory is not set up. The implication:

- The `ldr` and `str` instructions inside `init_cpu_task` are NOT KASAN-instrumented
  (assembly code is never instrumented — only C code is)
- The first KASAN-instrumented C code runs only after `kasan_early_init()` in
  `__primary_switched` (several lines AFTER `init_cpu_task`)
- The `init_stack` memory touched by `init_cpu_task` (writing the frame sentinel)
  will eventually be covered by KASAN shadow memory, but not yet at this point

This is safe because KASAN instrumentation is only added to C code by the compiler.
The assembly macro bypasses KASAN instrumentation entirely.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the current task (process) is identified at EL0 via TPIDR_EL0 (user thread ID) and at EL1 via SP_EL0. Linux uses SP_EL0 to store the pointer to the current task_struct. SP_EL0 is a dedicated register (not the EL0 stack pointer when running in EL1 -- at EL1, SP_ELx selects either SP_EL0 or SP_EL1 as the active stack, controlled by PSTATE.SP). When the kernel uses SP_EL0 to store the current task pointer, it is using SP_EL0 as a general-purpose register (reading/writing it with MRS/MSR SP_EL0).

### Kernel Perspective (Linux ARM64)
init_cpu_task is a per-CPU variable (or boot-time initialization) that sets up the idle task (init_task / swapper) as the current task. In __primary_switched:
  msr  sp_el0, x23        // x23 holds init_task VA, set SP_EL0 = &init_task
  ldr  x8, [x23, #TSK_TI_CPU]  // verify .cpu field
The current macro in Linux ARM64 expands to:
  mrs x0, sp_el0          // read SP_EL0 as current task_struct pointer
SP_EL0 is never spilled to the stack (it is a system register), making current() essentially a zero-cost operation.

### Memory Perspective (ARMv8 Memory Model)
task_struct for init_task lives in the .data section of the kernel image (statically allocated). Its VA is in the kernel text/data mapping (TTBR1_EL1). When SP_EL0 is set to &init_task, the memory region is already mapped and accessible. The task's stack (thread_union) is in the .init.data section and is also already mapped. After start_kernel -> sched_init(), all subsequent tasks have their task_struct allocated from slab memory in the kernel heap (also in the TTBR1_EL1 region).