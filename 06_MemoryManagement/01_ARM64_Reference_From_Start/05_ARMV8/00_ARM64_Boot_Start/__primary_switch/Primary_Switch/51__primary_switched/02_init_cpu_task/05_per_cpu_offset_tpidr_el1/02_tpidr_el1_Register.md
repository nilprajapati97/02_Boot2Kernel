# `tpidr_el1` Register — Deep Architectural Analysis

## ARM64 Thread ID Registers

ARM64 provides three Thread ID Registers (TIDRs):

| Register | Accessible from | Linux Use |
|---|---|---|
| `tpidr_el0` | EL0 and EL1+ | User-space TLS pointer (`pthread_self()`) |
| `tpidrro_el0` | EL0 (read-only) + EL1+ | Auxiliary user TLS (e.g., Android bionic) |
| `tpidr_el1` | EL1+ only | **Linux per-CPU offset** |
| `tpidr_el2` | EL2+ only | **Linux per-CPU offset (VHE mode)** |

`tpidr_el1` is perfect for per-CPU data because:
1. User-space at EL0 CANNOT read it (EL1 privilege required)
2. Each CPU core has its own physical register (no sharing)
3. Readable in one `mrs` instruction
4. Not used by any standard ABI

---

## `set_this_cpu_offset` — The Installation Macro

```asm
// arch/arm64/include/asm/percpu.h:
.macro set_this_cpu_offset, val
#ifdef CONFIG_ARM64_VHE
alternative_if_not ARM64_HAS_VIRT_HOST_EXTN
    msr     tpidr_el1, \val    // non-VHE: use EL1 register
alternative_else
    msr     tpidr_el2, \val    // VHE: kernel at EL2, use EL2 register
alternative_endif
#else
    msr     tpidr_el1, \val    // no VHE support: always EL1
#endif
.endm
```

**VHE (Virtualization Host Extensions):** When `HCR_EL2.E2H=1`, the kernel runs at
EL2. In this mode, `tpidr_el1` is controlled by the (guest) EL1, while `tpidr_el2`
is the host kernel's register. `set_this_cpu_offset` selects the right register
at runtime via `alternative` patching.

---

## `read_sysreg(tpidr_el1)` — The Complementary Read Macro

```c
// arch/arm64/include/asm/sysreg.h:
#define read_sysreg(r)  ({                      \
    u64 __val;                                  \
    asm volatile("mrs %0, " __stringify(r)      \
                 : "=r" (__val));               \
    __val;                                      \
})

// Usage:
unsigned long pcpu_offset = read_sysreg(tpidr_el1);
// Compiles to: mrs x0, tpidr_el1
```

Or for VHE-aware code:
```c
// arch/arm64/include/asm/percpu.h:
static inline unsigned long __my_cpu_offset(void)
{
    return read_sysreg(tpidr_el1);
    // On VHE builds: read_sysreg(tpidr_el2)
}
#define __my_cpu_offset __my_cpu_offset()
```

---

## `lsl #3` in the Per-CPU Offset Load

```asm
ldr     \tmp1, [\tmp1, \tmp2, lsl #3]
```

This is an ARM64 **register-offset addressing** with shift:
- Base register: `tmp1` = `&__per_cpu_offset[0]` (start of array)
- Index: `tmp2` = CPU ID (0-based integer)
- Shift: `lsl #3` = multiply index by 8

Result: loads `__per_cpu_offset[cpu_id]` — correct for a `u64` array.

**Why the shift must be 3 (not 2 or 4):**
- `__per_cpu_offset` is `unsigned long __per_cpu_offset[NR_CPUS]`
- On ARM64, `unsigned long` = 64-bit = 8 bytes
- Array stride = 8 bytes = `1 << 3`
- Therefore index arithmetic: `base + cpu * 8 = base + (cpu << 3)`

If the shift were 2 (×4): we'd load from the wrong offset (32-bit stride instead of 64-bit).
If the shift were 4 (×16): we'd load 2 entries past the right one.

---

## Timing: When `__per_cpu_offset[0]` Is Valid

The critical question: is `__per_cpu_offset[0]` properly initialized when
`init_cpu_task` runs during early boot?

```
Boot sequence timeline:
T1: Bootloader → primary_entry()
T2: MMU on → __primary_switched()
T3: init_cpu_task → reads __per_cpu_offset[0]   ← IS IT VALID?
T4: start_kernel() → setup_arch() → setup_per_cpu_areas()
```

`setup_per_cpu_areas()` (T4) is what INITIALIZES `__per_cpu_offset` for all CPUs.
But `init_cpu_task` runs at T3 — BEFORE T4!

**How does this work?**

Answer: `__per_cpu_offset[0]` is pre-initialized at compile time / link time. For
CPU0, the per-CPU section IS the prototype section. So `__per_cpu_offset[0] = 0`
(or a small fixed value). The linker places a `.data` initialization:

```c
// kernel/percpu.c or vmlinux.lds.S:
// CPU0's per-CPU data starts at __per_cpu_start
// __per_cpu_offset[0] = __per_cpu_start - __per_cpu_start = 0
// OR a small relocation offset
```

When `setup_per_cpu_areas()` runs later, it MAY update `__per_cpu_offset[0]` if
it reallocates the per-CPU section (e.g., NUMA-aware placement). But between T3
and T4, `tpidr_el1 = __per_cpu_offset[0] = 0` works correctly for CPU0 because
per-CPU variable accesses from the prototype address are valid.

---

## ISB After `set_this_cpu_offset`

Does `set_this_cpu_offset` require an ISB? Let's check:

```asm
msr     tpidr_el1, x5    // write per-CPU offset
// NO ISB here in init_cpu_task
// (next use of tpidr_el1 is in C code via mrs)
```

The ARM architecture guarantees that an `msr` to a non-context-synchronizing
register (like `tpidr_el1`) is visible to subsequent `mrs` instructions within
the SAME exception level and SAME PE without an ISB. The guarantee is:
_"A read of a non-context-synchronizing register after a write to the same
register, within the same exception level, is guaranteed to observe the write."_

`tpidr_el1` is classified as non-context-synchronizing. No ISB needed.

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