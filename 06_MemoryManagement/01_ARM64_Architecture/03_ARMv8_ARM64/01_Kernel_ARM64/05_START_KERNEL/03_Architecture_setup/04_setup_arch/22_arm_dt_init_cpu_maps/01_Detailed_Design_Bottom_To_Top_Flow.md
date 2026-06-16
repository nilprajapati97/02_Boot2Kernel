# arm_dt_init_cpu_maps() — Detailed Design Bottom-To-Top Flow

## 1. What Does This Function Do?

`arm_dt_init_cpu_maps()` reads the `/cpus` node from the device tree and builds the `cpu_logical_map[]` array — the mapping from logical CPU index (0, 1, 2, 3) to the hardware CPU identifier (MPIDR register value).

Without this mapping, the kernel can't:
- Identify which hardware CPU to bring online for SMP
- Match CPUs in the DT to CPUs in PSCI calls
- Build the MPIDR hash for fast logical-to-hardware lookup

---

## 2. The MPIDR Register

On ARM Cortex-A SoCs, each CPU has a **MPIDR (Multiprocessor Affinity Register)**:

```
MPIDR format (ARM32 Cortex-A7 example):
  Bits [31]: MP flag = 1 (multiprocessor)
  Bits [25:24]: U = 0 (part of cluster), MT = 0 (no SMT)
  Bits [23:16]: Aff2 = cluster of clusters (0 for most)
  Bits [15:8]:  Aff1 = cluster ID (e.g., 0 for cluster 0)
  Bits [7:0]:   Aff0 = CPU ID within cluster (0,1,2,3)

Raspberry Pi 2 (BCM2836, 4x Cortex-A7):
  CPU0: MPIDR = 0x80000000  (Aff0=0)
  CPU1: MPIDR = 0x80000001  (Aff0=1)
  CPU2: MPIDR = 0x80000002  (Aff0=2)
  CPU3: MPIDR = 0x80000003  (Aff0=3)
```

ARM32 uses MPIDR[23:0] (24-bit) as the CPU identifier in SMP operations.

---

## 3. Source Code: arm_dt_init_cpu_maps()

**File:** `arch/arm/kernel/devtree.c`

```c
void __init arm_dt_init_cpu_maps(void)
{
    struct device_node *cpus = of_find_node_by_path("/cpus");
    struct device_node *cpu;
    unsigned int i = 0, cpuidx = 1;
    u32 mpidr;

    if (!cpus)
        return;

    /* Boot CPU (logical 0) has its MPIDR from read_cpuid_mpidr() */
    cpu_logical_map(0) = mpidr_fixup(read_cpuid_mpidr() & MPIDR_HWID_BITMASK);

    /* Iterate all cpu@N nodes */
    for_each_child_of_node(cpus, cpu) {
        const char *device_type;
        u32 hwid;

        /* Only process nodes with device_type = "cpu" */
        if (of_property_read_string(cpu, "device_type", &device_type) ||
            strcmp(device_type, "cpu"))
            continue;

        /* Read the "reg" property — this is the MPIDR/hardware CPU ID */
        if (of_property_read_u32(cpu, "reg", &hwid))
            continue;

        /* Skip the boot CPU (already in logical slot 0) */
        if (hwid == cpu_logical_map(0)) {
            set_cpu_possible(0, true);
            continue;
        }

        cpu_logical_map(cpuidx) = hwid;
        set_cpu_possible(cpuidx, true);
        cpuidx++;
    }

    of_node_put(cpus);
}
```

---

## 4. cpu_logical_map[] Array

```c
/* arch/arm/include/asm/smp_plat.h */
extern u32 __cpu_logical_map[];
#define cpu_logical_map(cpu) __cpu_logical_map[cpu]
```

After `arm_dt_init_cpu_maps()`:

```
cpu_logical_map[0] = 0x00000000  /* boot CPU */
cpu_logical_map[1] = 0x00000001
cpu_logical_map[2] = 0x00000002
cpu_logical_map[3] = 0x00000003
```

On a big.LITTLE system (Cortex-A15 + Cortex-A7):

```
/* big.LITTLE: A15 cluster (cluster 1) + A7 cluster (cluster 0) */
DT:
  cpu@0   reg = <0x000>  ← A7, cluster 0, cpu 0
  cpu@1   reg = <0x001>  ← A7, cluster 0, cpu 1
  cpu@100 reg = <0x100>  ← A15, cluster 1, cpu 0
  cpu@101 reg = <0x101>  ← A15, cluster 1, cpu 1

cpu_logical_map[0] = 0x000  (boot: A7 cluster 0, cpu 0)
cpu_logical_map[1] = 0x001
cpu_logical_map[2] = 0x100
cpu_logical_map[3] = 0x101
```

The logical CPU index (0-3) maps to MPIDR values that span different clusters. Without `cpu_logical_map[]`, the SMP bringup code couldn't know which MPIDR to use for each logical CPU.

---

## 5. DT cpu node format

```
cpus {
    #address-cells = <1>;
    #size-cells = <0>;

    cpu@0 {
        device_type = "cpu";
        compatible = "arm,cortex-a7";
        reg = <0x0>;             ← MPIDR[23:0] on ARM32
        enable-method = "psci";
    };
    cpu@1 {
        device_type = "cpu";
        compatible = "arm,cortex-a7";
        reg = <0x1>;
        enable-method = "psci";
    };
};
```

---

## 6. set_cpu_possible() — The cpumask

After `arm_dt_init_cpu_maps()`:

```c
set_cpu_possible(cpuidx, true);
```

This marks each discovered CPU in the `cpu_possible_mask` cpumask. The kernel uses three CPU masks:
- `cpu_possible_mask` — CPUs that could theoretically be brought online
- `cpu_present_mask` — CPUs detected in hardware (set during `smp_init_cpus()`)
- `cpu_online_mask` — CPUs currently running

`arm_dt_init_cpu_maps()` populates the `possible` mask from DT. `smp_init_cpus()` refines to `present`. `cpu_up()` flips `online`.

---

## 7. Interview Q&A

**Q1: What happens if the DT has 4 CPU nodes but the SoC only has 2 CPUs online?**
> `arm_dt_init_cpu_maps()` marks all 4 as `possible` (in `cpu_possible_mask`). During `smp_init_cpus()`, the kernel tries to probe each MPIDR via PSCI (calls PSCI `CPU_ON` for secondary CPUs). If a CPU doesn't respond or PSCI returns an error, that CPU is not added to `cpu_present_mask`. `cpu_up()` won't try to bring up non-present CPUs. The practical effect: in `/sys/devices/system/cpu/`, cpu2 and cpu3 directories may appear (they're possible) but remain offline and cannot be onlined — `echo 1 > /sys/devices/system/cpu/cpu2/online` fails with ENODEV.

**Q2: What does mpidr_fixup() do?**
> `read_cpuid_mpidr()` reads the MPIDR register and returns a 32-bit value with the hardware affinity bits. `mpidr_fixup()` masks out the upper non-affinity bits (the U/MT/MP flags in bits 31, 30, 24). After masking with `MPIDR_HWID_BITMASK = 0x00FFFFFF`, only the 24-bit hardware ID remains. This allows direct comparison: the DT `reg` property contains the same 24-bit MPIDR value, so `cpu_logical_map[0] == hwid` in the DT means "this is the boot CPU."

**Q3: Why must arm_dt_init_cpu_maps() run after unflatten_device_tree() but before smp_init_cpus()?**
> `arm_dt_init_cpu_maps()` uses `of_find_node_by_path("/cpus")` — requires the unflattened `device_node` tree (set up by `unflatten_device_tree()`). `smp_init_cpus()` uses `cpu_logical_map[]` to know which MPIDRs to bring online — requires the map built here. So the ordering is forced: unflatten → cpu_maps → smp_init_cpus. If `cpu_logical_map[]` were empty when `smp_init_cpus()` runs, it wouldn't know any secondary CPU MPIDRs and would bring up zero secondary CPUs (single-CPU boot).
