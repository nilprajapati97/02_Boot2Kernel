# `scs_load_current` Macro — Detailed Analysis

## Macro Source

```asm
// arch/arm64/include/asm/scs.h

.macro scs_load_current
#ifdef CONFIG_SHADOW_CALL_STACK
    get_current_task    x18
    ldr     x18, [x18, #TSK_TI_SCS_SP]
#endif
.endm

.macro get_current_task, tsk
    mrs     \tsk, sp_el0
.endm
```

When `CONFIG_SHADOW_CALL_STACK` is NOT set, the macro expands to NOTHING.
Zero instructions, zero overhead.

When enabled, it expands to:
```asm
mrs     x18, sp_el0              // x18 = &current_task (set in OP1)
ldr     x18, [x18, #TSK_TI_SCS_SP]  // x18 = current_task.thread_info.scs_sp
```

---

## Why Two Instructions and Not One?

The macro needs two operations:
1. GET the current task pointer (stored in `sp_el0`)
2. LOAD the SCS pointer from the task struct (a MEMORY READ)

These cannot be combined into one instruction because:
- `mrs sp_el0, x18` gives a register value (the task address)
- `ldr x18, [x18, #TSK_TI_SCS_SP]` gives the SCS pointer from that task's memory

ARM64 has no "chain load" instruction. Two instructions are the minimum.

---

## Why Does `scs_load_current` Exist — Why Not Just Use a Per-CPU Variable?

Option A: `x18 = per_cpu(scs_sp, cpu)` 
- Needs `tpidr_el1` (set in OP5 — AFTER `scs_load_current` in OP4)
- Would require doing OP5 before OP4 — but then OP4 uses `sp_el0` (set in OP1), 
  while OP5 uses `sp_el0` too via `TSK_TI_CPU` — OK, both can use OP1's result
- Actually this could work... but the Linux design chose: SCS pointer lives in
  `task_struct`, not in per-CPU data. This way it moves with the task (task can
  migrate between CPUs), and `cpu_switch_to` can save/restore it uniformly.

Option B: Hard-code `x18 = init_shadow_call_stack` at boot:
- This would only work for `init_task` — every other task has a different SCS buffer
- Would need to special-case boot code → maintenance burden

The `scs_load_current` approach is uniform: same macro works for ANY task, whether
it's `init_task` or a task on CPU 47.

---

## Ordering Constraint: Why OP4 Must Come After OP1

```
OP1: msr sp_el0, tsk        → sp_el0 = &init_task
OP4: mrs x18, sp_el0        → x18 = sp_el0 = &init_task (reads OP1's result)
     ldr x18, [x18, #SCS]   → x18 = init_task.thread_info.scs_sp
```

If OP4 ran BEFORE OP1:
- `mrs x18, sp_el0` would read the OLD `sp_el0` (firmware garbage or 0)
- `ldr x18, [x18, #SCS]` would dereference garbage address
- Translation fault → crash

The ordering OP1 → OP4 is enforced by `init_cpu_task`'s instruction sequence.
The `mrs` in OP4 has a data dependency on the `msr` in OP1 via the `sp_el0` register.
The CPU pipeline will stall if needed to ensure OP1 is visible before OP4 reads.

Actually: there's no explicit barrier between OP1 and OP4. The ARM architecture
guarantees that a `msr` followed by `mrs` (on the same register path, same PE)
will see the new value. This is within the same instruction stream on the same CPU.

---

## `TSK_TI_SCS_SP` — The Offset Value

```c
// asm-offsets.c:
DEFINE(TSK_TI_SCS_SP,
       offsetof(struct task_struct, thread_info.scs_sp));
```

Example layout:
```
struct task_struct {
    struct thread_info thread_info {   // offset 0
        u64 flags;        // +0
        u64 ttbr0;        // +8
        u64 preempt;      // +16
        u32 cpu;          // +24
        u32 _pad;         // +28
        u64 ttbr0_more;   // +32 (may vary by config)
        void *scs_base;   // +40
        void *scs_sp;     // +48   ← TSK_TI_SCS_SP = 48
    };
    // ... rest of task_struct ...
    void *stack;          // offset ~24 from task_struct start ← TSK_STACK
};
```

The exact value depends on the kernel version and configuration. It's always
generated correctly by `asm-offsets.c`.

---

## SCS Overflow Detection

The SCS buffer is bounded. A deeply recursive kernel function could overflow it:

```c
// arch/arm64/include/asm/scs.h
#define SCS_SIZE   (PAGE_SIZE)  // 4096 bytes = 512 return addresses
```

The kernel checks for SCS overflow in some configurations:
```c
static __always_inline void scs_check_stack(void)
{
    unsigned long *p = (unsigned long *)current->thread_info.scs_sp;
    if (unlikely(p >= current->thread_info.scs_base + SCS_SIZE))
        scs_corrupted();  // overflow!
}
```

512 return addresses is enough for any normal kernel call chain. ROP chains
typically require far fewer (3-10 gadgets). SCS overflow is not a practical concern
for legitimate kernel code.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the stack pointer is a dedicated register (SP_EL1 at EL1, SP_EL0 at EL0). SP_EL1 is the stack pointer used by the kernel during normal execution. The AAPCS64 ABI requires the stack to be 16-byte aligned at any instruction that may cause an exception. SCTLR_EL1.SA (bit 3) enables hardware enforcement of this alignment: if SP_EL1 is not 16-byte aligned when a load/store using SP is executed, an SP alignment fault is raised. The frame pointer (x29) is a general-purpose register used by convention to hold the base of the current stack frame. Writing x29 is the first act of any C function that wishes to be unwound.

### Kernel Perspective (Linux ARM64)
After the MMU is enabled, __primary_switch reinitializes the stack pointer to a virtual address. The early boot stack is defined as:
  __INIT_DATA: init_thread_union (size THREAD_SIZE, typically 16 KB)
The LDR instruction loads the VA of init_thread_union + THREAD_SIZE into x0, then MOV sp, x0 sets SP_EL1. This is necessary because the old stack pointer was set to a physical address (before the MMU) and that PA is no longer the correct address for the kernel VA layout. x29 is set to zero (zero frame pointer) to terminate the unwind chain at the first kernel stack frame.

### Memory Perspective (ARMv8 Memory Model)
The kernel stack resides in Normal Inner-Shareable Write-Back Cacheable memory (MT_NORMAL). Once the MMU and D-cache are enabled, all stack accesses (PUSH/POP equivalents: STP/LDP) go through the L1 D-cache. The L1 D-cache write-back policy means that the stack contents are not immediately visible to physical memory until a cache clean or eviction. This is safe for the stack because the kernel does not use DMA to read stack memory. The stack pointer reinitalization at VA is a hard cut: all future kernel stack frames exist in the high VA kernel mapping.