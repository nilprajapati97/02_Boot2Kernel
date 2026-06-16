## `setup_per_cpu_areas()` — Deep Dive for Interview (ARM32 & ARM64)

---

### 1. What Problem Does It Solve?

In an SMP system, many kernel subsystems need **per-CPU variables** — data that each CPU owns privately with no locking (e.g., `current_task`, `irq_stack_ptr`, scheduler runqueues, `loops_per_jiffy`).

The compiler places all `DEFINE_PER_CPU(type, var)` declarations in a special ELF section: **`.data..percpu`**. At link time, there is only *one* copy of these in the kernel image (the "master template"). `setup_per_cpu_areas()` is called in `start_kernel()` (line 901) to **replicate this template once per CPU** and set up the addressing mechanism so each CPU transparently accesses its own copy.

---

### 2. The Memory Layout Created

```
__per_cpu_load / __per_cpu_start  (linker template, one copy)

After setup:
  CPU0 area: [static | reserved | dynamic]   <-- copy of template
  CPU1 area: [static | reserved | dynamic]   <-- copy of template
  CPU2 area: [static | reserved | dynamic]   <-- ...
```

Each CPU's area has three regions:
- **Static**: copied from the linker template at boot
- **Reserved**: for kernel module percpu variables (`PERCPU_MODULE_RESERVE`)
- **Dynamic**: runtime `alloc_percpu()` allocations (`PERCPU_DYNAMIC_RESERVE`)

---

### 3. The Generic SMP Path — `pcpu_embed_first_chunk()`

For both ARM32 and ARM64, when `CONFIG_HAVE_SETUP_PER_CPU_AREA` is **not** defined (which is the default for most ARM configs), the generic fallback in percpu.c is used:

```c
// mm/percpu.c:3383
void __init setup_per_cpu_areas(void)
{
    unsigned long delta;
    unsigned int cpu;
    int rc;

    rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
                                PERCPU_DYNAMIC_RESERVE,
                                PAGE_SIZE, NULL, NULL);
    if (rc < 0)
        panic("Failed to initialize percpu areas.");

    delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
    for_each_possible_cpu(cpu)
        __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
}
```

**`pcpu_embed_first_chunk`** does:
1. Groups CPUs by NUMA node
2. For each group, allocates one physically contiguous block from `memblock` (bootmem)
3. `memcpy(ptr, __per_cpu_load, static_size)` — copies the template into each CPU's area
4. Calculates `pcpu_base_addr` = the lowest allocated address across all groups

Then the **offset array** is populated:
```
__per_cpu_offset[cpu] = pcpu_base_addr_delta + pcpu_unit_offsets[cpu]
```

This offset, when added to any percpu symbol address (`__per_cpu_start`-relative), gives the CPU-private address.

---

### 4. How Per-CPU Access Works at Runtime

#### The `per_cpu(var, cpu)` Macro

```c
// Simplified:
per_cpu(var, cpu)  →  *(typeof(var) *)((unsigned long)&var + __per_cpu_offset[cpu])
```

The key insight: the linker symbol `&var` points into the **template** section. Adding `__per_cpu_offset[cpu]` redirects to that CPU's private copy.

#### `this_cpu_read(var)` / `raw_cpu_read(var)` — The Hot Path

This uses `__my_cpu_offset` which is **read from a hardware register** — no array lookup, no memory access for the offset itself.

---

### 5. ARM32 — `TPIDRPRW` Register (CP15 c13)

ARM32 stores the per-CPU offset in the **Thread ID Register for Privileged Reads/Writes**: `TPIDRPRW` (CP15 register c13, opcode 4).

From percpu.h:

```c
// Writing the offset (called during CPU bring-up via set_my_cpu_offset):
static inline void set_my_cpu_offset(unsigned long off)
{
    /* Set TPIDRPRW */
    asm volatile("mcr p15, 0, %0, c13, c0, 4" : : "r" (off) : "memory");
}

// Reading the offset (used by __my_cpu_offset macro):
static __always_inline unsigned long __my_cpu_offset(void)
{
    unsigned long off;
    asm("mrc p15, 0, %0, c13, c0, 4" : "=r" (off) : ...);
    return off;
}
```

- **`mcr p15, 0, Rn, c13, c0, 4`** — Move to Coprocessor (write TPIDRPRW)
- **`mrc p15, 0, Rd, c13, c0, 4`** — Move from Coprocessor (read TPIDRPRW)

On each secondary CPU boot (smp.c):
```c
set_my_cpu_offset(per_cpu_offset(smp_processor_id()));
```

So each CPU writes its own `__per_cpu_offset[cpu]` value into its own `TPIDRPRW` — the register is **banked per-CPU** in hardware.

**ARMv6 note**: For ARMv6 UP (uniprocessor), `TPIDRPRW` may not exist. The code has an alternative path using `__per_cpu_offset[0]` directly, selected at runtime via the `.alt.smp.init` patching mechanism.

---

### 6. ARM64 — `tpidr_el1` / `tpidr_el2` Register

ARM64 uses the **Thread Pointer ID Register for EL1**: `tpidr_el1` (or `tpidr_el2` under VHE/KVM).

From percpu.h:

```c
static inline void set_my_cpu_offset(unsigned long off)
{
    asm volatile(ALTERNATIVE("msr tpidr_el1, %0",
                             "msr tpidr_el2, %0",   // VHE (KVM host)
                             ARM64_HAS_VIRT_HOST_EXTN)
                :: "r" (off) : "memory");
}

static inline unsigned long __kern_my_cpu_offset(void)
{
    unsigned long off;
    asm(ALTERNATIVE("mrs %0, tpidr_el1",
                    "mrs %0, tpidr_el2",
                    ARM64_HAS_VIRT_HOST_EXTN)
        : "=r" (off) : ...);
    return off;
}
```

- **`msr tpidr_el1, Xn`** — write offset to system register
- **`mrs Xd, tpidr_el1`** — read offset (single instruction, extremely fast)

The `ALTERNATIVE()` macro patches the instruction at boot time: if the CPU supports VHE (Virtualization Host Extensions), `tpidr_el2` is used instead (because VHE runs the kernel at EL2, and `tpidr_el1` would be used by guest VMs).

---

### 7. ARM32 vs ARM64 — Key Differences

| Aspect | ARM32 | ARM64 |
|---|---|---|
| Register | `TPIDRPRW` (CP15, c13, opcode4) | `tpidr_el1` (or `tpidr_el2` with VHE) |
| Access instruction | `mcr`/`mrc p15, 0, Rn, c13, c0, 4` | `msr`/`mrs tpidr_el1, Xn` |
| VHE/Virtualization | No | Yes — `ALTERNATIVE()` patches at boot |
| ARMv6 UP fallback | `.alt.smp.init` patches to use `__per_cpu_offset[0]` | Not needed |
| Word size | 32-bit offset | 64-bit offset |
| Atomic ops | Generic LL/SC via `ldrex`/`strex` | Optimized with LSE atomics (`stadd`, etc.) |

---

### 8. UP (Uniprocessor) Path

When `CONFIG_SMP` is not set, percpu.c uses a trivial path:
```c
void __init setup_per_cpu_areas(void)
{
    // Allocate one unit, identity-mapped
    // No offset needed — per_cpu(var, 0) == var directly
}
```
No register needs to be set up; `__my_cpu_offset` is always 0.

---

### 9. Summary Flow for Interview

```
start_kernel()
  └─ setup_per_cpu_areas()
       └─ pcpu_embed_first_chunk()
            ├─ pcpu_build_alloc_info()      // Group CPUs by NUMA node
            ├─ memblock_alloc() per group   // Allocate CPU areas from bootmem
            ├─ memcpy(__per_cpu_load, ...)  // Copy static template to each area
            └─ pcpu_setup_first_chunk()     // Set pcpu_base_addr, unit offsets
       └─ for_each_possible_cpu:
            __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu]

  CPU N boots (secondary_start_kernel / smp.c):
       └─ set_my_cpu_offset(__per_cpu_offset[N])
            └─ ARM32: mcr p15, 0, Rn, c13, c0, 4  (writes TPIDRPRW)
            └─ ARM64: msr tpidr_el1, Xn            (writes tpidr_el1)

  Runtime access — this_cpu_read(var):
       └─ reads TPIDRPRW / tpidr_el1 → offset
       └─ adds to &var → CPU-private address
       └─ No locks, no atomics needed
```

**Key interview points to emphasize:**
- The register (`TPIDRPRW` / `tpidr_el1`) is **physically banked** — each CPU has its own copy
- `this_cpu_ptr()` is a **single instruction** at the offset read — no memory indirection for the common case
- The "embed" strategy piggy-backs on the kernel's linear mapping, allowing the MMU's large-page TLB entries to also cover percpu data (TLB efficiency)
- ARM64 adds `ALTERNATIVE()` patching for VHE — the same binary works in both bare-metal EL1 and KVM host EL2 modes