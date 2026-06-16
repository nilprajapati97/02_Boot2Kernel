# smp_setup_block — Detailed Design Bottom-To-Top Flow

## 1. The SMP Setup Block in setup_arch()

```c
#ifdef CONFIG_SMP
if (is_smp()) {
    smp_set_ops(mdesc->smp);
    smp_init_cpus();
    smp_build_mpidr_hash();
}
#endif
```

This three-function block configures the SMP subsystem — selecting which mechanism brings secondary CPUs online, discovering all CPUs, and building a hash table for fast CPU identification.

---

## 2. is_smp(): Checking for SMP Capability

```c
/* arch/arm/include/asm/smp_plat.h */
static inline bool is_smp(void)
{
#ifndef CONFIG_SMP
    return false;
#else
    return smp_get_ops() != NULL || 
           (read_cpuid_mpidr() & MPIDR_UP_BITMASK) != MPIDR_UP_VALUE;
    /* MPIDR UP bit: 1 = uniprocessor variant, 0 = multiprocessor */
#endif
}
```

On uniprocessor (UP) ARM builds, `is_smp()` returns false and the entire block is skipped. On SMP builds, the MPIDR register tells us if this is actually an MP-capable CPU.

---

## 3. smp_set_ops(): Selecting the SMP Operations

```c
/* arch/arm/kernel/smp.c */
void __init smp_set_ops(const struct smp_operations *ops)
{
    if (ops)
        smp_ops = *ops;
}
```

The `smp_ops` global holds the function pointers for SMP operations:

```c
struct smp_operations {
    void (*smp_init_cpus)(void);
    void (*smp_prepare_cpus)(unsigned int max_cpus);
    void (*smp_secondary_init)(unsigned int cpu);
    int  (*smp_boot_secondary)(unsigned int cpu, struct task_struct *idle);
#ifdef CONFIG_HOTPLUG_CPU
    int  (*cpu_kill)(unsigned int cpu);
    void (*cpu_die)(unsigned int cpu);
    bool (*cpu_can_disable)(unsigned int cpu);
    int  (*cpu_disable)(unsigned int cpu);
#endif
};
```

The `smp_boot_secondary()` function is the most critical — it actually starts a secondary CPU core.

---

## 4. PSCI smp_operations

If `psci_dt_init()` succeeded (PSCI available), the smp_ops use PSCI:

```c
/* arch/arm/include/asm/smp_plat.h */
extern const struct smp_operations psci_smp_ops;

/* drivers/firmware/psci/psci_arm.c */
const struct smp_operations psci_smp_ops __initdata = {
    .smp_boot_secondary = psci_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
    .cpu_die     = psci_cpu_die,
    .cpu_kill    = psci_cpu_kill,
#endif
};
```

If PSCI is unavailable, `mdesc->smp` is used (board-specific platform operations).

---

## 5. smp_init_cpus(): Discovering Secondary CPUs

```c
/* arch/arm/kernel/smp.c */
void __init smp_init_cpus(void)
{
    if (smp_ops.smp_init_cpus)
        smp_ops.smp_init_cpus();
}
```

For PSCI:

```c
/* Actually called from smp_prepare_cpus() which is called later */
/* But arm_dt_init_cpu_maps() already marked possible CPUs */
```

For platform-specific (non-PSCI) boards:

```c
/* Example: OMAP */
void __init omap4_smp_init_cpus(void)
{
    unsigned int i, ncores;

    ncores = scu_get_core_count(scu_base);
    for (i = 0; i < ncores && i < NR_CPUS; i++)
        set_cpu_possible(i, true);
}
```

Platform `smp_init_cpus()` probes hardware (SCU — Snoop Control Unit on Cortex-A9) to count cores, then marks them possible.

---

## 6. smp_build_mpidr_hash(): Fast CPU Lookup

After `smp_init_cpus()`, `smp_build_mpidr_hash()` creates a compact hash table:

```c
/* arch/arm/kernel/smp.c */
void __init smp_build_mpidr_hash(void)
{
    struct mpidr_hash *hash = &mpidr_hash;
    u32 i, affinity, fs[3], bits[3];
    u32 mask = 0;

    /* Find which bits vary across CPUs */
    for_each_possible_cpu(i) {
        affinity = cpu_logical_map(i);
        mask |= affinity;  /* bits that are set in any CPU's MPIDR */
    }

    /* For each affinity level, find the first set bit and bit width */
    /* shift_aff[0] = shift to align Aff0 to bit 0 */
    /* shift_aff[1] = shift to align Aff1 to bit 0 */
    /* shift_aff[2] = shift to align Aff2 to bit 0 */
    /* bits[0,1,2] = number of bits used at each affinity level */

    hash->shift_aff[0] = MPIDR_LEVEL_SHIFT(0);
    hash->shift_aff[1] = MPIDR_LEVEL_SHIFT(1);
    hash->shift_aff[2] = MPIDR_LEVEL_SHIFT(2);
    hash->bits[0] = bits[0];
    hash->bits[1] = bits[1];
    hash->bits[2] = bits[2];
    hash->l1_shift = bits[0];
    hash->l2_shift = bits[0] + bits[1];
    hash->mask = mask;
}
```

The hash is used in `__mpidr_hash_get()`:

```c
static inline u32 mpidr_hash_entry(u32 mpidr)
{
    u32 l = (mpidr & 0xFF) >> mpidr_hash.shift_aff[0];
    u32 m = ((mpidr >> 8) & 0xFF) >> mpidr_hash.shift_aff[1];
    u32 h = ((mpidr >> 16) & 0xFF) >> mpidr_hash.shift_aff[2];
    return (h << mpidr_hash.l2_shift) | (m << mpidr_hash.l1_shift) | l;
}
```

This converts an MPIDR to a dense array index, enabling O(1) MPIDR→CPU lookup.

---

## 7. Interview Q&A

**Q1: Why does smp_set_ops() come before smp_init_cpus()? Can they be in any order?**
> `smp_init_cpus()` calls `smp_ops.smp_init_cpus()` — the function pointer. If `smp_set_ops()` hasn't run, `smp_ops.smp_init_cpus` is NULL and the call does nothing (NULL check). So `smp_set_ops()` MUST precede `smp_init_cpus()`. However, for PSCI-based systems, `smp_init_cpus()` is essentially a no-op because `arm_dt_init_cpu_maps()` already filled `cpu_logical_map[]` and set `cpu_possible_mask`. The actual secondary CPU bringup happens later in `smp_prepare_cpus()` and `cpu_up()` during `kernel_init()`.

**Q2: What is the difference between smp_init_cpus() (at boot) and cpu_up() (hotplug)?**
> `smp_init_cpus()` runs during `setup_arch()` — it's architecture-specific discovery code that identifies which CPUs exist and marks them in `cpu_possible_mask` and `cpu_present_mask`. No CPUs are actually started here. `cpu_up(cpu)` is called later (from `smp_init()` in `kernel_init()`) and performs the actual secondary CPU bringup: allocates idle thread, sets up CPU's page tables, calls `smp_ops.smp_boot_secondary()` → PSCI CPU_ON → secondary CPU starts running. CPU hotplug (runtime) also uses `cpu_up()` — same path as boot bringup but from a later starting point.

**Q3: What is mpidr_hash used for at runtime (not just at boot)?**
> `mpidr_hash` is used in the IPI (Inter-Processor Interrupt) fast path. When CPU 0 sends an IPI to CPU 2, the GIC needs to know the MPIDR of CPU 2 to target the interrupt. `cpuid_to_hwid_mpidr(2)` uses the hash table for O(1) lookup: `mpidr_hash_entry(cpu_logical_map[2])`. Without the hash, the kernel would need a linear scan of `cpu_logical_map[]` to find the CPU with that index — O(NR_CPUS) on every IPI. On a 32-CPU system sending hundreds of IPIs per second (scheduler load balancing, TLB flushes), this O(1) vs O(N) difference matters.
