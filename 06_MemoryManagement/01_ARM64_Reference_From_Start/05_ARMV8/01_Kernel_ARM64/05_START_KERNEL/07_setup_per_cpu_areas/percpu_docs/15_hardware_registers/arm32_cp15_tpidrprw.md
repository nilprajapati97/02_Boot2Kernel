# ARM32 CP15 Register: TPIDRPRW (Thread Pointer/ID Register)

## Register Identity

| Property | Value |
|---|---|
| Full name | Thread Pointer / ID Register, Privileged Read/Write |
| CP15 encoding | `p15, 0, Rn, c13, c0, 4` |
| Architecture | ARMv6 and later (ARMv7-A, ARMv7-R) |
| Access level | PL1 (Privileged Level 1 — kernel mode only) |
| User access | **No** — user space cannot read or write TPIDRPRW |

---

## CP15 c13 Register Family (Thread ID Registers)

The CP15 coprocessor register `c13` contains the entire Thread ID Register bank:

| Register | Encoding | Name | Access | Kernel Use |
|---|---|---|---|---|
| FCSEIDR | `p15,0,Rn,c13,c0,0` | FCSE Process ID Register | PL1 R/W | FCSE (deprecated in ARMv7) |
| CONTEXTIDR | `p15,0,Rn,c13,c0,1` | Context ID Register | PL1 R/W | ASID (Address Space ID) |
| TPIDRURW | `p15,0,Rn,c13,c0,2` | User Thread ID, User R/W | PL0+PL1 R/W | User-space TLS (`__set_tls`) |
| TPIDRURO | `p15,0,Rn,c13,c0,3` | User Thread ID, User R-only | PL0 R / PL1 R/W | User TLS read-only alias |
| **TPIDRPRW** | **`p15,0,Rn,c13,c0,4`** | **Privileged Thread ID R/W** | **PL1 R/W only** | **Per-CPU offset** |

---

## Write Instruction: `MCR`

```asm
; Write Rn to TPIDRPRW:
mcr   p15, 0, Rn, c13, c0, 4

; Field breakdown:
;   p15     = coprocessor 15 (system control)
;   0       = opcode1 (must be 0 for Thread ID registers)
;   Rn      = ARM register holding value to write
;   c13     = CRn (coprocessor register number)
;   c0      = CRm (secondary coprocessor register)
;   4       = opcode2 (selects TPIDRPRW among c13 registers)

; Example: write 0x80300000 to TPIDRPRW on CPU 0:
ldr   r0, =0x80300000
mcr   p15, 0, r0, c13, c0, 4
```

---

## Read Instruction: `MRC`

```asm
; Read TPIDRPRW into Rd:
mrc   p15, 0, Rd, c13, c0, 4

; Example: read TPIDRPRW and load from a per-CPU variable:
mrc   p15, 0, r2, c13, c0, 4     ; r2 = per-CPU offset
ldr   r0, [r2, #<variable_offset>]  ; r0 = CPU-private variable
```

---

## ARM32 Per-CPU Encoding (from `arch/arm/include/asm/percpu.h`)

```c
/* arch/arm/include/asm/percpu.h:17 */
static inline void set_my_cpu_offset(unsigned long off)
{
    /* write TPIDRPRW */
    asm volatile("mcr p15, 0, %0, c13, c0, 4" :: "r" (off) : "memory");
}

/* arch/arm/include/asm/percpu.h:27 */
#define __my_cpu_offset                             \
({                                                  \
    unsigned long off;                              \
    asm volatile(                                   \
        ALT_SMP("mrc p15, 0, %0, c13, c0, 4",      \
                 "mov %0, #0")                      \
        : "=r" (off)                                \
        : "Q" (*(unsigned long *)NULL)              \  /* "Q" hazard */
    );                                              \
    off;                                            \
})
```

---

## The `ALT_SMP` Mechanism (ARM32 UP Patching)

The `ALT_SMP(smp_instr, up_instr)` macro is ARM32's analog of ARM64's `ALTERNATIVE()`.
It enables the same kernel binary to run on both single-processor (UP) and multi-processor
(SMP) systems by patching instructions at boot time.

### How It Works

```c
/* arch/arm/include/asm/alternative.h (conceptual): */
#define ALT_SMP(smp, up)                                                \
    ".pushsection \".alt.smp.init\", \"a\"\n"                          \
    ".long   661f\n"          /* address of original instruction */     \
    ".popsection\n"                                                     \
    "661:\n"                  /* label marks the SMP instruction */     \
    smp "\n"                  /* original (SMP) instruction */          \
    "662:\n"                                                            \
```

The `.alt.smp.init` section contains a table of instruction addresses.
Each entry: `(address, UP replacement instruction)`.

### Boot-Time Patching Flow

```
ARM32 SMP_ON_UP boot:
  fixup_smp()   (arch/arm/kernel/smp_on_up.c)
    ↓
  Scans .alt.smp.init table
    ↓
  For each entry, if running as UP:
    Overwrites SMP instruction with UP replacement
    (e.g., "mrc p15,0,r0,c13,c0,4" → "mov r0, #0")
    ↓
  flush_icache_range() for patched region
    ↓
  ISB (Instruction Synchronization Barrier)
```

### Result on UP Systems

```asm
; On a single-core (UP) system, after patching:
mov   r2, #0      ; offset is always 0 (patched from "mrc p15,0,r2,c13,c0,4")
ldr   r0, [r2, #<offset>]   ; reads directly from template (offset 0 from __per_cpu_start)
```

This is correct because on UP systems, `__per_cpu_offset[0] = 0` (the single CPU's
data IS at the template location).

---

## The "Q" Constraint Explained

```c
asm volatile(...
    : "=r" (off)
    : "Q" (*(unsigned long *)NULL)   /* ← this is the hazard marker */
);
```

`"Q"` is an ARM32 constraint meaning "memory address in a register" (Q = memory
operand). The `(*(unsigned long *)NULL)` is never actually dereferenced — it's a
syntactic device. The constraint forces the compiler to treat this asm block as if
it reads from memory, preventing two dangerous optimizations:

1. **Hoisting:** Moving the `mrc` instruction above a preceding memory store
   (e.g., above `preempt_disable()`'s store to the preemption counter)

2. **Sinking:** Moving the `mrc` below a subsequent memory load that should
   see the per-CPU offset computed from the "current" CPU

Without "Q", the compiler's alias analysis might reorder the `mrc` freely, since
the compiler cannot see that `mrc` reads a hardware register that is logically
dependent on the CPU-specific memory state.

---

## Privilege Level: Why User Space Cannot Access TPIDRPRW

```
ARM32 Privilege Levels:
  PL0 = User mode (applications)
  PL1 = Privileged mode (kernel, IRQ, FIQ, Supervisor, Abort, Undefined, System)
  PL2 = Hyp mode (Virtualization, ARMv7-A only)

TPIDRPRW is "Privileged Read/Write":
  - PL1 read: YES (MRC in kernel mode works)
  - PL1 write: YES (MCR in kernel mode works)
  - PL0 read: UNDEFINED INSTRUCTION → fault
  - PL0 write: UNDEFINED INSTRUCTION → fault

Compare with TPIDRURW:
  - User-space TLS: PL0 read = YES, PL0 write = YES, PL1 R/W = YES
  - Used by glibc for user-space thread-local storage
```

---

## Power State and Context Switching

### No Context Switch Required

```
Unlike general-purpose registers (r0-r15):
  - TPIDRPRW is NOT saved/restored in the thread context switch path
  - This is by design: each CPU has exactly one TPIDRPRW
  - The register stores the CPU's per-CPU offset, NOT a thread's offset
  - All tasks on the same CPU share the same per-CPU offset

Context switch path (arch/arm/kernel/entry-armv.S):
  __switch_to:
    stmia r4, {r4 - sl, fp, sp, lr}   ; save task registers
    ldmia ip, {r4 - sl, fp, sp, pc}   ; restore next task registers
    ; NOTE: TPIDRPRW is not in this list — intentionally
```

### Power Down / CPU Offline

```
When CPU is powered down (hotplug or deep idle):
  TPIDRPRW register content is LOST (hardware reset)

When CPU comes back online:
  secondary_start_kernel() is called
  set_my_cpu_offset() re-writes TPIDRPRW
  Normal operation resumes
```

---

## Cortex-A Series Implementation Notes

| CPU | Architecture | Notes |
|---|---|---|
| Cortex-A5 | ARMv7-A | Full CP15 c13 support including TPIDRPRW |
| Cortex-A7 | ARMv7-A | TPIDRPRW supported; used in low-power designs |
| Cortex-A8 | ARMv7-A | TPIDRPRW supported; single-core |
| Cortex-A9 | ARMv7-A | TPIDRPRW supported; found in most dual/quad-core designs |
| Cortex-A15 | ARMv7-A | TPIDRPRW supported; high performance |
| Cortex-A17 | ARMv7-A | TPIDRPRW supported |

Note: All ARMv7-A implementations support TPIDRPRW per the architecture spec.
The register was introduced in ARMv6 (ARM11 family) and is mandatory in ARMv7-A.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Full name of TPIDRPRW? | Thread Pointer / ID Register, Privileged Read/Write |
| CP15 encoding? | `p15, 0, Rn, c13, c0, 4` |
| Write instruction? | `mcr p15, 0, Rn, c13, c0, 4` |
| Read instruction? | `mrc p15, 0, Rd, c13, c0, 4` |
| Can user space read TPIDRPRW? | No — Undefined Instruction fault at PL0 |
| Is it saved in context switch? | No — it's a per-CPU register, not per-task |
| What is TPIDRURW? | User-writable Thread ID Register — used for user TLS (glibc) |
| What is ALT_SMP for ARM32? | Boot-time patching mechanism for UP/SMP kernels |
| When is TPIDRPRW lost? | When the CPU core powers down (deep idle or hotplug) |
| When was TPIDRPRW introduced? | ARMv6 (ARM1136, ARM11 family), mandatory in ARMv7-A |
