# arm_dt_init_cpu_maps() — ARM32 vs ARM64 Design Details

## 1. The Key Difference: MPIDR Width

| | ARM32 | ARM64 |
|--|-------|-------|
| MPIDR bits used | 24-bit [23:0] | 40-bit [39:0] (Aff3 added) |
| cpu_logical_map type | `u32` | `u64` |
| DT reg property | `<u32>` (1 cell) | `<u64>` as two u32 cells |
| Function name | `arm_dt_init_cpu_maps()` | `init_cpu_topology()` |
| max CPUs (NR_CPUS) | 32 (typical) | 4096 (server-class) |

---

## 2. ARM64 MPIDR: The Extended Format

ARM64 adds **Aff3** (bits [39:32]) for large systems:

```
ARM64 MPIDR_EL1:
  Bits [63:40]: Reserved
  Bits [39:32]: Aff3 — Socket/die (multi-socket servers)
  Bits [31]:    RES1 (always 1)
  Bits [23:16]: Aff2 — Package/node
  Bits [15:8]:  Aff1 — Cluster
  Bits [7:0]:   Aff0 — CPU within cluster

Example: Ampere Altra (80-core ARM64 server)
  CPU 0: MPIDR = 0x0000000080000000 (Aff3=0, Aff2=0, Aff1=0, Aff0=0)
  CPU 1: MPIDR = 0x0000000080000001 (Aff0=1)
  ...
  CPU 63: MPIDR = 0x000000008001003F (Aff2=1, Aff1=0, Aff0=63)
```

DT for ARM64 CPU nodes uses two `#address-cells`:

```
cpus {
    #address-cells = <2>;    /* Two u32 cells = one u64 MPIDR */
    #size-cells = <0>;

    cpu@0 {
        device_type = "cpu";
        compatible = "arm,cortex-a72";
        reg = <0x0 0x0>;     /* 64-bit: Aff3|Aff2 = 0, Aff1|Aff0 = 0 */
        enable-method = "psci";
    };
    cpu@100 {
        device_type = "cpu";
        compatible = "arm,cortex-a72";
        reg = <0x0 0x100>;   /* Aff1=1, Aff0=0 */
        enable-method = "psci";
    };
};
```

---

## 3. ARM64: init_cpu_topology() vs arm_dt_init_cpu_maps()

ARM64 doesn't call `arm_dt_init_cpu_maps()`. Instead it uses:

```c
/* arch/arm64/kernel/topology.c */
void __init init_cpu_topology(void)
{
    reset_cpu_topology();
    if (of_have_populated_dt() && parse_dt_topology())
        return;
    if (acpi_disabled)
        return;
    acpi_parse_srat();  /* SRAT table for NUMA topology */
}
```

ARM64 builds a richer topology: not just logical→MPIDR mapping, but also cluster IDs, NUMA nodes, and SMT thread siblings.

---

## 4. cpu_logical_map: u32 (ARM32) vs u64 (ARM64)

### ARM32

```c
/* arch/arm/include/asm/smp_plat.h */
extern u32 __cpu_logical_map[NR_CPUS];
#define cpu_logical_map(cpu) __cpu_logical_map[cpu]
```

Uses `u32` — sufficient for 24-bit MPIDR on ARM32.

### ARM64

```c
/* arch/arm64/include/asm/smp_plat.h */
extern u64 __cpu_logical_map[NR_CPUS];
#define cpu_logical_map(cpu) __cpu_logical_map[cpu]
```

Uses `u64` — 40-bit MPIDR needs 64-bit storage.

---

## 5. MPIDR Hash: ARM32 vs ARM64

Both ARM32 and ARM64 build a hash table to quickly convert a logical CPU index to MPIDR. This is used in hot paths (IPI sending, power management).

### ARM32: smp_build_mpidr_hash()

```c
/* arch/arm/kernel/smp.c */
void __init smp_build_mpidr_hash(void)
{
    u32 i, affinity, fs[3], bits[3];

    /* Compute shift amounts for Aff0, Aff1, Aff2 */
    for_each_possible_cpu(i) {
        affinity = cpu_logical_map(i);
        fs[0] = ffs(affinity & 0xFF);        /* Aff0 field start */
        fs[1] = ffs((affinity >> 8) & 0xFF); /* Aff1 field start */
        fs[2] = ffs((affinity >> 16) & 0xFF);/* Aff2 field start */
        ...
    }
    mpidr_hash.shift_aff[0] = ...;
    mpidr_hash.bits[0] = ...;
}
```

### ARM64: Similar but extended for 4-level affinity

ARM64 `smp_build_mpidr_hash()` handles Aff3 for large systems. The hash table is larger (up to 4096 CPUs).

---

## 6. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Function | `arm_dt_init_cpu_maps()` | `init_cpu_topology()` |
| MPIDR width | 24-bit (Aff0-Aff2) | 40-bit (Aff0-Aff3) |
| cpu_logical_map type | `u32` | `u64` |
| DT #address-cells | 1 (one u32) | 2 (two u32 = one u64) |
| Max CPUs | 32-64 typical | 4096 possible |
| Topology data | MPIDR only | MPIDR + cluster + NUMA |
| ACPI path | Not supported | SRAT table if ACPI |
| big.LITTLE support | Yes (Aff1 = cluster) | Yes + DynamIQ (Aff2 = cluster) |
