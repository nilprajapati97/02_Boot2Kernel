# ARM32: No Architecture Override for `setup_per_cpu_areas()`

## Source Reference
- `arch/arm/kernel/setup.c` — ARM32 setup_arch; no setup_per_cpu_areas here
- `arch/arm/mm/init.c` — memory initialization
- `mm/percpu.c:3383` — the function ARM32 actually uses

---

## Verification: ARM32 Uses Generic Path

A search of the entire ARM32 kernel tree confirms no override exists:

```bash
$ grep -r "setup_per_cpu_areas" arch/arm/
# (no results)
```

The only references to per-CPU initialization in ARM32 arch code are in:
- `arch/arm/kernel/smp.c:500` — `smp_prepare_boot_cpu()` (writes TPIDRPRW)
- `arch/arm/kernel/smp.c:410` — `secondary_start_kernel()` (writes TPIDRPRW)
- `arch/arm/include/asm/percpu.h` — the hardware register access macros

---

## Why ARM32 Doesn't Need an Override

### 1. The generic allocator handles everything

`pcpu_embed_first_chunk()` in `mm/percpu.c` is a general-purpose, architecture-agnostic
implementation that:
- Reads `__per_cpu_start` / `__per_cpu_end` linker symbols (provided by ARM32 linker scripts)
- Allocates physically contiguous memory via `memblock_alloc_try_nid()` (works on all archs)
- Copies the template via `memcpy()` (portable)
- Sets `__per_cpu_offset[]` (architecture-agnostic array)

None of these steps require ARM32-specific knowledge.

### 2. ARM32's only arch-specific task is writing the hardware register

ARM32's unique contribution to per-CPU initialization is writing `TPIDRPRW`. But this is:
- Done **after** `setup_per_cpu_areas()` returns (in `smp_prepare_boot_cpu()`)
- A separate concern from the memory allocation
- Simple enough to be a one-liner: `set_my_cpu_offset(per_cpu_offset(cpu))`

### 3. ARM32 is rarely NUMA

Most ARM32 SoCs are single-node (no NUMA). The generic `pcpu_build_alloc_info()` handles
this correctly — it creates a single group with all CPUs. No arch-specific NUMA topology
knowledge is needed.

---

## ARM32 Linker Script: Per-CPU Section

ARM32's per-CPU section is defined in:
```
arch/arm/kernel/vmlinux.lds.S (for ARM32)
```

Which includes the generic per-CPU section macros:
```ld
/* vmlinux.lds.S includes asm-generic/vmlinux.lds.h */
PERCPU_SECTION(L1_CACHE_BYTES)
```

This expands to (from `include/asm-generic/vmlinux.lds.h`):
```ld
.data..percpu : {
    __per_cpu_load = .;
    __per_cpu_start = .;
    *(.data..percpu..first)
    . = ALIGN(PAGE_SIZE);
    *(.data..percpu..page_aligned)
    . = ALIGN(L1_CACHE_BYTES);
    *(.data..percpu..read_mostly)
    . = ALIGN(L1_CACHE_BYTES);
    *(.data..percpu)
    *(.data..percpu..shared_aligned)
    __per_cpu_end = .;
}
```

On ARM32:
- `__per_cpu_load` may differ from `__per_cpu_start` for XIP kernels
- Both are virtual addresses in the kernel's VA space
- The section is physically loaded by the bootloader

---

## ARM32 Memory Map During Per-CPU Setup

```
Kernel virtual address space (ARM32, LPAE disabled):
  3GB user / 1GB kernel split:
  0x00000000 - 0xBFFFFFFF : user space
  0xC0000000 - 0xFFFFFFFF : kernel space

Typical kernel image layout:
  0xC0008000 : _stext (start of kernel code)
  ...
  0xC0800000 : __per_cpu_start  ← start of .data..percpu template
  0xC0880000 : __per_cpu_end    ← end of .data..percpu template (e.g., 512KB)

bootmem allocation for per-CPU:
  0xC2000000 : pcpu_base_addr   ← memblock allocates here for 4 CPUs
               CPU 0: 0xC2000000 - 0xC2090000  (unit_size = 0x90000)
               CPU 1: 0xC2090000 - 0xC2120000
               CPU 2: 0xC2120000 - 0xC21B0000
               CPU 3: 0xC21B0000 - 0xC2240000

After setup_per_cpu_areas():
  delta = 0xC2000000 - 0xC0800000 = 0x01800000
  __per_cpu_offset[0] = 0x01800000
  __per_cpu_offset[1] = 0x01890000
  __per_cpu_offset[2] = 0x01920000
  __per_cpu_offset[3] = 0x019B0000
```

---

## ARM32 Special Case: PERCPU_ENOUGH_ROOM

For ARM32 systems with very small static per-CPU sections, the kernel also supports
an older allocation method. However, modern ARM32 kernels universally use
`pcpu_embed_first_chunk()`.

---

## ARM32-Specific Macros Related to Per-CPU

### `DEFINE_PER_CPU` on ARM32

```c
/* No arch-specific override; uses generic: */
/* include/linux/percpu-defs.h:114 */
#define DEFINE_PER_CPU(type, name)  \
    DEFINE_PER_CPU_SECTION(type, name, "")

/* Ends up in: .data..percpu */
```

### `this_cpu_ptr()` on ARM32

```c
/* include/linux/percpu-defs.h:250 */
#define this_cpu_ptr(ptr)  raw_cpu_ptr(ptr)

/* arch/arm/include/asm/percpu.h */
#define SHIFT_PERCPU_PTR(__p, __offset) \
    RELOC_HIDE((typeof(*(__p)) __kernel __force *)(__p), (__offset))

/* The __offset comes from __my_cpu_offset = mrc p15,0,r0,c13,c0,4 */
```

---

## Summary: What ARM32 Does vs. Doesn't Do for Per-CPU

| Task | ARM32 Involvement | Where |
|---|---|---|
| Allocate per-CPU memory | None — uses generic | `mm/percpu.c:3383` |
| Copy template to units | None — uses generic memcpy | `mm/percpu.c:~3145` |
| Set `__per_cpu_offset[]` | None — computed generically | `mm/percpu.c:~3400` |
| Write hardware register (CPU0) | **YES** | `arch/arm/kernel/smp.c:500` |
| Write hardware register (CPU N) | **YES** | `arch/arm/kernel/smp.c:410` |
| Read hardware register (runtime) | **YES** | `arch/arm/include/asm/percpu.h:27` |
| Define UP patching mechanism | **YES** | `.alt.smp.init` in percpu.h |
