# ARM32 `set_my_cpu_offset()` — TPIDRPRW Register Deep Dive

## Source Reference
- `arch/arm/include/asm/percpu.h:17` — `set_my_cpu_offset()`
- `arch/arm/include/asm/percpu.h:27` — `__my_cpu_offset()`
- `arch/arm/kernel/smp.c:500` — called from `smp_prepare_boot_cpu()`
- `arch/arm/kernel/smp.c:410` — called from `secondary_start_kernel()`

---

## The TPIDRPRW Register

**Full name:** Thread and Process ID Register, Privileged Read/Write

**CP15 encoding:**
```
MRC p15, 0, <Rd>, c13, c0, 4   ; Read TPIDRPRW
MCR p15, 0, <Rn>, c13, c0, 4   ; Write TPIDRPRW
```

| Field | Value |
|---|---|
| Coprocessor | p15 |
| Opcode1 | 0 |
| CRn (register) | c13 |
| CRm (sub-register) | c0 |
| Opcode2 | 4 |
| Exception level | PL1 (Privileged) only |
| Banked per-core? | Yes |

### Register Family (CP15 c13)

ARM CP15 c13 contains the Thread ID registers:
```
c13,c0,0: FCSEIDR  — FCSE Process ID Register (deprecated in ARMv7+)
c13,c0,1: CONTEXTIDR — Context ID Register (ASID + PROCID)
c13,c0,2: TPIDRURW — Thread ID, User R/W (user-space thread pointer)
c13,c0,3: TPIDRURO — Thread ID, User R/O (user-space, kernel-write only)
c13,c0,4: TPIDRPRW — Thread ID, Privileged R/W  ← USED FOR PER-CPU OFFSET
```

**Why TPIDRPRW and not TPIDRURW or TPIDRURO?**
- TPIDRURW is readable and writable from user space → security risk
- TPIDRURO is readable from user space (used by glibc for TLS pointer)
- TPIDRPRW is only accessible from kernel mode (PL1) → safe for kernel use

---

## `set_my_cpu_offset()` — Writing TPIDRPRW

```c
/* arch/arm/include/asm/percpu.h:17 */
static inline void set_my_cpu_offset(unsigned long off)
{
    /* MCR instruction: Write 'off' to TPIDRPRW */
    asm volatile("mcr p15, 0, %0, c13, c0, 4" :: "r" (off) : "memory");
    /*
     * "memory" clobber: tells compiler that this write may affect all
     * memory — prevents reordering of adjacent memory operations
     * with respect to the register write.
     */
}
```

### Instruction Encoding

```
MCR p15, 0, Rn, c13, c0, 4
│    │   │   │    │   │   │
│    │   │   │    │   │   └── opc2 = 4 → selects TPIDRPRW
│    │   │   │    │   └────── CRm = c0
│    │   │   │    └────────── CRn = c13 (thread ID registers)
│    │   │   └─────────────── Rn = source register (holds offset value)
│    │   └─────────────────── opc1 = 0
│    └─────────────────────── target coprocessor = CP15
└──────────────────────────── Move to CoProcessor Register instruction
```

Machine encoding (Thumb-2, 32-bit instruction):
```
1110 1110 0 001 1101 Rn00 1111 0 10 1 Rm00   (approximately)
```

### When Called

| Caller | File | Line | CPU | Value Written |
|---|---|---|---|---|
| `smp_prepare_boot_cpu()` | `arch/arm/kernel/smp.c` | 500 | CPU 0 | `__per_cpu_offset[0]` |
| `secondary_start_kernel()` | `arch/arm/kernel/smp.c` | 410 | CPU N | `__per_cpu_offset[N]` |

---

## `__my_cpu_offset` — Reading TPIDRPRW

```c
/* arch/arm/include/asm/percpu.h:27 */
#ifdef CONFIG_SMP
static inline unsigned long __my_cpu_offset(void)
{
    unsigned long off;
    /*
     * Read TPIDRPRW into 'off'.
     *
     * "Q" constraint: a memory operand with base register and no offset.
     * Applied to a NULL pointer dereference — this is a FAKE dependency.
     * Purpose: prevent compiler from hoisting this mrc above stores
     * that it precedes in the source. Without this, the compiler's
     * memory model could allow the read to appear before stores
     * that logically precede it.
     */
    asm("mrc p15, 0, %0, c13, c0, 4"
        : "=r" (off)
        : "Q" (*(unsigned long *)NULL)
    );
    return off;
}
#define __my_cpu_offset  __my_cpu_offset()
#else
/* UP: no SMP, offset is always 0 */
#define __my_cpu_offset  0UL
#endif
```

### The "Q" Constraint Explained

```
"Q" in ARM GCC = memory operand with base register (no additional offset)

"Q" (*(unsigned long *)NULL)
   means: "read from address NULL" (which would fault at runtime)
   BUT: the compiler sees this as a memory input dependency

Why this works:
  - Compiler cannot move the asm block BEFORE any instruction that
    might write to the memory the "Q" operand "depends on"
  - Since NULL could be any stack location (from compiler's perspective),
    it conservatively assumes ALL stack writes must complete first
  - This prevents the mrc from being scheduled before the stores that
    set up the per-CPU area being accessed

Real-world scenario where this matters:
  store(value_to_per_cpu_area)
  x = this_cpu_read(var)    ← needs updated TPIDRPRW perspective
  
  Without "Q": compiler might execute mrc BEFORE the store
  With "Q":    compiler knows mrc depends on memory state → no hoisting
```

---

## UP (Uniprocessor) Patching — `.alt.smp.init`

For kernels built with `CONFIG_SMP=y` but running on single-core hardware:

### The Problem
A `CONFIG_SMP` kernel has:
```c
asm("mrc p15, 0, %0, c13, c0, 4" ...)  ← reads TPIDRPRW
```
But on UP hardware, TPIDRPRW may not be written (saves a `mcr` at boot) and
the value should always be 0.

### The Solution: `.alt.smp.init` Patching

```c
/* arch/arm/include/asm/percpu.h — ARMv6 UP path */
static inline unsigned long __my_cpu_offset(void)
{
    unsigned long off;
    asm(
        "1: mrc p15, 0, %0, c13, c0, 4\n"
        "   .pushsection \".alt.smp.init\", \"a\"\n"
        "   .long 1b\n"                    /* address of instruction to patch */
        "   ldr %0, [pc]\n"                /* replacement: load 0 from pc+offset */
        "   .long 0\n"                     /* the immediate value 0 */
        "   .popsection\n"
        : "=r" (off)
        : "Q" (*(unsigned long *)NULL)
    );
    return off;
}
```

At boot time, if running on UP (`nr_cpu_ids == 1`):
```c
/* arch/arm/kernel/smp.c: fixup_smp() */
/* Scans .alt.smp.init section and patches each listed instruction */
/* mrc p15,0,Rd,c13,c0,4  →  mov Rd, #0  (or similar zero-returning instr) */
```

**Result on UP hardware:**
```asm
; After patching:
mov   r0, #0     ; always returns 0 (patched from mrc)
; 1 instruction, no CP15 access needed on UP
```

---

## Context Switch: Does TPIDRPRW Need Saving?

**No.** TPIDRPRW is a **per-core** register:
- Each physical CPU core has its own private TPIDRPRW
- When CPU 0 reads TPIDRPRW, it always gets CPU 0's offset
- When CPU 3 reads TPIDRPRW, it always gets CPU 3's offset
- **No context switch code** needs to save/restore TPIDRPRW
- TPIDRPRW is written once at CPU bring-up and never changes thereafter

Compare with:
- TPIDRURO: used for **task** TLS pointer → must be saved/restored on task switch
- TPIDRPRW: used for **CPU** per-CPU offset → written once per CPU, never changes

---

## Cortex-A Series Implementation Notes

| Core | TPIDRPRW behavior |
|---|---|
| Cortex-A5 | Supported (ARMv7-A) |
| Cortex-A7 | Supported; stays valid across `wfi` (Wait For Interrupt) |
| Cortex-A9 | Supported; survives all power states except full core power-down |
| Cortex-A15 | Supported; EL1 access only (PL1) |

On cores that support CPU power management (CPUIDLE / CPU hotplug), TPIDRPRW
must be re-written after a full-core power-down:
```c
/* arch/arm/kernel/sleep.S or similar */
/* After CPU wakeup from deep sleep: */
cpu_resume: → secondary_start_kernel() → set_my_cpu_offset()
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Full register name? | Thread and Process ID Register, Privileged R/W |
| CP15 encoding? | `p15, 0, Rx, c13, c0, 4` |
| Which privilege level can access? | PL1 (kernel mode) only |
| Is it banked per core? | Yes — each core has its own TPIDRPRW |
| Instruction to write? | `mcr p15, 0, Rn, c13, c0, 4` |
| Instruction to read? | `mrc p15, 0, Rd, c13, c0, 4` |
| Why "Q" constraint in read? | Prevents compiler from hoisting mrc above memory writes |
| What is `.alt.smp.init`? | UP patching: replaces mrc with zero-return on single-core systems |
| Must TPIDRPRW be saved on context switch? | No — per-core, not per-task |
| When is it written? | Once at CPU bring-up (smp_prepare_boot_cpu / secondary_start_kernel) |
| What value is written? | `__per_cpu_offset[cpu]` |
