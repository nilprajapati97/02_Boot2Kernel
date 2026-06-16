# ARM64 `setup_per_cpu_areas()` Call Flow

## Architecture: AArch64 (ARM64), ARMv8-A / ARMv8.1-A and later
## Key Registers: `tpidr_el1` (non-VHE) / `tpidr_el2` (VHE / KVM host)

---

## ARM64-Specific Context

ARM64 **does NOT** define its own `setup_per_cpu_areas()`. The function used is the
**generic SMP implementation** from `mm/percpu.c:3383`.

The ARM64-specific per-CPU handling is:
1. `arch/arm64/include/asm/percpu.h` — ALTERNATIVE-based register access macros
2. `arch/arm64/kernel/smp.c:456` — `smp_prepare_boot_cpu()` writes `tpidr_el1` for CPU0
3. `arch/arm64/kernel/smp.c:203` — `secondary_start_kernel()` writes register per CPU
4. `apply_alternatives()` / `apply_boot_alternatives()` — patch VHE instruction variants

---

## Complete ARM64 Per-CPU Initialization Flow

```
arch/arm64/kernel/head.S
│   CPU0 starts at kernel entry point (EL2 or EL1)
│   MMU off initially; enabled early in head.S
│
▼
__primary_switched() [arch/arm64/kernel/head.S]
│   BSS cleared, kernel image mapped
│   C environment ready
│
▼
start_kernel() [init/main.c:854]
│
├─ setup_arch(&cmd_line) [arch/arm64/kernel/setup.c]
│   │   ARM64-specific setup:
│   │   - CPU feature detection (mrs ID_AA64MMFR0_EL1 etc.)
│   │   - paging_init(): set up page tables, configure TCR_EL1
│   │   - apply_boot_alternatives():   ← IMPORTANT for VHE
│   │   │   Scans __alt_instructions table
│   │   │   If ARM64_HAS_VIRT_HOST_EXTN (VHE feature):
│   │   │     Patches: msr tpidr_el1 → msr tpidr_el2  in all alt sites
│   │   │     Patches: mrs tpidr_el1 → mrs tpidr_el2  in all alt sites
│   │   │
│   │   └─ cpu_read_bootcpu_ops() sets up CPU operations table
│
├─ setup_nr_cpu_ids() [kernel/smp.c]
│
├─ setup_per_cpu_areas() [mm/percpu.c:3383]
│   │   ← Generic function, no ARM64 override
│   │
│   ├─ pcpu_embed_first_chunk(8192, 20480, PAGE_SIZE, NULL, NULL)
│   │   │
│   │   ├─ pcpu_build_alloc_info()
│   │   │   static_size = __per_cpu_end - __per_cpu_start
│   │   │   ARM64 typical: 256KB – 1MB (more features = larger)
│   │   │   NUMA grouping: may have multiple groups on server-class ARM64
│   │   │   (Cavium ThunderX2, Ampere Altra have NUMA topologies)
│   │   │
│   │   ├─ memblock_alloc_try_nid() per group
│   │   │   ARM64: allocates from ZONE_DMA or ZONE_NORMAL depending on
│   │   │   MAX_DMA_ADDRESS and physical memory layout
│   │   │
│   │   ├─ memcpy(unit_virt, __per_cpu_load, static_size) per CPU
│   │   │   __per_cpu_load == __per_cpu_start on ARM64 (no XIP)
│   │   │   Both point to same virtual address in the linked image
│   │   │
│   │   └─ pcpu_setup_first_chunk()
│   │       Sets pcpu_base_addr, pcpu_unit_offsets[], chunk structures
│   │
│   └─ Populate __per_cpu_offset[cpu] for all possible CPUs
│
│   ▲ __per_cpu_offset[] fully populated ▲
│   ▲ tpidr_el1 not yet written! ▲
│
├─ smp_prepare_boot_cpu() [arch/arm64/kernel/smp.c:456]
│   │
│   │   /* arch/arm64/kernel/smp.c:453 comment:
│   │    * "The runtime per-cpu areas have been allocated by
│   │    *  setup_per_cpu_areas(), and CPU0's boot time per-cpu area will
│   │    *  be freed shortly, so we must move over to the runtime per-cpu
│   │    *  area." */
│   │
│   └─ set_my_cpu_offset(per_cpu_offset(smp_processor_id()))
│       │   smp_processor_id() = 0 on boot CPU
│       │
│       └─ arch/arm64/include/asm/percpu.h:15:
│           asm volatile(
│               ALTERNATIVE("msr tpidr_el1, %0",
│                           "msr tpidr_el2, %0",
│                           ARM64_HAS_VIRT_HOST_EXTN)
│               :: "r" (offset)
│           );
│           │   If not VHE: "msr tpidr_el1, X0" executes at runtime
│           │   If VHE:     "msr tpidr_el2, X0" was patched in by apply_boot_alternatives
│           │
│           ▲ CPU0 can now use this_cpu_*() correctly ▲
│
└─ ... rest of start_kernel() ...
    apply_alternatives():   ← for non-boot CPUs (re-applies on hotplug)
    smp_init() → cpu_up() → secondary_start_kernel() per secondary CPU
```

---

## VHE (Virtualization Host Extensions) Path

VHE is ARMv8.1-A feature (`ARM64_HAS_VIRT_HOST_EXTN` capability):

```
Without VHE (standard kernel):
  EL1 (kernel) uses tpidr_el1
  EL2 (hypervisor) separate — tpidr_el2 NOT used by Linux kernel

With VHE (kernel runs at EL2 as KVM host):
  EL2 host kernel uses tpidr_el2
  Reason: tpidr_el1 is now the GUEST's register (saves/restored on VM entry/exit)
  Using tpidr_el1 would corrupt guest state

  ALTERNATIVE() macro patches the instruction at boot:
    "msr tpidr_el1, Xn"  →  "msr tpidr_el2, Xn"
    "mrs Xd, tpidr_el1"  →  "mrs Xd, tpidr_el2"

  The patching happens in apply_boot_alternatives() during setup_arch()
  BEFORE setup_per_cpu_areas() is called, so the patched instructions
  are in place when set_my_cpu_offset() first executes.
```

---

## `__hyp_my_cpu_offset` — Hypervisor EL2 Access

When code runs at EL2 in a true hypervisor context (not VHE):
```c
/* arch/arm64/include/asm/percpu.h:23 */
static inline unsigned long __hyp_my_cpu_offset(void)
{
    /*
     * No "Q" hazard constraint here — at EL2, preemption is always
     * disabled (we're in the hypervisor), so no compiler hazard needed
     */
    return read_sysreg(tpidr_el2);
}
```

This reads `tpidr_el2` directly without the stack hazard constraint because
hypervisor code runs with interrupts and preemption disabled.

---

## Secondary CPU Bring-Up (ARM64)

```
el2_setup() [arch/arm64/kernel/head.S]
│   Secondary CPU assembly; determines VHE or non-VHE mode
│
▼
secondary_startup() [arch/arm64/kernel/head.S]
│
▼
secondary_start_kernel() [arch/arm64/kernel/smp.c:203]
│
├─ cpu = smp_processor_id()
│
├─ /* apply_alternatives_all() — re-apply patches on this CPU */
│   Each CPU must apply alternatives independently (patches are per-core)
│   apply_alternatives_vdso() also called here
│
├─ set_my_cpu_offset(__per_cpu_offset[cpu])
│   └─ ALTERNATIVE("msr tpidr_el1, X0",
│                  "msr tpidr_el2, X0",
│                  ARM64_HAS_VIRT_HOST_EXTN)
│       ▲ This CPU's per-CPU access now works ▲
│
├─ notify_cpu_starting(cpu)
├─ calibrate_delay()
└─ cpu_startup_entry(CPUHP_AP_ONLINE_IDLE)
```

---

## ARM64 `this_cpu_read()` Assembly Path

```c
/* arch/arm64/include/asm/percpu.h:32 */
#define __kern_my_cpu_offset ({                             \
    unsigned long off;                                      \
    asm volatile(                                           \
        ALTERNATIVE("mrs %0, tpidr_el1",                   \
                    "mrs %0, tpidr_el2",                    \
                    ARM64_HAS_VIRT_HOST_EXTN)               \
        : "=r" (off)                                        \
        : "Q" (*(unsigned long *)NULL)   /* stack hazard */ \
    );                                                      \
    off;                                                    \
})
```

Assembly output for `this_cpu_read(my_var)` on ARM64 (non-VHE):

```asm
mrs  x0, tpidr_el1               ; get __per_cpu_offset[current_cpu]
add  x0, x0, #<offset_of_my_var> ; add compile-time offset of my_var
ldr  w1, [x0]                    ; load the value (32-bit var example)
; 3 instructions total
```

For VHE (after patching by `apply_boot_alternatives`):
```asm
mrs  x0, tpidr_el2               ; reads tpidr_el2 instead (patched)
add  x0, x0, #<offset_of_my_var>
ldr  w1, [x0]
; Still 3 instructions — just different register
```

---

## The "Q" Stack Hazard Constraint — ARM64

The `"Q" (*(unsigned long *)NULL)` fake dependency in `__kern_my_cpu_offset`:

**Why it's needed:**
```
Consider:
    store_data_to_per_cpu_area();  // stores to *sp etc.
    x = this_cpu_read(var);        // needs offset from tpidr_el1

The compiler might reorder the mrs BEFORE the stores, which would be wrong
if the stores themselves depend on preemption state.

The "Q" constraint (with NULL creating a fake stack dereference) tells the
compiler: "this mrs reads from memory" — preventing it from being hoisted
above memory writes.

This is documented in arch/arm64/include/asm/percpu.h:
  * We want to test the ORIG stack pointer value, and also to serialize against
  * the ALTERNATIVE block for VIRT_HOST_EXTN.
```

---

## ARM64 vs ARM32: Register Comparison

| Aspect | ARM32 | ARM64 |
|---|---|---|
| Register name | TPIDRPRW | tpidr_el1 (or tpidr_el2) |
| CP15 encoding | CRn=c13, opc1=0, CRm=c0, opc2=4 | System register (AArch64) |
| Write instruction | `mcr p15, 0, Rn, c13, c0, 4` | `msr tpidr_el1, Xn` |
| Read instruction | `mrc p15, 0, Rd, c13, c0, 4` | `mrs Xd, tpidr_el1` |
| VHE variant | N/A | `tpidr_el2` (VHE enabled) |
| Patching mechanism | `.alt.smp.init` (UP patching) | `ALTERNATIVE()` + `apply_alternatives` |
| "Q" hazard | Yes, in `__my_cpu_offset` | Yes, in `__kern_my_cpu_offset` |
| User space access | Not possible (PRW = privileged R/W) | Not possible (EL1 register) |
| Banked per-core | Yes | Yes |
| Saved on ctx switch | No (kernel-only, not per-task) | No (kernel-only, not per-task) |

---

## Summary: ARM64 Per-CPU Sequence

1. `setup_arch()` → `apply_boot_alternatives()` patches VHE instructions if needed
2. `setup_per_cpu_areas()` allocates memory, copies template, sets `__per_cpu_offset[]`
3. `smp_prepare_boot_cpu()` → `set_my_cpu_offset()` → `msr tpidr_el1, X0` (or el2)
4. Secondary CPUs: `secondary_start_kernel()` → `apply_alternatives_all()` → `msr`
5. Runtime: `this_cpu_read(var)` → `mrs tpidr_el1` + `add` + `ldr` (3 instructions)
