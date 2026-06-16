# arm_dt_init_cpu_maps() — System Design Approach and Q&A

## 1. Why a Mapping Is Needed

Linux uses **logical CPU indices** (0, 1, 2, ...) internally for all CPU operations: `smp_call_function_single(cpu, ...)`, `per_cpu(var, cpu)`, `cpumask_set_cpu(cpu, mask)`. These are dense integers starting at 0, ideal for array indexing.

Hardware uses **MPIDR values** (0x000, 0x001, 0x100, 0x101 for big.LITTLE) — sparse, hardware-defined identifiers. PSCI CPU_ON takes an MPIDR, not a logical index.

The `cpu_logical_map[]` bridges:
```
Logical CPU 0 ─→ MPIDR 0x000 ─→ PSCI CPU_ON(0x000) ─→ Cortex-A7 core 0
Logical CPU 1 ─→ MPIDR 0x001 ─→ PSCI CPU_ON(0x001) ─→ Cortex-A7 core 1
Logical CPU 2 ─→ MPIDR 0x100 ─→ PSCI CPU_ON(0x100) ─→ Cortex-A15 core 0
Logical CPU 3 ─→ MPIDR 0x101 ─→ PSCI CPU_ON(0x101) ─→ Cortex-A15 core 1
```

---

## 2. Dependency Graph

```
[unflatten_device_tree()]
  └── of_root populated → /cpus/cpu@N nodes accessible
        │
[arm_dt_init_cpu_maps()]
  ├── reads /cpus/cpu@N/reg → hwid (MPIDR)
  ├── fills cpu_logical_map[0..N]
  └── set_cpu_possible(cpu, true) for each
        │
[psci_dt_init()]
  └── reads enable-method = "psci" from cpu nodes
  └── registers psci_ops (cpu_on/cpu_off/cpu_suspend)
        │
[smp_set_ops()]
  └── selects SMP operations (PSCI or mdesc->smp)
        │
[smp_init_cpus()]
  └── for each possible CPU: calls smp_ops.smp_init_cpus(cpu_logical_map[cpu])
  └── marks cpu_present_mask
        │
[smp_build_mpidr_hash()]
  └── builds hash table for MPIDR→logical conversion (used in IPI hot path)
```

---

## 3. enable-method in the CPU Node

```
cpu@0 {
    enable-method = "psci";   ← modern ARM: use PSCI firmware calls
    /* or */
    enable-method = "spin-table";  ← older ARM: boot CPU polls a spin table
};
```

`arm_dt_init_cpu_maps()` doesn't process `enable-method` — that's done by `psci_dt_init()`. But both functions read from the same CPU nodes, which is why they run in sequence.

---

## 4. System Design Q&A

**Q: How does the kernel discover which CPU is the boot CPU vs secondary CPUs?**
> The boot CPU (logical index 0) is identified by reading `MPIDR` directly: `read_cpuid_mpidr() & MPIDR_HWID_BITMASK`. This gives the boot CPU's hardware ID. When iterating DT cpu nodes, any node whose `reg` value matches this read MPIDR is the boot CPU — it gets `cpu_logical_map[0]`. All other nodes are secondary CPUs and get logical indices 1, 2, 3, etc. The assignment order (which secondary gets logical 1 vs 2) follows DT node order, which is typically determined by the board DT author.

**Q: What is the relationship between cpu_logical_map[] and /sys/devices/system/cpu/cpu0/?**
> `/sys/devices/system/cpu/cpu0/` corresponds to logical CPU 0. The file `/sys/devices/system/cpu/cpu0/topology/core_id` shows the physical core ID within its cluster (Aff0 value). `/sys/devices/system/cpu/cpu0/topology/cluster_id` shows Aff1. These sysfs files are populated by `store_cpu_topology()` which uses `cpu_logical_map[cpu]` — the MPIDR value built by `arm_dt_init_cpu_maps()`. So this function is the source of truth for all CPU topology information visible to userspace via sysfs.

**Q: What happens on a device that has CPUs not listed in the DT?**
> If a physical CPU core exists in hardware but has no `cpu@N` node in the DT, `arm_dt_init_cpu_maps()` never discovers it. It won't appear in `cpu_possible_mask`, won't be registered, and can never be brought online. The kernel is completely unaware of it. From the kernel's perspective, that CPU doesn't exist. This is intentional: the DT is the authoritative hardware description. A board vendor can deliberately exclude a faulty CPU core by removing it from the DT — the kernel won't try to use it.
