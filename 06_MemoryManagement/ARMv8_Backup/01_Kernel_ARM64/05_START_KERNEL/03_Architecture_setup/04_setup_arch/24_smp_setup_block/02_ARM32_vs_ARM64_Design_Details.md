# smp_setup_block — ARM32 vs ARM64 Design Details

## 1. The Block in Context

### ARM32

```c
#ifdef CONFIG_SMP
if (is_smp()) {
    smp_set_ops(mdesc->smp);   /* ARM32: selects ops from machine descriptor */
    smp_init_cpus();
    smp_build_mpidr_hash();
}
#endif
```

### ARM64

```c
/* arch/arm64/kernel/setup.c — no explicit smp_setup block */
/* ARM64 SMP is handled differently: */
smp_init_cpus();    /* called without is_smp() check */
/* smp_build_mpidr_hash() called separately */
/* ARM64 always uses PSCI — no mdesc->smp fallback */
```

ARM64 removes the `mdesc->smp` concept entirely. PSCI is always the SMP mechanism.

---

## 2. smp_operations: ARM32 Rich Ecosystem vs ARM64 PSCI-Only

### ARM32 smp_operations variants

| Board Family | smp_operations | Mechanism |
|-------------|---------------|-----------|
| Cortex-A9 systems | `cortex_a9_smp_ops` | SCU (Snoop Control Unit) + pen release |
| OMAP4 | `omap4_smp_ops` | Write entry addr to OMAP_AUX_CORE_BOOT_0 register |
| Qualcomm MSM8974 | `msm8974_smp_ops` | Write to RPM scratch register, assert CPU reset |
| Exynos 4 | `exynos4_smp_ops` | Write to GIC CPU interface reset |
| PSCI (any) | `psci_smp_ops` | SMC CPU_ON |

ARM32 has a large collection of board-specific smp_operations in `arch/arm/mach-*/platsmp.c`.

### ARM64: Always PSCI

```c
/* arch/arm64/kernel/smp.c */
/* No smp_set_ops() — SMP ops are hardcoded */
int boot_secondary(unsigned int cpu, struct task_struct *idle)
{
    /* Always: */
    return psci_ops.cpu_on(cpu_logical_map(cpu),
                           __pa_symbol(secondary_entry));
}
```

ARM64 eliminated the smp_operations abstraction. PSCI is the only allowed SMP mechanism (UEFI system firmware requires it). Only `spin-table` is allowed as a fallback (used in FPGA/emulator environments).

---

## 3. Secondary CPU Entry Point

### ARM32: secondary_startup

```c
/* arch/arm/kernel/head.S */
ENTRY(secondary_startup)
    /* Secondary CPU starts here (physical address passed to CPU_ON) */
    safe_svcmode_maskall r0
    mrc p15, 0, r0, c0, c0, 5   /* Read MPIDR */
    and r0, r0, #0xFF            /* Aff0 */
    adr r4, __secondary_data
    /* Set up stack, page tables, then jump to secondary_start_kernel() */
ENDPROC(secondary_startup)
```

### ARM64: secondary_entry

```c
/* arch/arm64/kernel/head.S */
SYM_CODE_START(secondary_entry)
    /* MMU is off, EL1 mode */
    bl      init_kernel_el         /* Set up EL1 system registers */
    b       secondary_startup      /* Jump to C code */
SYM_CODE_END(secondary_entry)
```

ARM64's `secondary_entry` is the physical address passed to `PSCI CPU_ON`. The secondary CPU starts with MMU off, in EL1 mode, then enables MMU and jumps to `secondary_start_kernel()`.

---

## 4. smp_build_mpidr_hash: ARM32 vs ARM64

### ARM32: 3-level hash (Aff0, Aff1, Aff2)

```c
struct mpidr_hash {
    u32 shift_aff[3];  /* shift values for Aff0, Aff1, Aff2 */
    u32 bits[3];       /* bit width at each level */
    u32 l1_shift;      /* combined Aff0 bits */
    u32 l2_shift;      /* combined Aff0+Aff1 bits */
    u32 mask;          /* MPIDR bits that vary across CPUs */
};
```

### ARM64: 4-level hash (Aff0, Aff1, Aff2, Aff3)

```c
struct mpidr_hash {
    u64 mask;
    u32 shift_aff[4];   /* includes Aff3 */
    u32 bits[4];
};
```

ARM64 hash table can accommodate 4096 CPUs (Aff3 enables multi-socket indexing).

---

## 5. CPU Hotplug: ARM32 vs ARM64

### ARM32 Hotplug (board-specific)

```c
/* In smp_operations */
int  (*cpu_kill)(unsigned int cpu);   /* Optional: power off hardware */
void (*cpu_die)(unsigned int cpu);    /* Called on CPU being turned off */
int  (*cpu_disable)(unsigned int cpu);/* Can we disable this CPU? */
```

ARM32 hotplug varies by platform — PSCI boards use `psci_cpu_die()`, Cortex-A9 boards may use SCU power control.

### ARM64 Hotplug (PSCI only)

```c
/* arch/arm64/kernel/cpu_ops.c */
static int psci_cpu_kill(unsigned int cpu)
{
    int err, i;
    err = psci_ops.affinity_info(cpu_logical_map(cpu), 0);
    /* Wait for PSCI to confirm CPU is off */
    for (i = 0; i < 10; i++) {
        if (err == PSCI_0_2_AFFINITY_LEVEL_OFF)
            return 0;
        msleep(100);
        err = psci_ops.affinity_info(cpu_logical_map(cpu), 0);
    }
    return -ETIMEDOUT;
}
```

PSCI `AFFINITY_INFO` polls firmware to confirm the CPU is truly powered off before removing it from the online mask.

---

## 6. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| SMP mechanism selection | `smp_set_ops(mdesc->smp)` or PSCI | Always PSCI (or spin-table) |
| smp_operations abstraction | Yes (board-specific) | No (hardcoded PSCI) |
| is_smp() check | Yes (UP variant check) | No explicit check |
| MPIDR hash levels | 3 (Aff0-Aff2) | 4 (Aff0-Aff3) |
| Secondary entry point | `secondary_startup` (EL1 SVC mode) | `secondary_entry` (EL1, MMU off) |
| Max CPUs | 32 (typical) | 4096 (theoretical) |
| CPU hotplug | Board-specific ops | PSCI AFFINITY_INFO |
| Cortex-A9 pen release | Supported | N/A (no Cortex-A9 in ARM64) |
