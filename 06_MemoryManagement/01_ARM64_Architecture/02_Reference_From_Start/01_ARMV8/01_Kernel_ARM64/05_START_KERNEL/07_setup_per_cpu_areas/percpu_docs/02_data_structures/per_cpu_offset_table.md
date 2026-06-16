# `__per_cpu_offset[]` — The Per-CPU Offset Table

## Source Reference
- `include/asm-generic/percpu.h:19` — declaration
- `mm/percpu.c:3400` — population loop in `setup_per_cpu_areas()`
- `arch/arm/include/asm/percpu.h:27` — ARM32 uses it via `__my_cpu_offset`
- `arch/arm64/include/asm/percpu.h:32` — ARM64 uses it via `__kern_my_cpu_offset()`

---

## Declaration

```c
/* include/asm-generic/percpu.h:19 */
extern unsigned long __per_cpu_offset[NR_CPUS];

/* Accessor macro */
#define per_cpu_offset(x)    (__per_cpu_offset[x])
```

---

## Purpose

`__per_cpu_offset[N]` holds the **relocation delta** for CPU N:

```
__per_cpu_offset[N] = (virtual address of CPU N's per-CPU unit)
                    - (virtual address of .data..percpu section template)
```

When accessing per-CPU variable `var` on CPU N:
```c
per_cpu(var, N) = *(typeof(var) *)((char *)&var + __per_cpu_offset[N])
```

`&var` points into the **template** (in .data..percpu).  
Adding `__per_cpu_offset[N]` redirects the pointer to **CPU N's private copy**.

---

## Population: Where Values Come From

`setup_per_cpu_areas()` in `mm/percpu.c:3383` populates the table:

```c
/* mm/percpu.c ~3400 (SMP path) */
void __init setup_per_cpu_areas(void)
{
    unsigned long delta;
    unsigned int cpu;
    int rc;

    /* Step 1: Allocate and initialize all CPU units */
    rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
                                PERCPU_DYNAMIC_RESERVE,
                                PAGE_SIZE, NULL, NULL);

    /* Step 2: Compute the delta between allocated base and template */
    delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
    //       ^^^^ set by pcpu_setup_first_chunk()    ^^^^ kernel linker symbol

    /* Step 3: Populate __per_cpu_offset for every possible CPU */
    for_each_possible_cpu(cpu)
        __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
        //                      ^^^^^   ^^^^^^^^^^^^^^^^^^^^^^^
        //                      common  per-cpu unit's own offset
        //                      base    from pcpu_base_addr
}
```

### The Delta Formula Broken Down

```
delta = pcpu_base_addr - __per_cpu_start

Where:
  pcpu_base_addr = lowest address of any allocated per-CPU unit
                  (set inside pcpu_setup_first_chunk)
  __per_cpu_start = kernel image virtual address of .data..percpu start
                   (linker symbol, compile-time constant)

So for CPU N:
  __per_cpu_offset[N] = delta + pcpu_unit_offsets[N]
                      = (pcpu_base_addr - __per_cpu_start)
                        + (offset of CPU N's unit from pcpu_base_addr)
                      = (CPU N's unit base) - __per_cpu_start
```

This is precisely the number of bytes to **add** to any template address to get CPU N's
private copy address.

---

## Concrete 4-CPU Numeric Example

### Setup

```
Kernel image (compile-time constants):
  __per_cpu_start = 0xC0800000  (virtual address)
  __per_cpu_end   = 0xC0880000  (virtual address, so static_size = 512KB)
  __per_cpu_load  = 0x40800000  (physical load address)

  unit_size = 0x90000  (= 512KB static + 8KB reserved + 20KB dyn, rounded up)

memblock_alloc gives contiguous memory starting at physical 0x42000000:
  Physical: 0x42000000  →  Virtual: 0xC2000000  (4-CPU area)

pcpu_base_addr = 0xC2000000  (set by pcpu_setup_first_chunk)
```

### pcpu_unit_offsets[] (set by pcpu_setup_first_chunk)

```
pcpu_unit_offsets[0] = 0x00000  (CPU 0 at pcpu_base_addr + 0)
pcpu_unit_offsets[1] = 0x90000  (CPU 1 at pcpu_base_addr + unit_size)
pcpu_unit_offsets[2] = 0x120000 (CPU 2 at pcpu_base_addr + 2*unit_size)
pcpu_unit_offsets[3] = 0x1B0000 (CPU 3 at pcpu_base_addr + 3*unit_size)
```

### delta computation

```
delta = pcpu_base_addr - __per_cpu_start
      = 0xC2000000    - 0xC0800000
      = 0x01800000
```

### __per_cpu_offset[] values

```
__per_cpu_offset[0] = 0x01800000 + 0x000000 = 0x01800000
__per_cpu_offset[1] = 0x01800000 + 0x090000 = 0x01890000
__per_cpu_offset[2] = 0x01800000 + 0x120000 = 0x01920000
__per_cpu_offset[3] = 0x01800000 + 0x1B0000 = 0x019B0000
```

### Verification

```
For CPU 2, template variable 'var' at __per_cpu_start + 0x1000:
  &var = 0xC0800000 + 0x1000 = 0xC0801000

  per_cpu(var, 2) address = &var + __per_cpu_offset[2]
                          = 0xC0801000 + 0x01920000
                          = 0xC2121000

  CPU 2 unit base          = pcpu_base_addr + pcpu_unit_offsets[2]
                           = 0xC2000000 + 0x120000
                           = 0xC2120000

  CPU 2's copy of var at   = 0xC2120000 + 0x1000 = 0xC2121000  ✓ matches
```

---

## The Hardware Register Shortcut

Loading `__per_cpu_offset[current_cpu]` on every `this_cpu_*` access would require:
1. Call `raw_smp_processor_id()` — reads another register or global
2. Index into `__per_cpu_offset[]` — one load from memory

This two-instruction overhead is avoided by storing `__per_cpu_offset[N]` directly
in a dedicated hardware register on each CPU:

```
ARM32: TPIDRPRW = __per_cpu_offset[N]   (written by set_my_cpu_offset())
ARM64: tpidr_el1 = __per_cpu_offset[N]  (written by set_my_cpu_offset())
```

With the register already holding the offset, `this_cpu_read(var)` needs only:
```asm
mrs x0, tpidr_el1          ; get __per_cpu_offset[current_cpu] (1 instr)
ldr w1, [x0, #<var_off>]   ; access var (1 instr)
```

---

## UP (Uniprocessor) Special Case

```c
/* mm/percpu.c:3413 — UP path */
#ifndef CONFIG_SMP
void __init setup_per_cpu_areas(void)
{
    /* Only one CPU, offset is always 0 */
    __per_cpu_offset[0] = 0;
    pcpu_unit_offsets[0] = 0;
    /* per-CPU vars ARE the template vars — no copy needed */
}
#endif
```

On UP, `per_cpu(var, 0)` == `var` — adding offset 0 changes nothing. The compiler
can optimize `this_cpu_read(var)` to a direct memory access of the template variable.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What does `__per_cpu_offset[N]` store? | (CPU N unit address) minus (template start address) |
| Where is it declared? | `include/asm-generic/percpu.h:19` |
| Where is it populated? | `setup_per_cpu_areas()` in `mm/percpu.c:3400` |
| What is `delta`? | `pcpu_base_addr - __per_cpu_start` — common component of all offsets |
| What is `pcpu_unit_offsets[cpu]`? | Per-CPU additional offset from pcpu_base_addr |
| Why store offset in a hardware register? | Eliminates array lookup on every `this_cpu_*` access |
| What is the value on UP? | Always 0 for CPU 0 |
| Is this array modified after boot? | No — read-only after `setup_per_cpu_areas()` completes |
