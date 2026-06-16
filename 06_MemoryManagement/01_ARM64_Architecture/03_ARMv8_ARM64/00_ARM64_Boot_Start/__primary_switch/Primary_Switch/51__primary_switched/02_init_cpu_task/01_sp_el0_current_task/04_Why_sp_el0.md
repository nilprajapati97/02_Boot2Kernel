# Why `sp_el0` Specifically — Architectural Analysis

## All Available "Free" Registers at Boot

At the time `__primary_switched` runs, the CPU has just switched to virtual
addresses. What registers are available to Linux for repurposing?

| Register | Architectural Role at EL1 | Linux Use | Available for `current`? |
|---|---|---|---|
| `sp` (SP_EL1) | EL1 stack pointer | Kernel stack | NO — must be stack |
| `sp_el0` | EL0 stack pointer (not used at EL1) | **Linux: current task** | ✓ YES |
| `tpidr_el0` | User thread ID (used by EL0) | User TLS | NO — user-space uses it |
| `tpidr_el1` | EL1 thread ID | **Linux: per-CPU offset** | NO — already used |
| `tpidr_el2` | EL2 thread ID | VHE per-CPU | NO — used for VHE |
| `x18` | Caller-saved GPR | **Linux: SCS pointer** | NO — used for SCS |
| `x19`-`x28` | Callee-saved GPRs | Normal usage | NO — used by C code |
| `x29` | Frame pointer | Frame chains | NO — needed for unwinder |
| `x30` | Link register | Return address | NO — needed for calls |

`sp_el0` is the only register that:
1. Has NO architectural role at EL1 (it's completely idle)
2. Is NOT used by any user-space ABI expectation
3. Is hardware-banked per-CPU (each CPU has its own physical register)
4. Can be read in one instruction (`mrs`)

---

## The Banking Mechanism

ARM64 exception levels have **banked** (private) registers:

```
         EL0          EL1          EL2          EL3
         ────         ────         ────         ────
SP:    SP_EL0       SP_EL1       SP_EL2       SP_EL3
SPsr:  SPSR_EL0    SPSR_EL1    SPSR_EL2    SPSR_EL3
ELR:   ELR_EL0     ELR_EL1     ELR_EL2     ELR_EL3
TPIDR: TPIDR_EL0   TPIDR_EL1   TPIDR_EL2   TPIDR_EL3
```

When at EL1 (kernel mode):
- `SP_EL1` is the stack pointer (aliased as `sp`)
- `SP_EL0` is accessible via `mrs/msr` but NOT automatically used
- Reading `sp_el0` from EL1 reads the USER's stack pointer value
- But Linux overwrites it with `&current_task` at every kernel entry

**Context switches:**
When the OS switches from user to kernel: `sp_el0` retains the user's stack pointer.
Linux saves this in `pt_regs.sp` and writes `&current_task` over it:
```asm
// kernel_entry macro in entry.S:
mrs     x21, sp_el0     // save user's sp_el0 → x21 (then stored in pt_regs)
msr     sp_el0, tsk     // overwrite with current task pointer
```

On kernel exit:
```asm
// kernel_exit macro in entry.S:
ldr     x21, [sp, #S_SP]   // restore user sp from pt_regs
msr     sp_el0, x21        // restore EL0 sp (user stack pointer)
```

---

## CPU Register Physical Organization

On a Cortex-A55 or Cortex-A78 CPU die:

```
Physical register file:
┌─────────────────────────────────────────────────────────┐
│  x0-x30  (31 × 64-bit registers, shared across ELs)    │
├─────────────────────────────────────────────────────────┤
│  SP_EL0  (private to EL0 context)                       │
│  SP_EL1  (private to EL1 context)   ← kernel SP        │
│  SP_EL2  (private to EL2 context)                       │
├─────────────────────────────────────────────────────────┤
│  TPIDR_EL0, TPIDR_EL1, TPIDR_EL2                       │
│  VBAR_EL1, VBAR_EL2                                     │
│  ... other system registers ...                         │
└─────────────────────────────────────────────────────────┘
```

Each physical core has its own copy of ALL of these. There is NO sharing of
physical register state between CPUs (cores). This is what makes `sp_el0` suitable
as a per-CPU "current" pointer.

---

## Alternative Designs (And Why They're Worse)

### Alternative 1: Global `current_task[NR_CPUS]` array
```c
extern struct task_struct *current_task[NR_CPUS];
#define current current_task[smp_processor_id()]
```
Problems:
- `smp_processor_id()` itself uses `tpidr_el1` (per-CPU data)
- Requires memory access (cache miss possible)
- Needs two loads: CPU ID + array dereference
- Creates a circular dependency (need CPU ID to find current, need current...)

### Alternative 2: Per-CPU variable for current task
```c
DEFINE_PER_CPU(struct task_struct *, current_task_ptr);
#define current this_cpu_read(current_task_ptr)
```
Problems:
- `this_cpu_read` uses `tpidr_el1` — per-CPU offset
- Still needs `tpidr_el1` to work first
- Two register reads + one memory access vs. one register read

### Alternative 3: x27 or x28 reserved for `current`
```c
// Hypothetical: always keep current in x27
register struct task_struct *current asm("x27");
```
Problems:
- GCC/Clang would need to NEVER allocate x27 for any other variable
- Doubles the pressure on other general-purpose registers (x0-x26)
- Every function call must save/restore x27 (callee-saved)
- Context switches must explicitly update x27 in every function... impossible

The `sp_el0` approach is provably optimal.

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