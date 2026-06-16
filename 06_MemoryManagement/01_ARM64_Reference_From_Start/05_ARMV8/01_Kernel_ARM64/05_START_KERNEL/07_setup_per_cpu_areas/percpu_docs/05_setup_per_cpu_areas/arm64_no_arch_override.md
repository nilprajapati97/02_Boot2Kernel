# ARM64: No Architecture Override for `setup_per_cpu_areas()`

## Source Reference
- `arch/arm64/kernel/setup.c` — ARM64 setup_arch; no setup_per_cpu_areas here
- `arch/arm64/mm/init.c` — memory initialization
- `mm/percpu.c:3383` — the function ARM64 actually uses

---

## Verification: ARM64 Uses Generic Path

```bash
$ grep -r "setup_per_cpu_areas" arch/arm64/
# (no results)
```

ARM64 performs no per-CPU area allocation. All allocation logic lives in `mm/percpu.c`.
The ARM64 arch code only touches per-CPU in:
- `arch/arm64/kernel/smp.c:456` — `smp_prepare_boot_cpu()` (writes `tpidr_el1`)
- `arch/arm64/kernel/smp.c:203` — `secondary_start_kernel()` (writes register per CPU)
- `arch/arm64/include/asm/percpu.h` — `ALTERNATIVE()`-based macros

---

## Why ARM64 Doesn't Need an Override

### 1. Generic allocator is sufficient

`pcpu_embed_first_chunk()` handles all ARM64 scenarios including:
- Large static per-CPU sections (ARM64 kernels often have 512KB+)
- Multi-node NUMA ARM64 servers (ThunderX2, Altra, Grace)
- Single-node embedded ARM64 SoCs

### 2. VHE patching happens separately

The VHE (Virtualization Host Extensions) register patching — switching from `tpidr_el1`
to `tpidr_el2` — happens via `apply_boot_alternatives()` in `setup_arch()`, **before**
`setup_per_cpu_areas()` is called. By the time per-CPU setup runs, the correct
instructions are already in place.

### 3. Alternatives framework integration

`ALTERNATIVE()` macros in `arch/arm64/include/asm/percpu.h` are handled entirely by
the alternatives patching subsystem. The per-CPU allocator doesn't need to know about
VHE — it just sets `__per_cpu_offset[]`, and the runtime access code uses the
(potentially patched) register instructions.

---

## ARM64 Linker Script: Per-CPU Section

ARM64 uses the same generic per-CPU section via:
```
arch/arm64/kernel/vmlinux.lds.S
```

```ld
/* ARM64 vmlinux.lds.S */
PERCPU_SECTION(L1_CACHE_BYTES)
```

Expanded:
```ld
.data..percpu : AT(ADDR(.data..percpu) - LOAD_OFFSET) {
    __per_cpu_load = .;
    __per_cpu_start = .;
    *(.data..percpu..first)
    . = ALIGN(PAGE_SIZE);
    *(.data..percpu..page_aligned)
    . = ALIGN(L1_CACHE_BYTES)  /* ARM64: L1_CACHE_BYTES = 64 */
    *(.data..percpu..read_mostly)
    . = ALIGN(L1_CACHE_BYTES);
    *(.data..percpu)
    *(.data..percpu..shared_aligned)
    __per_cpu_end = .;
}
```

On ARM64:
- `__per_cpu_load == __per_cpu_start` (no XIP; ARM64 doesn't support execute-in-place)
- Both point to the same virtual address in the kernel image
- L1_CACHE_BYTES = 64 (standard ARM64 cache line size)

---

## ARM64 NUMA Per-CPU Layout

Server-class ARM64 SoCs may have NUMA topology. Example: Cavium ThunderX2 (2 sockets):

```
NUMA topology:
  Node 0: CPUs 0-27  (socket 0)
  Node 1: CPUs 28-55 (socket 1)

pcpu_build_alloc_info() will create 2 groups:
  Group 0: CPUs 0-27,  allocated from Node 0 memory
  Group 1: CPUs 28-55, allocated from Node 1 memory

pcpu_embed_first_chunk():
  memblock_alloc_try_nid(..., nid=0) for Group 0
  memblock_alloc_try_nid(..., nid=1) for Group 1

Result:
  CPU 0-27's per-CPU data is on Node 0 physical memory
  CPU 28-55's per-CPU data is on Node 1 physical memory
  → Local memory access for per-CPU data on all CPUs
```

---

## ARM64 Memory Map During Per-CPU Setup

```
ARM64 kernel virtual address space (48-bit VA, 4KB pages):
  0x0000_0000_0000_0000 - 0x0000_FFFF_FFFF_FFFF : user space
  0xFFFF_0000_0000_0000 - 0xFFFF_FFFF_FFFF_FFFF : kernel space

Typical kernel image layout:
  0xFFFF_8000_1008_0000 : _text (kernel code start, KASLR may shift this)
  ...
  0xFFFF_8000_10A0_0000 : __per_cpu_start
  0xFFFF_8000_1140_0000 : __per_cpu_end   (1MB template, ARM64 has more drivers)

bootmem allocation for per-CPU (4 CPUs):
  0xFFFF_8000_4000_0000 : pcpu_base_addr
               CPU 0: [0xFFFF_8000_4000_0000, 0xFFFF_8000_40F0_0000)
               CPU 1: [0xFFFF_8000_40F0_0000, 0xFFFF_8000_41E0_0000)
               CPU 2: [0xFFFF_8000_41E0_0000, 0xFFFF_8000_42D0_0000)
               CPU 3: [0xFFFF_8000_42D0_0000, 0xFFFF_8000_43C0_0000)
               (unit_size = 0xF00000 = ~1MB)

After setup_per_cpu_areas():
  delta = 0xFFFF_8000_4000_0000 - 0xFFFF_8000_10A0_0000 = 0x2F60_0000

  tpidr_el1 holds this delta + unit offset for the running CPU
```

---

## ARM64 and `__per_cpu_load`

On ARM64, the linker symbol `__per_cpu_load` is **identical** to `__per_cpu_start`:

```c
/* arch/arm64 doesn't use XIP, so: */
__per_cpu_load  == __per_cpu_start   /* same virtual address */
```

The template copy in `pcpu_embed_first_chunk()`:
```c
memcpy(unit_addr, __per_cpu_load, static_size);
/* reads from __per_cpu_start on ARM64 */
/* same as: memcpy(unit_addr, __per_cpu_start, static_size) */
```

---

## ARM64-Specific Considerations for `this_cpu_*`

### Stack hazard and memory ordering

The `"Q"` constraint in `__kern_my_cpu_offset()` ensures correct ordering with respect
to stack operations. This is more critical on ARM64 than ARM32 because:

1. ARM64 has a more aggressive out-of-order execution pipeline
2. The `ALTERNATIVE()` macro itself is an assembler pseudo-instruction that could
   theoretically be reordered
3. VHE systems have an additional complication: `tpidr_el2` behavior around
   EL1↔EL2 transitions (VM entry/exit)

### `raw_cpu_*` vs `this_cpu_*` on ARM64

On ARM64 with CONFIG_PREEMPT:
```c
this_cpu_read(var):
    preempt_disable()        ← acquires preemption lock
    val = raw_cpu_read(var)  ← mrs tpidr_el1 + add + ldr
    preempt_enable()
    return val

/* But in practice, many uses don't need preempt_disable because: */
/* - they're in interrupt/softirq context (preemption already off) */
/* - they use get_cpu_var() which also disables preemption */
/* - they use __this_cpu_read which is explicitly preemption-unsafe */
```

---

## Summary: What ARM64 Does vs. Doesn't Do for Per-CPU

| Task | ARM64 Involvement | Where |
|---|---|---|
| Allocate per-CPU memory | None — uses generic | `mm/percpu.c:3383` |
| Copy template to units | None — uses generic memcpy | `mm/percpu.c:~3145` |
| Set `__per_cpu_offset[]` | None — computed generically | `mm/percpu.c:~3400` |
| VHE instruction patching | **YES** — `apply_boot_alternatives()` | `arch/arm64/kernel/alternative.c` |
| Write hardware register (CPU0) | **YES** | `arch/arm64/kernel/smp.c:456` |
| Write hardware register (CPU N) | **YES** | `arch/arm64/kernel/smp.c:203` |
| Read hardware register (runtime) | **YES** — `ALTERNATIVE()`-based | `arch/arm64/include/asm/percpu.h:32` |
| Hypervisor EL2 offset access | **YES** — `__hyp_my_cpu_offset` | `arch/arm64/include/asm/percpu.h:23` |
