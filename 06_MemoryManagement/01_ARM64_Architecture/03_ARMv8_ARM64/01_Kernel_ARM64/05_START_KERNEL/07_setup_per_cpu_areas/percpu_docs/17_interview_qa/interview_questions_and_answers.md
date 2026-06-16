# Interview Questions & Answers: Linux Per-CPU System (ARM32 & ARM64)

> **Purpose:** Complete reference for Kernel Engineer interviews covering
> the `setup_per_cpu_areas()` call chain, per-CPU mechanisms, and hardware
> register details for ARM32 and ARM64.

---

## Part 1: Fundamental Per-CPU Concepts

---

### Q1: What is per-CPU data in the Linux kernel, and why does it exist?

**Answer:**

Per-CPU data is a mechanism where each CPU core has its own private copy of a
variable. There is no sharing between CPUs, so no locks are needed for access.

**Why it exists:**
- **Performance:** Eliminates lock contention on frequently updated data
  (e.g., statistics counters, scheduler run queues, memory allocator caches)
- **Cache efficiency:** Each CPU's data lives in its own cache line — no false
  sharing, no cache bouncing between cores
- **Scalability:** As CPU count grows, per-CPU scales linearly (no shared lock bottleneck)

**Example (before vs after):**
```c
/* Before per-CPU: every CPU contends for this */
atomic_t global_alloc_count;
atomic_inc(&global_alloc_count);   /* lock prefix on x86, STXR retry on ARM */

/* After per-CPU: zero contention */
DEFINE_PER_CPU(int, local_alloc_count);
this_cpu_inc(local_alloc_count);   /* 3 instructions, no lock, no cache bounce */
```

---

### Q2: Why doesn't per-CPU access need locks?

**Answer:**

The key insight: **CPU N is the only CPU that writes to `__per_cpu_offset[N]`'s data**.

1. Each CPU has its own private memory region (the "unit")
2. `this_cpu_*()` accesses use the current CPU's offset from its hardware register
3. No other CPU can have the same offset → no memory sharing → no races

The only potential race is **CPU migration**: if a task reads the per-CPU pointer
on CPU 0, then migrates to CPU 1, it would access CPU 0's data from CPU 1.
This is prevented by:
- `preempt_disable()` before reading `this_cpu_ptr()`
- Interrupt context (hardware-guaranteed non-preemptible)
- `get_cpu_var()` (automatically calls `preempt_disable()`)

---

### Q3: What is `DEFINE_PER_CPU(int, x)` and what does it actually create?

**Answer:**

It's a macro that places a variable in the `.data..percpu` ELF section with the
`__percpu` sparse annotation:

```c
DEFINE_PER_CPU(int, x)
/* expands to: */
__percpu __attribute__((section(".data..percpu"))) int x;
```

What it creates:
- A single definition of `x` in the `.data..percpu` template section
- NOT NR_CPUS copies at compile time
- At boot time, `setup_per_cpu_areas()` creates NR_CPUS copies via `memcpy`

The template copy is only used for initialization. At runtime, all accesses
use the CPU-private allocated copies.

---

### Q4: Walk through `setup_per_cpu_areas()` step by step.

**Answer:**

`setup_per_cpu_areas()` lives in `mm/percpu.c:3383` (no arch override for ARM).

```
setup_per_cpu_areas()
  ↓
pcpu_embed_first_chunk(
    PERCPU_MODULE_RESERVE,    /* 8KB for modules */
    PERCPU_DYNAMIC_RESERVE,   /* 20KB for alloc_percpu */
    PAGE_SIZE,                /* align */
    pcpu_cpu_distance,        /* NUMA distance function */
    pcpu_fc_alloc,            /* allocator: memblock */
    pcpu_fc_free              /* free function */
)
```

Inside `pcpu_embed_first_chunk()`:

**Stage 1:** `pcpu_build_alloc_info()` — surveys CPUs, groups them by NUMA node,
computes optimal unit sizes, fills `pcpu_alloc_info` struct.

**Stage 2:** `memblock_alloc_try_nid()` per group — allocates physically contiguous
memory from each NUMA node's memblock. At this point memblock is the only allocator.

**Stage 3:** Find base address — the lowest VA across all group allocations.

**Stage 4:** Compute group offsets — `pcpu_unit_offsets[cpu]` = offset from base.

**Stage 5:** `memcpy()` per CPU — copies the `.data..percpu` template into each
CPU's private unit.

**Stage 6:** Free tail padding — if the last group's allocation has unused tail
memory, return it to memblock.

**Stage 7:** `pcpu_setup_first_chunk()` — sets `pcpu_base_addr`, computes
`__per_cpu_offset[cpu]` for each CPU, initializes chunk management structures.

---

### Q5: What is the formula for `__per_cpu_offset[cpu]`?

**Answer:**

```
__per_cpu_offset[cpu] = (pcpu_base_addr - __per_cpu_start) + pcpu_unit_offsets[cpu]
```

Where:
- `pcpu_base_addr` = VA of the first byte of the first chunk allocation
- `__per_cpu_start` = VA of the first byte of the `.data..percpu` template section
- `pcpu_unit_offsets[cpu]` = byte offset of CPU's unit from `pcpu_base_addr`

Why this formula works:

```
To access var for CPU N:
  result_addr = &var + __per_cpu_offset[N]
              = &var + (pcpu_base_addr - __per_cpu_start + pcpu_unit_offsets[N])
              = pcpu_base_addr + (&var - __per_cpu_start) + pcpu_unit_offsets[N]
                                   ↑
                        (offset of var within template section)
              = [start of chunk] + [var's offset in template] + [CPU N's unit start]
              = CPU N's private copy of var  ✓
```

---

## Part 2: ARM32 Specifics

---

### Q6: Which ARM32 register stores the per-CPU offset, and how is it accessed?

**Answer:**

Register: **TPIDRPRW** (`Thread Pointer / ID Register, Privileged Read/Write`)

CP15 encoding: `p15, 0, Rn, c13, c0, 4`

```asm
; Write (set_my_cpu_offset):
mcr   p15, 0, r0, c13, c0, 4   ; r0 = __per_cpu_offset[cpu]

; Read (__my_cpu_offset):
mrc   p15, 0, r2, c13, c0, 4   ; r2 = per-CPU offset
```

Properties:
- **PL1 access only** — user space cannot read/write (fault if attempted)
- **Per-core** — each core has its own physical register (no sharing)
- **Not context-switched** — saves/restores are NOT needed on task switch
  (the register belongs to the CPU, not the task)
- **Introduced in ARMv6** (ARM11 family), mandatory in ARMv7-A

---

### Q7: What is the ARM32 `.alt.smp.init` mechanism?

**Answer:**

ARM32 Linux needs to run the same kernel binary on both UP (single-processor)
and SMP (multi-processor) systems.

On UP systems, `__per_cpu_offset[0] = 0` always, so reading TPIDRPRW is
unnecessary overhead. The `.alt.smp.init` mechanism allows the kernel to
**patch itself at boot**:

- **SMP instruction** (in `.text`): `mrc p15, 0, r2, c13, c0, 4`
- **UP replacement** (in `.alt.smp.init` table): `mov r2, #0`

At boot, `fixup_smp()` scans the `.alt.smp.init` section. If the system is UP,
it overwrites each SMP instruction with its UP replacement. If SMP, no patching.

This is the ARM32 equivalent of ARM64's `ALTERNATIVE()` macro.

---

### Q8: Why is there a `"Q"` constraint in `__my_cpu_offset` on ARM32?

**Answer:**

The `"Q"` constraint is an ARM memory operand constraint. In `__my_cpu_offset`:

```c
asm volatile(ALT_SMP("mrc p15, 0, %0, c13, c0, 4", "mov %0, #0")
             : "=r" (off)
             : "Q" (*(unsigned long *)NULL));  /* ← hazard marker */
```

`"Q"` marks a fake memory input. The compiler treats the entire `asm` block
as if it **reads from memory**. This prevents the compiler from:

1. **Hoisting** the `mrc` above a preceding `preempt_disable()` store
2. **Reordering** the access relative to surrounding memory operations

Without `"Q"`: The compiler could legally move `mrc` before `preempt_disable()`
completes, reading a stale per-CPU offset for the wrong CPU.

The `*(unsigned long *)NULL` is never dereferenced at runtime — it's a syntactic
device to attach the constraint to the asm block.

---

## Part 3: ARM64 Specifics

---

### Q9: Which ARM64 register stores the per-CPU offset, and why might it change?

**Answer:**

**Default (non-VHE):** `tpidr_el1`
**VHE systems:** `tpidr_el2`

```c
set_my_cpu_offset():
    ALTERNATIVE("msr tpidr_el1, %0",
                "msr tpidr_el2, %0",
                ARM64_HAS_VIRT_HOST_EXTN)
```

**Why it changes on VHE:**

VHE (Virtualization Host Extensions, ARMv8.1-A) allows Linux KVM to run at EL2
instead of EL1. When the host kernel runs at EL2, `tpidr_el1` is reserved for
guest OS use during VM entry/exit. If Linux used `tpidr_el1` on a VHE system,
KVM would need to save/restore it on every VM entry/exit (expensive).

By using `tpidr_el2` on VHE, the host's per-CPU offset register survives VM
entry/exit without needing save/restore, since guests run at EL1 and cannot
access `tpidr_el2`.

---

### Q10: What is the `ALTERNATIVE()` macro and when does it apply?

**Answer:**

`ALTERNATIVE(orig_instr, new_instr, feature_flag)` patches the kernel's `.text`
section at boot time based on detected CPU features.

```
How it works:
1. `orig_instr` is placed in .text (executed by default)
2. `new_instr` is placed in .altinstr_replacement section
3. A descriptor is placed in .altinstructions (address, alt-address, feature)

At boot (apply_boot_alternatives()):
  For each descriptor in .altinstructions:
    If feature_flag is set in cpucap bitmask:
      Overwrite `orig_instr` with `new_instr`
      Flush I-cache for that range
      Execute ISB
```

For per-CPU on ARM64:
- `orig_instr` = `msr tpidr_el1, x0` (default)
- `new_instr` = `msr tpidr_el2, x0` (VHE patch)
- `feature_flag` = `ARM64_HAS_VIRT_HOST_EXTN`

After `apply_boot_alternatives()`, on VHE systems, the `msr tpidr_el1` instruction
in the kernel's `.text` section has been overwritten with `msr tpidr_el2`.

---

### Q11: Why does ARM64 have no `arch_setup_per_cpu_areas()`?

**Answer:**

Both ARM32 and ARM64 use the **generic** `setup_per_cpu_areas()` from `mm/percpu.c:3383`.
Neither architecture defines an `arch_setup_per_cpu_areas()` override.

This is because the arch-specific part of per-CPU setup is NOT the memory allocation —
it's only the **hardware register write**:
```
arch_setup_per_cpu_areas() — NOT present for ARM/ARM64
  (the memory layout, memblock allocation, and offset table
   computation are all architecture-independent)

set_my_cpu_offset() — THIS is the arch-specific part
  (called from smp_prepare_boot_cpu() and secondary_start_kernel())
  (writes TPIDRPRW on ARM32, tpidr_el1/el2 on ARM64)
```

The generic allocator `pcpu_embed_first_chunk()` handles all memory layout decisions,
NUMA grouping, and offset computation. It only calls `pcpu_fc_alloc()` (which uses
`memblock_alloc_try_nid()`) for the actual allocation — and that is already
architecture-aware through memblock's NUMA node awareness.

---

## Part 4: Secondary CPU Bring-Up

---

### Q12: What happens when a secondary CPU boots regarding per-CPU setup?

**Answer:**

ARM32 (`arch/arm/kernel/smp.c:secondary_start_kernel()`):
```c
/* 1. Disable preemption */
preempt_disable();
/* 2. Get this CPU's logical ID */
cpu = smp_processor_id();    /* reads MPIDR, not TPIDRPRW */
/* 3. Write per-CPU offset to TPIDRPRW */
set_my_cpu_offset(per_cpu_offset(cpu));
/* Executes: mcr p15, 0, r0, c13, c0, 4 */
/* 4. CPU is now ready for this_cpu_*() access */
cpu_init();
...
```

ARM64 (`arch/arm64/kernel/smp.c:secondary_start_kernel()`):
```c
/* 1. Get CPU ID */
cpu = task_cpu(current);
/* 2. Write per-CPU offset to tpidr_el1 (or el2) */
set_my_cpu_offset(per_cpu_offset(cpu));
/* Executes: msr tpidr_el1, x0 (or tpidr_el2 on VHE) */
/* 3. Apply alternatives for this CPU (ARM64-specific) */
apply_alternatives_all();
/* 4. Continue initialization */
notify_cpu_starting(cpu);
...
```

**Critical ordering:** `set_my_cpu_offset()` must come before any `cpu_init()` call,
because `cpu_init()` uses `this_cpu_*()` macros internally.

---

### Q13: Must `tpidr_el1` be re-written after CPU hotplug (offline → online)?

**Answer:**

**Yes.** When a CPU core powers down (`PSCI CPU_OFF`), all its hardware registers —
including `tpidr_el1` / TPIDRPRW — are reset to their power-on reset values.

When the CPU comes back online:
1. PSCI firmware powers the core back on
2. `secondary_entry` (head.S) is the entry point again
3. `secondary_start_kernel()` is called again
4. `set_my_cpu_offset(per_cpu_offset(cpu))` re-writes the register

This is transparent to the kernel because the `secondary_start_kernel()` path
is used for both initial boot AND hotplug re-online. The `__per_cpu_offset[]`
array was computed once at boot and never changes, so the re-write uses the
same value as before.

---

## Part 5: Memory Layout and Allocator

---

### Q14: What is `pcpu_build_alloc_info()` doing?

**Answer:**

`pcpu_build_alloc_info()` (`mm/percpu.c:2864`) is the NUMA-aware grouping algorithm
that determines how CPUs are organized into memory allocation groups.

```
Input: nr_cpus, NUMA topology, cpu_distance_fn

Algorithm:
1. Group CPUs by NUMA node (all CPUs on the same node → same group)
2. Calculate nr_units: max(nr_cpus, nr_groups * PCPU_MIN_NR_UNITS_PER_GROUP)
3. Optimize: if using more units than CPUs (to fill groups),
   expand unit size rather than add empty units
   "75% fill" rule: if filling would exceed 3/4 capacity, expand size instead
4. Assign CPUs to unit slots within each group
5. Compute unit size = max(static_size + reserved + dyn, PCPU_MIN_UNIT_SIZE)

Output:
  pcpu_alloc_info {
    unit_size   = 32KB (typically)
    atom_size   = PAGE_SIZE
    nr_groups   = (number of NUMA nodes with CPUs)
    groups[]    = { nr_units, cpu_map[], base_offset }
  }
```

Returns: `pcpu_alloc_info*` (allocated from memblock)

---

### Q15: What is `pcpu_base_addr`?

**Answer:**

`pcpu_base_addr` is a global variable set by `pcpu_setup_first_chunk()`:

```c
/* mm/percpu.c */
void *pcpu_base_addr __read_mostly;
```

**Value:** The virtual address of the very first byte of the first chunk allocation
(i.e., the start of CPU 0's unit in the first group).

**Role in the delta formula:**
```
__per_cpu_offset[cpu] = (pcpu_base_addr - __per_cpu_start) + pcpu_unit_offsets[cpu]
```

It bridges the gap between:
- The **template** (`.data..percpu` in the kernel image)
- The **allocation** (the embedded chunk created by `pcpu_embed_first_chunk()`)

---

### Q16: What is `PCPU_MIN_UNIT_SIZE` and why does it exist?

**Answer:**

`PCPU_MIN_UNIT_SIZE = SZ_32K = 32768 bytes`

The minimum size enforced for each CPU's private area, even if the actual
static + reserved + dynamic sum is less.

**Why it exists:**
1. **Future-proofing:** Ensures sufficient space for dynamic `alloc_percpu()`
   requests without having to relocate the chunk
2. **Alignment:** Keeps CPU units on reasonably large boundaries, improving
   spatial locality and cache behavior
3. **Module compatibility:** Modules loaded later may add per-CPU variables.
   The reserved module area (8KB) is carved from this space.
4. **Buddy allocator compatibility:** The allocation bitmap is managed in
   `PCPU_MIN_ALLOC_SIZE` (8-byte) blocks; larger units give more granularity

If the static section + reserves exceeds 32KB, the unit grows to fit.

---

## Part 6: Advanced Topics

---

### Q17: What is the `alloc_map` bitmap in `pcpu_chunk`?

**Answer:**

`alloc_map` is a bitmap within `struct pcpu_chunk` that tracks which
`PCPU_MIN_ALLOC_SIZE`-sized (8-byte) blocks in the chunk are allocated vs free.

```c
struct pcpu_chunk {
    unsigned long   *alloc_map;  /* block allocation map */
    unsigned long   *bound_map;  /* allocation boundary map */
    /* ... */
};
```

- One bit per 8-byte block
- A 32KB unit has 32768 / 8 = 4096 blocks → 4096 bits = 512 bytes of bitmap
- `alloc_map[bit] = 1` → block is allocated
- `alloc_map[bit] = 0` → block is free

The allocator scans `alloc_map` to find free contiguous blocks for
`alloc_percpu(size)` requests.

---

### Q18: What is `pcpu_slot[]` and how does it relate to chunk management?

**Answer:**

`pcpu_slot[]` is an array of lists (one per "free space size bucket") used to
find chunks with sufficient free space for `alloc_percpu()` requests.

```c
/* mm/percpu.c */
static LIST_HEAD(pcpu_slot[PCPU_NR_SLOT]);
/*
 * Slot N contains chunks that have >= 2^N bytes free.
 * alloc_percpu(size):
 *   → compute slot = order_base_2(size)
 *   → search pcpu_slot[slot..PCPU_NR_SLOT-1] for a chunk with enough free space
 *   → use pcpu_find_block_fit() to find exact location within chunk
 */
```

`pcpu_chunk_relocate()` moves chunks between slots when their free space changes
(after allocation or deallocation).

---

### Q19: What is the difference between `__per_cpu_load` and `__per_cpu_start`?

**Answer:**

Both are linker symbols, but they serve different purposes:

- **`__per_cpu_start`**: The **virtual address** where the `.data..percpu` section
  begins in the kernel's VA space. Used as the base for address computation.

- **`__per_cpu_load`**: The **load address** (physical or VMA at link time) where
  the template data is initially stored in the kernel image. On most configs,
  `__per_cpu_load == __per_cpu_start`, but they can differ in configurations
  where the per-CPU section is loaded separately (e.g., for early memory mapping).

In `pcpu_embed_first_chunk()`, `__per_cpu_load` is used as the `src` in `memcpy()`
to copy the template data into each CPU's unit.

---

### Q20: How does `cpu_number` get set to the correct value for each CPU?

**Answer:**

`cpu_number` is a per-CPU variable defined with `DEFINE_PER_CPU_FIRST`:

```c
/* arch/arm/kernel/setup.c (ARM32) */
/* arch/arm64/kernel/setup.c (ARM64) */
DEFINE_PER_CPU_FIRST(int, cpu_number);
```

The `_FIRST` ensures it's at offset 0 within the per-CPU template section
(placed in `.data..percpu..first` subsection, which is linked first).

**Setup sequence:**

1. `setup_per_cpu_areas()` copies template (all zeros) to each CPU's unit.
   After this, all CPUs' `cpu_number` = 0 (zero-initialized).

2. `setup_per_cpu_areas()` calls `pcpu_setup_first_chunk()` which calls
   `setup_percpu_segment()` → which calls `set_my_cpu_offset()` for the
   boot CPU only.

3. For each CPU: `per_cpu(cpu_number, cpu) = cpu;` — explicitly sets each
   CPU's copy of `cpu_number` to the correct CPU ID.

This is done in `setup_per_cpu_areas()` for the boot CPU and in
`secondary_start_kernel()` context (via `cpu_init()`) for secondary CPUs.

---

## ARM32 vs ARM64 Comparison Table

| Topic | ARM32 | ARM64 |
|---|---|---|
| Arch file | `arch/arm/` | `arch/arm64/` |
| percpu.h | `arch/arm/include/asm/percpu.h` | `arch/arm64/include/asm/percpu.h` |
| arch_setup_per_cpu_areas() | Not present (uses generic) | Not present (uses generic) |
| Hardware register | TPIDRPRW (CP15 c13, op2=4) | tpidr_el1 (non-VHE) / tpidr_el2 (VHE) |
| Write instruction | `mcr p15, 0, Rn, c13, c0, 4` | `msr tpidr_el1, Xn` |
| Read instruction | `mrc p15, 0, Rd, c13, c0, 4` | `mrs Xd, tpidr_el1` |
| Register width | 32-bit | 64-bit |
| Boot patching mechanism | `.alt.smp.init` (UP ↔ SMP) | `ALTERNATIVE()` (non-VHE ↔ VHE) |
| When patching applies | UP system → patch out TPIDRPRW read | VHE system → switch to tpidr_el2 |
| "Q" constraint | Yes | Yes |
| Hypervisor register | N/A (ARMv7 Hyp mode, rarely used) | tpidr_el2 (EL2 = KVM host on VHE) |
| secondary_start_kernel | arch/arm/kernel/smp.c:410 | arch/arm64/kernel/smp.c:203 |
| apply_alternatives in secondary | No | Yes (apply_alternatives_all()) |
| CPU wake-up mechanism | SGI / pen release / boot ROM | PSCI CPU_ON |
| Hotplug register re-write | Yes (secondary_start_kernel again) | Yes (secondary_start_kernel again) |
| set_my_cpu_offset call site | smp_prepare_boot_cpu() + secondary_start_kernel() | smp_prepare_boot_cpu() + secondary_start_kernel() |

---

## Quick Reference: Key Source Locations

| Component | File | Line |
|---|---|---|
| `setup_per_cpu_areas()` | `mm/percpu.c` | 3383 |
| `pcpu_embed_first_chunk()` | `mm/percpu.c` | 3075 |
| `pcpu_build_alloc_info()` | `mm/percpu.c` | 2864 |
| `pcpu_setup_first_chunk()` | `mm/percpu.c` | 2608 |
| ARM32 `set_my_cpu_offset()` | `arch/arm/include/asm/percpu.h` | 17 |
| ARM32 `__my_cpu_offset` | `arch/arm/include/asm/percpu.h` | 27 |
| ARM32 `secondary_start_kernel()` | `arch/arm/kernel/smp.c` | 410 |
| ARM64 `set_my_cpu_offset()` | `arch/arm64/include/asm/percpu.h` | 15 |
| ARM64 `__kern_my_cpu_offset()` | `arch/arm64/include/asm/percpu.h` | 32 |
| ARM64 `secondary_start_kernel()` | `arch/arm64/kernel/smp.c` | 203 |
| `DEFINE_PER_CPU` | `include/linux/percpu-defs.h` | 114 |
| `SHIFT_PERCPU_PTR` | `include/linux/percpu-defs.h` | 230 |
| `__per_cpu_offset[]` declaration | `include/asm-generic/percpu.h` | 19 |
| `setup_per_cpu_areas()` call site | `init/main.c` | 901 |

---

## PPC64 Add-On (Cross-Architecture Interview)

### Q21: Does PPC64 use the generic `setup_per_cpu_areas()`?

**Answer:**

PPC64 implements an arch wrapper in `arch/powerpc/kernel/setup_64.c`, but still relies on
generic percpu allocators (`pcpu_embed_first_chunk` and fallback `pcpu_page_first_chunk`).

### Q22: Where does PPC64 store current CPU percpu offset?

**Answer:**

In PACA: `local_paca->data_offset` via `#define __my_cpu_offset local_paca->data_offset`
(`arch/powerpc/include/asm/percpu.h`).

### Q23: What is PPC64-specific in setup logic?

**Answer:**

`atom_size` is selected by MMU mode:
- Book3E: `SZ_1M`
- radix: `PAGE_SIZE`
- hash MMU: `PAGE_SIZE` if 4K linear psize, else `SZ_1M`

Then PPC64 writes both generic and arch views:
`__per_cpu_offset[cpu]` and `paca_ptrs[cpu]->data_offset`.
