# psci_dt_init() — System Design Approach and Q&A

## 1. Why PSCI Exists: The Fragmentation Problem

Before PSCI (pre-2012), every ARM SoC had its own CPU power management:
- TI OMAP: PRCM (Power and Reset Clock Manager) registers
- Qualcomm MSM: RPM (Resource Power Manager) custom protocol
- Samsung Exynos: PMU (Power Management Unit) registers
- NXP i.MX: ANATOP/SRC registers

This meant the Linux kernel had hundreds of lines of `#ifdef CONFIG_ARCH_OMAP` / `CONFIG_ARCH_MSM` scattered through the SMP code. Adding a new SoC required writing custom CPU on/off code in arch/arm.

PSCI solves this by:
1. Defining standard SMC call IDs that work on any ARM Cortex-A system
2. Moving hardware-specific code to firmware (ATF) which runs at EL3
3. Giving the kernel a single `psci_ops` interface regardless of SoC

---

## 2. Security Model: Firmware Isolation

PSCI is more than convenience — it's a security boundary:

```
EL0: User space (processes)
EL1: Linux kernel (ring 0 equivalent)
EL2: Hypervisor (KVM, Xen, Type-1)
EL3: Secure Monitor (ATF, handles PSCI)
     └── Has exclusive access to:
           - DRAM initialization registers
           - SoC reset controllers
           - Secure DRAM regions
           - Crypto accelerators (secure world)
```

The kernel at EL1 cannot directly write power management registers — those are in the **Secure World** accessible only to EL3 or TrustZone. PSCI SMC calls cross the EL3 boundary safely under firmware control. A buggy or malicious kernel driver can't corrupt power management hardware.

---

## 3. Dependency Graph

```
[unflatten_device_tree()]
  └── /psci node and cpu@N/enable-method accessible
        │
[arm_dt_init_cpu_maps()]
  └── cpu_logical_map[] filled
        │
[psci_dt_init()]
  ├── of_find_matching_node(NULL, psci_of_match)
  │     → finds compatible = "arm,psci-0.2"
  ├── get_set_conduit_method(np) → sets invoke_psci_fn to SMC or HVC variant
  ├── psci_probe() → calls PSCI_VERSION, validates firmware
  ├── psci_ops.cpu_on = psci_cpu_on
  ├── psci_ops.cpu_off = psci_cpu_off
  ├── psci_ops.cpu_suspend = psci_cpu_suspend
  └── register_restart_handler(&psci_sys_reset_nb) → priority 129
        │
[smp_set_ops()]
  └── smp_ops = psci_smp_ops (if PSCI available) or mdesc->smp
        │
[smp_init_cpus()]
  └── for each possible CPU: psci_cpu_on(mpidr, entry) → SMC → ATF → CPU boots
        │
[Runtime: CPU hotplug]
  └── cpu_up()/cpu_down() → psci_ops.cpu_on/cpu_off → SMC → ATF
```

---

## 4. psci_probe(): Version Handshake

```c
static int psci_probe(void)
{
    u32 ver;

    ver = invoke_psci_fn(PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0);

    /* PSCI_VERSION returns major.minor in bits [31:16].[15:0] */
    pr_info("PSCIv%d.%d detected in firmware.\n",
            PSCI_VERSION_MAJOR(ver), PSCI_VERSION_MINOR(ver));

    if (PSCI_VERSION_MAJOR(ver) == 0 && PSCI_VERSION_MINOR(ver) < 2) {
        pr_err("Conflicting PSCI version detected.\n");
        return -EINVAL;
    }

    psci_ops.get_version = psci_get_version;
    return 0;
}
```

The version handshake ensures the kernel doesn't issue PSCI 0.2 function calls to a PSCI 0.1 firmware that doesn't understand them.

---

## 5. System Design Q&A

**Q: How does the kernel handle a PSCI CPU_ON failure for a secondary CPU?**
> `psci_cpu_on()` returns a PSCI error code which is translated to a Linux errno. Common failures: `PSCI_RET_INVALID_PARAMS` (bad MPIDR — cpu_logical_map[] has wrong value), `PSCI_RET_ALREADY_ON` (CPU is already running — shouldn't happen at boot), `PSCI_RET_INTERNAL_FAILURE` (firmware error). The SMP bringup code in `__cpu_up()` waits for the secondary CPU to signal that it's online (via an atomic variable `secondary_boot_lock`). If the CPU doesn't come online within a timeout (~1 second), `__cpu_up()` returns `-ENOSYS`. The kernel logs "CPU N: failed to come online" and that CPU is removed from `cpu_present_mask`. The system boots with fewer CPUs.

**Q: What is the difference between cpu_on and cpu_suspend in PSCI? When is suspend used?**
> `CPU_ON` brings a CPU from power-off state to running — used for secondary CPU bringup during boot and CPU hotplug. `CPU_SUSPEND` puts a CPU into a low-power sleep state while retaining the ability to wake up on interrupts — used for CPU idle (cpuidle subsystem). When all CPUs are idle (no runnable processes), Linux's CPUIdle governor calls `arm_enter_idle_state()` → `psci_cpu_suspend_enter()` → `PSCI CPU_SUSPEND`. When an interrupt fires (timer, network, input), the firmware wakes the CPU and it returns to the kernel's idle loop. ARM's PSCI `CPU_SUSPEND` supports multiple power states: retention (registers preserved, fast wakeup), power down (full power off, slow wakeup, stack must be saved).

**Q: Why does psci_sys_reset_nb have priority 129 while arm_restart_nb has priority 128?**
> PSCI system reset (priority 129) is higher priority than the board-specific watchdog restart (priority 128). PSCI reset is preferred because: (1) it's coordinated through ATF which can cleanly shut down the Secure World, DRAM controllers, and PCIe PHYs before asserting the reset signal, (2) PSCI SYSTEM_RESET2 supports warm reset semantics that preserve selected state, (3) the board watchdog is an emergency backup for when PSCI fails. If PSCI `SYSTEM_RESET` returns (it shouldn't on success), `NOTIFY_DONE` falls through to the arm_restart handler at 128, which tries the watchdog. Having both registered provides defense-in-depth for reliable system restart.
