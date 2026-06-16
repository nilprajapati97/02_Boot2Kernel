# Generic `setup_per_cpu_areas()` — mm/percpu.c

## Source Reference
- `mm/percpu.c:3383` — SMP implementation
- `mm/percpu.c:3413` — UP implementation
- Used by: ALL architectures that don't define their own override

---

## Who Uses This Function?

Both ARM32 and ARM64 use this generic implementation. Let's verify:

```bash
# Search for arch-specific overrides:
$ grep -r "setup_per_cpu_areas" arch/arm/   # → empty (no override)
$ grep -r "setup_per_cpu_areas" arch/arm64/ # → empty (no override)

# Compare with PowerPC which DOES override:
$ grep -r "setup_per_cpu_areas" arch/powerpc/kernel/setup_64.c
# → arch/powerpc/kernel/setup_64.c:838: void __init setup_per_cpu_areas(void)
```

**ARM32 and ARM64 intentionally rely on the generic implementation** because:
- The generic `pcpu_embed_first_chunk()` algorithm handles NUMA topology correctly
- The architecture's only special work is writing the hardware register (`set_my_cpu_offset`)
- That happens in `smp_prepare_boot_cpu()` — a separate function

---

## SMP Implementation (`mm/percpu.c:3383`)

```c
/**
 * setup_per_cpu_areas - setup percpu areas
 *
 * Allocate percpu areas and setup percpu offset and area variables.
 *
 * RETURNS:
 * Nothing.
 */
void __init setup_per_cpu_areas(void)
{
    unsigned long delta;
    unsigned int cpu;
    int rc;

    /*
     * Always reserve area for module percpu variables.  That's
     * what the legacy allocator did.
     */
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

### Step-by-Step Analysis

#### 1. `pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE, PERCPU_DYNAMIC_RESERVE, PAGE_SIZE, NULL, NULL)`

| Parameter | Value | Meaning |
|---|---|---|
| `reserved_size` | `PERCPU_MODULE_RESERVE` | 8192 bytes for kernel modules |
| `dyn_size` | `PERCPU_DYNAMIC_RESERVE` | 20480 bytes for `alloc_percpu()` |
| `atom_size` | `PAGE_SIZE` | Alignment granularity (4KB) |
| `cpu_distance_fn` | `NULL` | Use default NUMA distance |
| `alloc_fn` | `NULL` | Use default `memblock_alloc` |

When `cpu_distance_fn` is NULL, `pcpu_build_alloc_info()` falls back to
`numa_distance()` for grouping — or a single group if NUMA is not configured.

The function allocates all per-CPU memory, initializes all units, and sets up
internal per-CPU chunk management structures.

On success, sets the global: `pcpu_base_addr` = address of the first byte of per-CPU
memory.

#### 2. `delta = pcpu_base_addr - __per_cpu_start`

```
pcpu_base_addr  = virtual address of allocated per-CPU memory base
                  (set inside pcpu_setup_first_chunk)

__per_cpu_start = virtual address of .data..percpu section in the kernel image
                  (linker-defined symbol, compile-time constant)

delta = relocation needed to go from template address to per-CPU memory
```

`delta` is the **common component** shared by all CPUs. It represents "how far the
allocated memory is from the template section." Every CPU's individual offset adds
its own intra-unit offset on top of this delta.

#### 3. `__per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu]`

```c
for_each_possible_cpu(cpu)
    __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
```

`pcpu_unit_offsets[cpu]` was set by `pcpu_setup_first_chunk()`:
```
pcpu_unit_offsets[cpu] = group[cpu].base_offset + (unit_index_within_group * unit_size)
```

So the final formula is:
```
__per_cpu_offset[cpu] = (pcpu_base_addr - __per_cpu_start)
                      + (group_base_offset + unit_index * unit_size)
                      = (CPU's unit base address) - __per_cpu_start
```

---

## UP Implementation (`mm/percpu.c:3413`)

```c
#ifndef CONFIG_SMP
void __init setup_per_cpu_areas(void)
{
    /*
     * Up percpu area can use PERCPU_ENOUGH_ROOM because the static
     * percpu area already reserved the room for modules.
     */
    unsigned long delta;
    unsigned int cpu;
    int rc;

    rc = pcpu_embed_first_chunk(0, PERCPU_DYNAMIC_RESERVE, PAGE_SIZE,
                                NULL, NULL);
    if (rc < 0)
        panic("Failed to initialize percpu areas.");

    delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
    for_each_possible_cpu(cpu)
        __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
}
#endif
```

Key difference from SMP:
- `reserved_size = 0` (no module reserve — UP kernels rarely need loadable modules
  with per-CPU vars, and the static section already has space)
- Otherwise identical structure
- Since there's only one CPU, `__per_cpu_offset[0]` is the only value set

On true UP kernels where the per-CPU section isn't relocated at all:
```c
/* If pcpu_embed_first_chunk placed memory right at __per_cpu_start: */
delta = 0
__per_cpu_offset[0] = 0
/* this_cpu_read(var) == *(&var) — direct access */
```

---

## Constants Referenced

```c
/* mm/percpu.c */
#define PERCPU_MODULE_RESERVE       (8 << 10)   /* 8KB for kernel modules */
#define PERCPU_DYNAMIC_RESERVE      (20 << 10)  /* 20KB for alloc_percpu */
```

```c
/* include/linux/percpu.h */
#define PCPU_MIN_UNIT_SIZE          SZ_32K      /* 32KB minimum unit */
#define PCPU_MIN_ALLOC_SHIFT        3           /* minimum alloc = 8 bytes */
#define PCPU_MIN_ALLOC_SIZE         (1 << PCPU_MIN_ALLOC_SHIFT)
```

---

## Return Value and Error Handling

```c
rc = pcpu_embed_first_chunk(...);
if (rc < 0)
    panic("Failed to initialize percpu areas.");
```

On failure, the kernel panics immediately. Per-CPU areas are **not optional** —
without them, the kernel cannot function. Failure typically indicates:
- Insufficient physically contiguous memory in bootmem
- Very fragmented early memory (unlikely since we're in early boot)

---

## Global Variables Set by This Function

After `setup_per_cpu_areas()` returns:

| Variable | Set By | Value |
|---|---|---|
| `pcpu_base_addr` | `pcpu_setup_first_chunk()` | Lowest address of per-CPU memory |
| `pcpu_unit_offsets[cpu]` | `pcpu_setup_first_chunk()` | Per-CPU unit offsets from pcpu_base_addr |
| `__per_cpu_offset[cpu]` | `setup_per_cpu_areas()` directly | Runtime per-CPU offsets (used by per_cpu()) |
| `pcpu_first_chunk` | `pcpu_setup_first_chunk()` | Pointer to dynamic region chunk |
| `pcpu_reserved_chunk` | `pcpu_setup_first_chunk()` | Pointer to static+reserved chunk |
| `pcpu_nr_units` | `pcpu_setup_first_chunk()` | Total number of CPU units |

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| File and line for generic SMP implementation? | `mm/percpu.c:3383` |
| Does ARM32 override this? | No — uses this generic function |
| Does ARM64 override this? | No — uses this generic function |
| Which architecture DOES override it? | PowerPC (`arch/powerpc/kernel/setup_64.c:838`) |
| What are the three arguments to embed? | reserved_size (8KB), dyn_size (20KB), PAGE_SIZE |
| What is delta? | `pcpu_base_addr - __per_cpu_start` |
| What formula sets `__per_cpu_offset[cpu]`? | `delta + pcpu_unit_offsets[cpu]` |
| What happens if it fails? | `panic()` — per-CPU is non-optional |
