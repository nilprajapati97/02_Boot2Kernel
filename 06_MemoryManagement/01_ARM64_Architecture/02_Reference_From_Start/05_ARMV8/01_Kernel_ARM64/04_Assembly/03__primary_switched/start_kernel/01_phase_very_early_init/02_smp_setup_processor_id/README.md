# `smp_setup_processor_id()` — Processor ID Setup

## Overview

| Attribute    | Value                                              |
|-------------|-----------------------------------------------------|
| **Function** | `smp_setup_processor_id(void)`                     |
| **Source**   | Weak default: `init/main.c`; arch override: `arch/x86/kernel/smpboot.c`, `arch/arm64/kernel/smp.c` |
| **Called**   | Second call in `start_kernel()`                    |
| **Purpose**  | Map the hardware CPU identifier to a logical kernel CPU number; establish the boot CPU's logical ID |

---

## Why It Exists

Modern CPUs identify themselves via hardware IDs:
- **x86**: APIC ID (Local APIC ID from CPUID leaf 1, or x2APIC ID from leaf 0x1F)
- **ARM64**: MPIDR_EL1 register (multi-processor affinity register — encodes cluster, core, thread)
- **RISC-V**: `mhartid` register

The kernel's internal CPU numbering is **logical** (0, 1, 2, ...) and must be mapped to these hardware IDs early. Before this mapping exists, SMP operations (`smp_processor_id()`, per-CPU variables) don't work correctly.

---

## Default Implementation

The default is a **weak no-op**:
```c
// init/main.c
void __init __weak smp_setup_processor_id(void) { }
```

Only architectures that need non-trivial setup override it.

---

## x86 Implementation

On x86, each logical CPU has an APIC ID. The boot CPU reads its own APIC ID from CPUID and stores it:

```c
// arch/x86/kernel/smpboot.c  (called via setup_arch for x86)
// The actual per-CPU APIC ID setup happens in:
// arch/x86/kernel/apic/apic.c → apic_intr_mode_init()

// But smp_setup_processor_id on x86 is typically the default no-op
// because x86 sets up per-CPU IDs during setup_arch
```

On x86 the real work happens in `setup_arch()` → `smp_init_topology()`.

---

## ARM64 Implementation

ARM64 has a substantive implementation:

```c
// arch/arm64/kernel/smp.c
void __init smp_setup_processor_id(void)
{
    u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
    // Map hardware MPIDR to logical CPU 0
    cpu_logical_map(0) = mpidr;

    // Set the boot CPU's MPIDR in the per-CPU array
    // This allows smp_processor_id() to work on the boot CPU
}
```

The `MPIDR_EL1` register on ARM64:
```
Bits [39:32] = Aff3 (affinity level 3, e.g., socket)
Bits [23:16] = Aff2 (affinity level 2, e.g., cluster)
Bits [15:8]  = Aff1 (affinity level 1, e.g., core)
Bits [7:0]   = Aff0 (affinity level 0, e.g., thread within core)
```

Example: Cortex-A55 in a big.LITTLE system might have MPIDR = 0x0000_0001 for the first LITTLE core.

---

## NUMA Topology Relationship

`smp_setup_processor_id()` is also related to **NUMA node assignment**:
- The boot CPU's logical ID (0) is assigned to NUMA node 0
- Other CPUs discovered later are assigned to their respective NUMA nodes based on ACPI/DT topology data

---

## Sub-Topics

- [01_cpu_id_and_apic](01_cpu_id_and_apic/README.md) — Deep dive into APIC IDs, x2APIC, MPIDR, and CPU topology

---

## Interview Q&A

### Q1: What is the difference between physical CPU ID and logical CPU ID?
**A:** Physical CPU ID is the hardware-assigned identifier — APIC ID on x86, MPIDR on ARM. These may not be contiguous (e.g., APIC IDs can be 0, 2, 4, 6 on a 4-core system). Logical CPU IDs are what the kernel uses internally — always 0, 1, 2, ... contiguous. `cpu_logical_map[]` maps logical → physical, `cpu_physical_id(cpu)` returns the physical ID for a logical CPU. This distinction matters for per-CPU variable access (always uses logical ID) vs. APIC programming (uses physical APIC ID).

### Q2: Why must processor ID setup happen before `debug_objects_early_init()`?
**A:** `debug_objects_early_init()` initializes per-CPU debug object hash buckets. Per-CPU data access uses `smp_processor_id()` which requires the logical CPU ID to be valid. If the mapping is not established first, `smp_processor_id()` could return garbage, leading to wrong per-CPU bucket access and potential corruption. The ordering is: set up CPU ID → then use per-CPU data.

### Q3: What happens to secondary CPUs' processor IDs?
**A:** During `smp_init()` (called much later via `kernel_init` → `do_basic_setup` → `do_initcalls`), secondary CPUs are brought out of their INIT/SIPI wait loop. Each secondary CPU calls `start_secondary()` → `smp_store_cpu_info()` which reads its own hardware ID and calls `set_cpu_sibling_map()` to populate the CPU topology. Each gets a unique logical ID assigned by `cpumask_next_zero()` on the `cpu_present_mask`.
