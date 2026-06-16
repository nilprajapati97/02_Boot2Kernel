# System Overview: Per-CPU Variables in the Linux Kernel

## Source References
- `include/linux/percpu-defs.h` — macro definitions
- `include/asm-generic/percpu.h` — generic offset infrastructure
- `mm/percpu.c:1-60` — comprehensive subsystem header comment

---

## 1. The Problem: SMP Without Locks

In a symmetric multiprocessor (SMP) system, every CPU shares the same kernel code and data.
Many kernel subsystems need **per-CPU state** — data that logically belongs to one CPU and
is only accessed by that CPU:

- Scheduler runqueues (`struct rq`)
- IRQ statistics (`irq_stat`)
- Current task pointer, stack canary
- `loops_per_jiffy` calibration
- Network receive queues, block device queues
- `kstack_offset` (KASLR stack randomization)

If this data were stored in a global array `data[NR_CPUS]`, every access would touch a
cache line potentially owned by a different CPU, causing **false sharing** — the cache
coherency protocol serializes accesses even though CPUs are logically independent.

**Per-CPU variables solve this by giving each CPU its own private copy at a different
physical address**, completely eliminating cache line contention and the need for any
locking on the hot path.

---

## 2. The Template-and-Copy Model

The compiler and linker handle per-CPU variables through a special ELF section:

```c
// include/linux/percpu-defs.h:49
#define __PCPU_ATTRS(sec) \
    __percpu __attribute__((section(PER_CPU_BASE_SECTION sec))) \
    PER_CPU_ATTRIBUTES

// PER_CPU_BASE_SECTION = ".data..percpu" (SMP) or ".data" (UP)
```

So `DEFINE_PER_CPU(int, my_counter)` compiles to:
```c
int __attribute__((section(".data..percpu"))) my_counter;
```

At link time, ALL per-CPU variables from ALL kernel subsystems are collected into the
`.data..percpu` section. This creates a **single master template** in the kernel image,
bounded by the linker symbols:

```
__per_cpu_load    ← physical load address of template (may differ from virtual)
__per_cpu_start   ← virtual address where template begins
__per_cpu_end     ← virtual address where template ends

Template size = __per_cpu_end - __per_cpu_start
```

`setup_per_cpu_areas()` copies this template **once per possible CPU** into separately
allocated memory regions, then wires each CPU to its private copy via a hardware register.

---

## 3. The Three Regions Per CPU Unit

Each CPU's allocated area is divided into three consecutive regions:

```
+---------------------------------------------------+
|  STATIC REGION                                    |
|  size = __per_cpu_end - __per_cpu_start           |
|  Initialized by: memcpy(__per_cpu_load, ...)      |
|  Contains: all DEFINE_PER_CPU() variables         |
|  Management: NEVER freed (immutable)              |
+---------------------------------------------------+
|  RESERVED REGION                                  |
|  size = PERCPU_MODULE_RESERVE (typically 8KB)     |
|  Purpose: kernel module DEFINE_PER_CPU() vars     |
|  Management: pcpu_reserved_chunk                  |
+---------------------------------------------------+
|  DYNAMIC REGION                                   |
|  size = PERCPU_DYNAMIC_RESERVE (typically 20KB)   |
|  Purpose: alloc_percpu() / __alloc_percpu()       |
|  Management: pcpu_first_chunk (bitmap allocator)  |
+---------------------------------------------------+
|  (Unused padding to unit_size boundary)           |
+---------------------------------------------------+
```

The total `unit_size` is page-aligned and at least `PCPU_MIN_UNIT_SIZE` bytes.

---

## 4. The Offset Mechanism

After setup, the kernel maintains an array:
```c
// include/asm-generic/percpu.h:19
unsigned long __per_cpu_offset[NR_CPUS];
```

For each CPU:
```
__per_cpu_offset[cpu] = (address of CPU's unit) - (address of template)
```

To access variable `var` on CPU N:
```c
per_cpu(var, N)  ==  *(typeof(var) *)((unsigned long)&var + __per_cpu_offset[N])
```

The symbol `&var` points into the template section. Adding the offset redirects to CPU N's
private copy. The template address is a **link-time constant**; the offset is a
**runtime value loaded from a hardware register** (one instruction, no memory access).

---

## 5. Hardware Register Acceleration

To make `this_cpu_read(var)` — the most common access pattern — as fast as possible,
the kernel stores the **current CPU's offset in a dedicated hardware register**:

| Architecture | Register | Write | Read |
|---|---|---|---|
| ARM32 (SMP) | TPIDRPRW (CP15 c13,c0,4) | `mcr p15, 0, Rn, c13, c0, 4` | `mrc p15, 0, Rd, c13, c0, 4` |
| ARM64 (non-VHE) | `tpidr_el1` | `msr tpidr_el1, Xn` | `mrs Xd, tpidr_el1` |
| ARM64 (VHE/KVM) | `tpidr_el2` | `msr tpidr_el2, Xn` | `mrs Xd, tpidr_el2` |

These registers are **physically banked per CPU core** — each core has its own copy of
the register, so reading it always returns the current CPU's private offset without any
inter-CPU communication.

The result: `this_cpu_read(var)` compiles to exactly **3 instructions**:
```asm
; ARM64 example
mrs  x0, tpidr_el1        ; load per-cpu offset for this CPU (1 cycle)
add  x0, x0, #<var_offset> ; add compile-time constant offset of 'var'
ldr  w1, [x0]             ; load the value
```

---

## 6. SMP vs UP (Uniprocessor) Path

**SMP (`CONFIG_SMP=y`):**
- `setup_per_cpu_areas()` at `mm/percpu.c:3383` allocates N units from bootmem
- `__per_cpu_offset[cpu]` is populated for each CPU
- Each CPU writes its offset to the hardware register on bring-up

**UP (`CONFIG_SMP=n`):**
- `setup_per_cpu_areas()` at `mm/percpu.c:3413` allocates a single unit
- `__per_cpu_offset[0] = 0` (identity — no relocation needed)
- `__my_cpu_offset` is always 0
- `per_cpu(var, 0)` compiles to a direct access of the template variable

---

## 7. Initialization Timeline

```
Bootloader → head.S
    │
    ▼
early_setup()  [PowerPC] / setup_arch()  [ARM/ARM64]
    │  Set up temporary PACA/boot_paca with data_offset=0 (PowerPC)
    │  Or: per-CPU access works directly on template until setup_per_cpu_areas
    │
    ▼
start_kernel()  [init/main.c]
    │
    ├─ setup_arch(&command_line)         ← arch-specific early init
    ├─ setup_command_line(command_line)
    ├─ setup_nr_cpu_ids()
    │
    ├─ setup_per_cpu_areas()  ←─────────── [LINE 901] THIS IS THE FUNCTION
    │       Allocates and initializes all per-CPU areas
    │       Populates __per_cpu_offset[cpu] for all CPUs
    │
    ├─ smp_prepare_boot_cpu()  ←────────── Writes hw register for CPU0
    │       ARM32: set_my_cpu_offset(per_cpu_offset(0)) → mcr TPIDRPRW
    │       ARM64: set_my_cpu_offset(per_cpu_offset(0)) → msr tpidr_el1
    │
    └─ [rest of kernel init...]
           ▼
    CPU N brought online:
    secondary_start_kernel()
        set_my_cpu_offset(__per_cpu_offset[N]) → writes hw register
```

After `smp_prepare_boot_cpu()`, per-CPU access via `this_cpu_*` works correctly
on the boot CPU. Secondary CPUs gain this capability in `secondary_start_kernel()`.

---

## 8. Why This Design is Lock-Free

1. **Spatial isolation**: Each CPU's data is at a different physical address — no shared
   cache lines between CPUs for `this_cpu_*` accesses.

2. **Register-based dispatch**: Reading the hardware register (one instruction) gives the
   offset; no shared data structure is consulted at access time.

3. **Single-writer rule**: `this_cpu_*` operations are only valid on the current CPU's
   own data. The kernel enforces this by preemption disabling in `get_cpu_var()` /
   `this_cpu_ptr()` (or by the caller being in interrupt context where migration is
   impossible).

4. **Cross-CPU access**: When CPU A reads CPU B's per-CPU variable via `per_cpu(var, B)`,
   it uses `__per_cpu_offset[B]` from the global array. This is a read-only array after
   setup, so no locking is needed.
