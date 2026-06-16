# Per-CPU Offset — Interview Q&A

---

## Q1: Explain `tpidr_el1` and how it enables per-CPU data access in one sentence.

**A:** `tpidr_el1` holds the byte offset from the per-CPU section prototype to this
CPU's private copy, so any per-CPU variable address is computed as
`tpidr_el1 + link_time_offset` — one register read plus one load, with zero false sharing.

---

## Q2: Why does `ldr w\tmp2, [\tsk, #TSK_TI_CPU]` use `w` (32-bit) not `x` (64-bit)?

**A:** `thread_info.cpu` is defined as `u32` (32 bits):
```c
struct thread_info {
    ...
    u32 cpu;    // 32-bit CPU ID
    ...
};
```
Using `ldr w6` performs a 32-bit load AND zero-extends the result to the full 64-bit
`x6` register. Then `x6, lsl #3` correctly computes `cpu * 8` as a 64-bit byte offset
into the `__per_cpu_offset` array. Using `ldr x6` would attempt a 64-bit load
potentially spanning a field boundary.

---

## Q3: What is `set_this_cpu_offset` and why is it a macro instead of a function?

**A:**
```asm
.macro set_this_cpu_offset, val
    msr tpidr_el1, \val
.endm
```
It's a macro because:
1. **Inlining guarantee:** A macro always inlines — no function call overhead. Writing
   to `tpidr_el1` is a one-instruction operation; adding `bl` overhead would be wasteful.
2. **Early boot context:** At the point `init_cpu_task` runs, there is no valid stack
   yet for a function call. The macro runs entirely within the inline code stream.
3. **VHE portability:** The macro can also expand to `msr tpidr_el2` under VHE without
   the caller needing to know. A function would need a runtime branch; a macro resolves
   this at compile time.

---

## Q4: On CPU hotplug (CPU1 coming online), does `init_cpu_task` run on CPU1?

**A:** Yes. From `head.S`:
```asm
SYM_FUNC_START_LOCAL(__secondary_switched)
    ...
    adr_l   x0, secondary_data
    ldr     x2, [x0, #CPU_BOOT_TASK]    // x2 = idle task for this CPU
    cbz     x2, __secondary_too_slow
    init_cpu_task x2, x1, x3           // SAME MACRO, different task
    ...
    bl      secondary_start_kernel
```

CPU1's `init_cpu_task` uses CPU1's idle task. It reads:
- `idle_task_for_cpu1.stack` → CPU1's own stack
- `idle_task_for_cpu1.thread_info.cpu` → 1 (not 0)
- `__per_cpu_offset[1]` → CPU1's per-CPU section base

CPU1's `tpidr_el1` is set to `__per_cpu_offset[1]`. From that point, any
`this_cpu_read()` on CPU1 accesses CPU1's private per-CPU data.

---

## Q5: Can you have more than `NR_CPUS` per-CPU variables?

**A:** The question conflates two things:

- **Number of per-CPU VARIABLES:** Unlimited — you can have thousands of
  `DEFINE_PER_CPU` variables. Each adds to the size of the per-CPU section.
  
- **`NR_CPUS`:** This is the MAXIMUM number of CPUs, not the number of variables.
  `__per_cpu_offset[NR_CPUS]` is sized for `NR_CPUS` entries.

The per-CPU SECTION SIZE limits how many variables you can have in total.
If the per-CPU section exceeds the allocated region size, the kernel panics at
`setup_per_cpu_areas()` with a size overflow.

Typical per-CPU section size: 64KB–1MB depending on kernel configuration.

---

## Q6: Why is `__per_cpu_offset` read via `adr_l` (PC-relative) not a global pointer?

**A:** `adr_l` expands to `adrp + add` — a PC-relative address computation. This
is preferred over loading from a global pointer for two reasons:

1. **One instruction to get the address, one to load:** With `adr_l`, computing
   `&__per_cpu_offset` costs 2 instructions (adrp+add). Dereferencing a global
   pointer to get the address would cost 2 instructions + 1 extra load (pointer
   dereference) = 3 instructions.

2. **KASLR compatibility:** `adr_l` computes the address relative to the current PC,
   which is already at the correct KASLR-randomized virtual address. A stored
   pointer would need to be fix-up'd at runtime (which it is — `__per_cpu_offset`
   is in `.data`, not a pointer to `.data`). The `adr_l` approach directly computes
   the VA — KASLR-correct by construction.

---

## Q7: What is `tpidr_el2` and when is it used instead of `tpidr_el1`?

**A:** Under **VHE (Virtualization Host Extensions)**, the Linux kernel runs at EL2
instead of EL1. In this case:
- `tpidr_el1` is inaccessible (or used by the hypervisor)
- `tpidr_el2` is the equivalent per-CPU register for EL2

The `set_this_cpu_offset` macro handles this transparently:
```asm
.macro set_this_cpu_offset, val
#ifdef CONFIG_ARM64_VHE
    // Runtime check or compile-time VHE flag
    msr tpidr_el2, \val
#else
    msr tpidr_el1, \val
#endif
.endm
```

Similarly, `get_this_cpu_offset` reads from the correct register based on
whether VHE is active. The rest of the kernel is completely unaware of
which register is used — the abstraction is perfect.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
TPIDR_EL1 (Thread ID Register for EL1) is a 64-bit system register available at EL1 for software use. ARMv8-A defines it as "EL1 software thread ID register" -- it holds an arbitrary 64-bit value chosen by the OS. On SMP Linux, each CPU stores the address of its per-CPU offset (or a pointer to the CPU's data) in TPIDR_EL1. Reading TPIDR_EL1 is a single MRS instruction with no memory access, making it the fastest way to identify which CPU is executing.

### Kernel Perspective (Linux ARM64)
Linux uses TPIDR_EL1 to point to the per-CPU offset table. The THIS_CPU_READ/WRITE macros use:
  mrs x0, tpidr_el1     // load per-CPU base
  ldr x0, [x0, #offset] // load per-CPU variable
In __primary_switched, TPIDR_EL1 is initialized to the boot CPU's per-CPU offset via msr tpidr_el1, x27 (or equivalent). Per-CPU variables are declared with DEFINE_PER_CPU and accessed through get_cpu_var()/put_cpu_var(). The CPU does not need to disable preemption to read its own TPIDR_EL1 because the register is private to each CPU.

### Memory Perspective (ARMv8 Memory Model)
Per-CPU data is physically spread across NUMA nodes or simply across L1 cache regions. By using per-CPU data, Linux avoids cache line bouncing: each CPU has its own copy of the data in its own L1 D-cache, with no sharing or coherency traffic between CPUs. TPIDR_EL1 itself lives in the system register file (not RAM), so reading it has zero memory latency. The pointed-to per-CPU memory is Normal Inner-Shareable, but because each CPU only reads its own slice, there is no actual sharing -- the inner-shareable attribute just means the memory type is compatible with the cache coherency domain.