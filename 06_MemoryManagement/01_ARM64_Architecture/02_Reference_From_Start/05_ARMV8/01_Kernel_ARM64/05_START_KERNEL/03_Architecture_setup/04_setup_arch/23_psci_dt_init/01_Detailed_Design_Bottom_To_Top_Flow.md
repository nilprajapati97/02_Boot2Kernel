# psci_dt_init() — Detailed Design Bottom-To-Top Flow

## 1. What Is PSCI?

**PSCI (Power State Coordination Interface)** is an ARM standard (ARM DEN0022) that defines a firmware interface for CPU power management operations. Instead of each SoC having its own custom power management registers, PSCI provides a unified API:

| PSCI Function | ID | Purpose |
|--------------|-----|---------|
| PSCI_VERSION | 0x84000000 | Query PSCI version |
| CPU_SUSPEND | 0x84000001 | Suspend a CPU core |
| CPU_OFF | 0x84000002 | Power off a CPU core |
| CPU_ON | 0x84000003 | Power on a secondary CPU |
| AFFINITY_INFO | 0x84000004 | Query CPU affinity state |
| SYSTEM_SUSPEND | 0x8400000E | Suspend entire system |
| SYSTEM_RESET | 0x84000009 | Reset the system |
| SYSTEM_OFF | 0x84000008 | Power off the system |

ARM64 uses 64-bit versions: CPU_ON_64 = 0xC4000003, etc.

---

## 2. How PSCI Is Invoked

PSCI is implemented at EL3 (Secure Monitor, ARM Trusted Firmware). The kernel calls PSCI by issuing an SMC (Secure Monitor Call) or HVC (Hypervisor Call):

```c
/* PSCI call via SMC */
static int invoke_psci_fn_smc(u32 function_id, u32 arg0, u32 arg1, u32 arg2)
{
    __invoke_psci_fn_smc(function_id, arg0, arg1, arg2);
    /* Assembly: smc #0 */
    /* CPU switches to EL3, ATF handles the call, returns here */
}

/* PSCI call via HVC (when running under hypervisor) */
static int invoke_psci_fn_hvc(u32 function_id, u32 arg0, u32 arg1, u32 arg2)
{
    __invoke_psci_fn_hvc(function_id, arg0, arg1, arg2);
    /* Assembly: hvc #0 */
}
```

---

## 3. psci_dt_init() Source

**File:** `drivers/firmware/psci/psci.c`

```c
int __init psci_dt_init(void)
{
    struct device_node *np;
    const struct of_device_id *matched_np;
    psci_initcall_t init_fn;

    /* Look for PSCI node in DT: "arm,psci-0.2", "arm,psci-1.0", or "arm,psci" */
    np = of_find_matching_node_and_match(NULL, psci_of_match, &matched_np);
    if (!np || !of_device_is_available(np))
        return -ENODEV;

    init_fn = (psci_initcall_t)matched_np->data;
    return init_fn(np);
}

static const struct of_device_id psci_of_match[] __initconst = {
    { .compatible = "arm,psci",     .data = psci_0_1_init },
    { .compatible = "arm,psci-0.2", .data = psci_0_2_init },
    { .compatible = "arm,psci-1.0", .data = psci_0_2_init },  /* same init */
    {},
};
```

---

## 4. PSCI 0.2 Initialization: psci_0_2_init()

```c
static int __init psci_0_2_init(struct device_node *np)
{
    int err;

    /* Determine call method: "smc" or "hvc" */
    err = get_set_conduit_method(np);
    if (err)
        return err;

    /* Query PSCI version from firmware */
    err = psci_probe();
    if (err)
        return err;

    /* Register PSCI ops */
    psci_ops.cpu_suspend = psci_cpu_suspend;
    psci_ops.cpu_off     = psci_cpu_off;
    psci_ops.cpu_on      = psci_cpu_on;
    psci_ops.migrate     = psci_migrate;

    /* Register system restart/shutdown handlers */
    register_restart_handler(&psci_sys_reset_nb);    /* priority 129 */
    register_pm_notifier(&psci_sys_off_nb);          /* system power off */

    return 0;
}
```

---

## 5. The DT PSCI Node

```dts
/* Typical Cortex-A board DT */
psci {
    compatible = "arm,psci-0.2";
    method = "smc";          /* or "hvc" if under hypervisor */
};

cpus {
    cpu@0 {
        enable-method = "psci";   /* boot CPU also uses PSCI for suspend */
        ...
    };
    cpu@1 {
        enable-method = "psci";   /* secondary CPUs brought up via PSCI CPU_ON */
        ...
    };
};
```

The `method` property selects SMC vs HVC:
- **SMC**: System is bare-metal, ATF at EL3 handles PSCI
- **HVC**: System is a VM (e.g., KVM, Xen), hypervisor at EL2 handles PSCI calls

---

## 6. PSCI CPU_ON Flow (Bringing Up Secondary CPU)

```c
/* Called from smp_init_cpus() → cpu_up() → __cpu_up() */
static int psci_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
    return psci_ops.cpu_on(cpu_logical_map(cpu),
                           __pa_symbol(secondary_entry));
    /* 
     * secondary_entry: physical address of ARM secondary boot vector
     * ATF at EL3:
     *   1. Powers on CPU with MPIDR = cpu_logical_map(cpu)
     *   2. Sets CPU PC = secondary_entry (physical address)
     *   3. Returns to kernel via ERET
     * Secondary CPU:
     *   secondary_entry → secondary_startup → __secondary_switched
     *   → secondary_start_kernel() → CPU is online
     */
}
```

---

## 7. struct psci_operations

```c
/* include/linux/psci.h */
struct psci_operations {
    u32 (*get_version)(void);
    int (*cpu_suspend)(u32 state, unsigned long entry_point);
    int (*cpu_off)(u32 state);
    int (*cpu_on)(unsigned long cpuid, unsigned long entry_point);
    int (*migrate)(unsigned long cpuid);
    int (*affinity_info)(unsigned long target_affinity,
                         unsigned long lowest_affinity_level);
    int (*migrate_info_type)(void);
};

extern struct psci_operations psci_ops;
```

After `psci_dt_init()`, `psci_ops` is filled in and used by the SMP code.

---

## 8. Interview Q&A

**Q1: What happens if the DT has no psci node? How do CPUs get brought up?**
> Without a PSCI node, `psci_dt_init()` returns `-ENODEV` and `psci_ops` remains empty. The kernel falls back to the machine descriptor's SMP operations: `mdesc->smp` → set via `smp_set_ops()`. Older ARM SoCs (pre-2013 before PSCI standardization) used board-specific secondary CPU bringup: Cortex-A9 used a "pen release" mechanism (write secondary entry to a scratch register, secondary CPU spins on that register). These mechanisms are registered as `smp_operations` structs in the machine descriptor.

**Q2: What is the difference between PSCI 0.1 and PSCI 0.2?**
> PSCI 0.1 is the original version: only mandatory functions are CPU_ON and CPU_OFF. The DT node specifies which functions are available and their custom IDs (non-standard). PSCI 0.2 standardizes all function IDs (no DT-specified IDs), adds AFFINITY_INFO, SYSTEM_SUSPEND, SYSTEM_OFF, SYSTEM_RESET — a much more complete interface. PSCI 1.0 (same init code) adds CPU_FREEZE, CPU_DEFAULT_SUSPEND. Nearly all modern boards use PSCI 0.2+. The kernel's `psci_0_2_init()` handles both 0.2 and 1.0 (they share the same standardized IDs).

**Q3: Can PSCI be used without ATF (ARM Trusted Firmware)?**
> Yes. PSCI is a specification — any firmware at EL3 can implement it. ATF (TF-A, Trusted Firmware-A) is the most common implementation for Cortex-A systems. U-Boot can implement PSCI for simple boards. For emulators: QEMU implements PSCI entirely in software (no real EL3 — QEMU intercepts SMC instructions). For Android/mobile: Qualcomm, MediaTek, and other SoC vendors have their own proprietary EL3 firmware that implements PSCI. The kernel only cares that `SMC #0` with PSCI function IDs works — it doesn't care about the EL3 implementation.
