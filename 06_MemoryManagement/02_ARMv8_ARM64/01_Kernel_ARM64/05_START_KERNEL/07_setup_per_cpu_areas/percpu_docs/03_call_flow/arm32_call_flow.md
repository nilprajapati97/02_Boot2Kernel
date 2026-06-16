# ARM32 `setup_per_cpu_areas()` Call Flow

## Architecture: ARM (32-bit), ARMv6/v7/v7-A
## Key Register: TPIDRPRW — CP15 c13, CRm 0, op2 4

---

## ARM32-Specific Context

ARM32 **does NOT** define its own `setup_per_cpu_areas()`. The function used is the
**generic SMP implementation** from `mm/percpu.c:3383`.

The ARM32-specific per-CPU handling is limited to:
1. `arch/arm/include/asm/percpu.h` — hardware register access macros
2. `arch/arm/kernel/smp.c:500` — `smp_prepare_boot_cpu()` writes TPIDRPRW for CPU0
3. `arch/arm/kernel/smp.c:410` — `secondary_start_kernel()` writes TPIDRPRW for each secondary

---

## Complete ARM32 Per-CPU Initialization Flow

```
arch/arm/kernel/head.S (or head-common.S)
│   CPU0 starts here in physical address space
│   MMU is off, no per-CPU access possible yet
│
▼
__mmap_switched() [arch/arm/kernel/head-common.S]
│   MMU on, virtual addressing active
│   C environment set up
│
▼
start_kernel() [init/main.c:854]
│
├─ setup_arch(&cmd_line) [arch/arm/kernel/setup.c]
│   │   ARM32-specific early setup:
│   │   - Machine type detection
│   │   - Memory map (memblock) initialization
│   │   - Early platform device setup
│   │   - No per-CPU access here (TPIDRPRW not yet written)
│   │
│   └─ paging_init() sets up page tables
│
├─ setup_nr_cpu_ids() [kernel/smp.c]
│   Determines nr_cpu_ids from cpu_possible_mask
│
├─ setup_per_cpu_areas() [mm/percpu.c:3383]
│   │   ← Generic function, no ARM32 override
│   │
│   ├─ pcpu_embed_first_chunk(8192, 20480, PAGE_SIZE, NULL, NULL)
│   │   │
│   │   ├─ pcpu_build_alloc_info()
│   │   │   Computes: static_size = __per_cpu_end - __per_cpu_start
│   │   │   ARM32 typical: 128KB – 512KB
│   │   │   Groups: 1 group (ARM32 systems are rarely NUMA)
│   │   │   upa: typically = 1 or 2 on small ARM SoCs
│   │   │
│   │   ├─ memblock_alloc_try_nid()
│   │   │   Allocates physically contiguous memory for all CPU units
│   │   │   ARM32: typically ZONE_NORMAL or ZONE_DMA memory
│   │   │   Result: contiguous block at some physical address P
│   │   │
│   │   ├─ memcpy(unit_virt, __per_cpu_load, static_size)  ← per CPU
│   │   │   __per_cpu_load is the physical load address of .data..percpu
│   │   │   On ARM32: __per_cpu_load may differ from __per_cpu_start if
│   │   │   the kernel uses XIP (execute-in-place) from flash
│   │   │
│   │   └─ pcpu_setup_first_chunk()
│   │       Sets pcpu_base_addr, pcpu_unit_offsets[], chunk structures
│   │
│   └─ Populate __per_cpu_offset[cpu] for all possible CPUs
│       __per_cpu_offset[cpu] = (pcpu_base_addr - __per_cpu_start)
│                               + pcpu_unit_offsets[cpu]
│
│   ▲ __per_cpu_offset[] fully populated ▲
│   ▲ But TPIDRPRW not yet written! ▲
│
├─ smp_prepare_boot_cpu() [arch/arm/kernel/smp.c:500]
│   │
│   └─ set_my_cpu_offset(per_cpu_offset(smp_processor_id()))
│       │   smp_processor_id() = 0 (we're on the boot CPU)
│       │   per_cpu_offset(0) = __per_cpu_offset[0]
│       │
│       └─ asm volatile("mcr p15, 0, %0, c13, c0, 4" : : "r"(offset))
│           │   Writes offset to TPIDRPRW (Thread Pointer/ID Register, Privileged R/W)
│           │   CP15 register: CRn=c13, opc1=0, CRm=c0, opc2=4
│           │   This register is banked per-core — each core has its own
│           │
│           ▲ CPU0 can now use this_cpu_*() correctly ▲
│
└─ ... rest of start_kernel() ...
    Later: smp_init() brings up secondary CPUs
```

---

## Secondary CPU Bring-Up (ARM32)

```
__secondary_switched() [arch/arm/kernel/head.S]
│   Secondary CPU assembly entry, called from SMP platform code
│
▼
secondary_start_kernel() [arch/arm/kernel/smp.c:410]
│
├─ preempt_disable()
├─ cpu = smp_processor_id()            ← reads MPIDR, computes CPU id
│
├─ set_my_cpu_offset(__per_cpu_offset[cpu])
│   └─ mcr p15, 0, Rn, c13, c0, 4    ← write TPIDRPRW for this core
│       ▲ This secondary CPU's per-CPU access now works ▲
│
├─ cpu_init()                          ← arch CPU initialization
├─ preempt_enable()
├─ notify_cpu_starting(cpu)
├─ complete(&cpu_running)
└─ cpu_startup_entry(CPUHP_AP_ONLINE_IDLE)  ← enter idle loop
```

---

## ARM32 `this_cpu_read()` Assembly Path

When kernel code calls `this_cpu_read(my_var)` on ARM32:

```asm
; Compiler expands this_cpu_read(my_var) to approximately:
;
; Step 1: Read TPIDRPRW into scratch register
mrc   p15, 0, r0, c13, c0, 4   ; r0 = __per_cpu_offset[current_cpu]

; Step 2: Add compile-time constant offset of my_var within the unit
add   r0, r0, #<offset_of_my_var_in_percpu_section>

; Step 3: Load the variable value
ldr   r1, [r0]                  ; r1 = my_var for current CPU

; Total: 3 instructions, no memory table lookup, no branch
```

The `"Q"` constraint in the `mrc` output:
```c
/* arch/arm/include/asm/percpu.h:27 */
static inline unsigned long __my_cpu_offset(void)
{
    unsigned long off;
    asm("mrc p15, 0, %0, c13, c0, 4"
        : "=r" (off)
        : "Q" (*(unsigned long *)NULL)  /* stack pointer hazard marker */
    );
    return off;
}
```
The `"Q"` (or `"m"` in some versions) input constraint on `NULL` is a **fake dependency**
that tells the compiler: "this read depends on memory state." This prevents the compiler
from hoisting the `mrc` instruction above store operations that set up per-CPU data.

---

## ARM32 UP (Uniprocessor) Patching — `.alt.smp.init`

For ARMv6 and earlier cores running a kernel built with `CONFIG_SMP=y` but on a
single-core system, ARM32 uses an **instruction patching mechanism**:

```c
/* arch/arm/include/asm/percpu.h */
#ifdef CONFIG_SMP
static inline unsigned long __my_cpu_offset(void)
{
    unsigned long off;
    asm volatile (
        "mrc p15, 0, %0, c13, c0, 4"
        ...
    );
    return off;
}
#else
/* UP: offset is always 0, compiler can optimize away entirely */
#define __my_cpu_offset  0UL
#endif
```

For ARMv6 SMP kernels running on UP hardware, the `.alt.smp.init` section contains
replacement instructions that are applied at boot by `fixup_smp()`:

```
At boot (smp_init_cpus -> fixup_smp):
  .alt.smp.init patches: mrc → mov r0, #0
  So on UP hardware, __my_cpu_offset always returns 0 (1 instruction instead of 1)
  No functional difference since there's only one CPU
```

---

## ARM32-Specific Memory Considerations

### TPIDRPRW register properties
- Introduced in ARMv6 (CP15 c13 thread registers)
- **Banked**: each CPU core has its own physical copy
- **Privileged R/W**: only accessible in PL1 (kernel mode) — not readable from user space
  (unlike TPIDRURO which is user-readable)
- Not saved/restored on context switch (only used for per-CPU kernel data, not per-task)
- Survives `wfi` (wait for interrupt) power state

### Physical vs virtual address concern
```
On ARM32, __per_cpu_start is a VIRTUAL address.
pcpu_base_addr is a VIRTUAL address.
TPIDRPRW holds a VIRTUAL offset (or absolute virtual address on some configs).

When the per-CPU area is accessed:
  this_cpu_ptr(&var) = &var + TPIDRPRW value
  &var is a virtual address in .data..percpu
  result is a virtual address in the per-CPU area
  Page tables map this virtual range to the allocated physical memory
```

### XIP (Execute-In-Place) kernels
Some embedded ARM32 systems execute from read-only flash memory. In this case:
- `.data..percpu` section is loaded (copied) to RAM at `__per_cpu_load`
- `__per_cpu_start` is the virtual address where it runs
- `memcpy(unit, __per_cpu_load, static_size)` uses the **physical load address** to read
  the template — this is why `__per_cpu_load` exists separately from `__per_cpu_start`

---

## Summary: ARM32 Per-CPU Sequence

1. `setup_per_cpu_areas()` allocates memory, copies template, sets `__per_cpu_offset[]`
2. `smp_prepare_boot_cpu()` → `set_my_cpu_offset()` → `mcr p15,0,r0,c13,c0,4`
3. Secondary CPUs: `secondary_start_kernel()` → `mcr` each CPU's register
4. Runtime: `this_cpu_read(var)` → `mrc p15,0,r0,c13,c0,4` + `add` + `ldr` (3 instructions)
