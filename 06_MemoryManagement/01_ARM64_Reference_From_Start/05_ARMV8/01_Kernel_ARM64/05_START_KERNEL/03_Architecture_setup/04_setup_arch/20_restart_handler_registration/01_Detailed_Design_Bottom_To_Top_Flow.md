# restart_handler_registration — Detailed Design Bottom-To-Top Flow

## 1. What Is This Code?

In `setup_arch()`:

```c
if (mdesc->restart)
    arm_pm_restart = mdesc->restart;

register_restart_handler(&arm_restart_nb);
```

This code registers the board-specific restart function so that when the user runs `reboot`, the system calls the correct hardware reset mechanism for this board.

---

## 2. The Problem Being Solved

ARM SoCs (System on Chip) use vastly different hardware reset mechanisms:

| SoC | Reset mechanism |
|-----|----------------|
| Raspberry Pi (BCM2835) | Watchdog timer register write |
| i.MX6 (Freescale) | SRC (System Reset Controller) register |
| OMAP (TI) | PRM (Power/Reset Manager) module |
| Allwinner A20 | Watch dog or PMIC reset |
| Qualcomm MSM | PSHOLD register de-assert |

The kernel needs a **unified** restart interface (`sys_reboot()`) that dispatches to the correct SoC-specific reset. The machine descriptor provides this via `mdesc->restart`.

---

## 3. Machine Descriptor restart Callback

**File:** `arch/arm/include/asm/mach/arch.h`

```c
struct machine_desc {
    ...
    void        (*restart)(enum reboot_mode, const char *);
    ...
};
```

Example — BCM2835 (Raspberry Pi 1/2):

```c
/* arch/arm/mach-bcm2835/bcm2835.c */
static void bcm2835_restart(enum reboot_mode mode, const char *cmd)
{
    /* Write magic value to watchdog WDOG_RSTC register */
    writel(PM_PASSWORD | PM_WDOG_TIME_SET,
           base + PM_WDOG);
    writel(PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET,
           base + PM_RSTC);
    /* Hardware watchdog fires in ~10ms, rebooting SoC */
    udelay(10000);
}

DT_MACHINE_START(BCM2835, "BCM2835")
    ...
    .restart    = bcm2835_restart,
MACHINE_END
```

---

## 4. arm_pm_restart: The Global Pointer

```c
/* arch/arm/kernel/reboot.c */
void (*arm_pm_restart)(enum reboot_mode reboot_mode, const char *cmd);
```

This is a global function pointer. When `mdesc->restart` is non-NULL, `setup_arch()` sets `arm_pm_restart = mdesc->restart`. The function pointer approach allows runtime selection of the restart handler without any `#ifdef` machinery.

---

## 5. register_restart_handler: The Notifier Chain

```c
/* arch/arm/kernel/reboot.c */
static struct notifier_block arm_restart_nb = {
    .notifier_call  = arm_restart,
    .priority       = 128,   /* Medium-high priority */
};

static int arm_restart(struct notifier_block *nb, unsigned long action, void *data)
{
    if (arm_pm_restart)
        arm_pm_restart(reboot_mode, data);
    else
        /* Default: use CPU reset instruction */
        cpu_reset(0);
    return NOTIFY_DONE;
}
```

`register_restart_handler()` adds `arm_restart_nb` to the global `restart_handler_list` notifier chain. When the system reboots:

```
sys_reboot()
  → kernel_restart(cmd)
    → kernel_restart_prepare(cmd)
      → blocking_notifier_call_chain(&restart_handler_list, ...)
        → arm_restart()        ← our registered handler
          → arm_pm_restart()   ← mdesc->restart (board-specific)
            → writes watchdog register / SRC register / etc.
```

---

## 6. Priority System for restart notifier

The `priority` field determines the order in which notifiers are called when multiple handlers are registered:

```
Priority 255 (highest):  PSCI restart handler (if available)
Priority 200:            EFI restart handler (UEFI systems)
Priority 128:            arm_restart_nb (mdesc->restart)
Priority 0 (lowest):     Default software reset
```

If PSCI is available (registered with priority 255), it gets called first and returns `NOTIFY_STOP`, preventing lower-priority handlers from running. If PSCI is unavailable, the arm_restart handler (128) runs.

---

## 7. Bottom-to-Top Call Stack at Reboot

```
User space: reboot(RB_AUTOBOOT)
  ↓
sys_reboot() — kernel/reboot.c
  ↓
kernel_restart(cmd)
  ↓
blocking_notifier_call_chain(&restart_handler_list)
  ↓
arm_restart() — arch/arm/kernel/reboot.c
  ↓
arm_pm_restart() = mdesc->restart()
  ↓
Board-specific: bcm2835_restart() / omap_restart() / ...
  ↓
Hardware: Watchdog/SRC/PMIC reset fires
  ↓
SoC reset, boot ROM starts, bootloader loads
```

---

## 8. Interview Q&A

**Q1: Why use a notifier chain for restart instead of just calling arm_pm_restart() directly?**
> The notifier chain allows multiple subsystems to register restart handlers with different priorities. PSCI firmware can register at priority 255, EFI at 200, board-specific at 128. The highest priority handler that successfully performs reset "wins" — lower priority handlers don't run if the higher priority one returns `NOTIFY_STOP`. This is critical on heterogeneous systems (e.g., a board that has both PSCI and a watchdog) — PSCI is tried first, and only if PSCI isn't available does the watchdog path run. A single function pointer would require manual priority management and complex `#ifdef CONFIG_PSCI`.

**Q2: What happens if mdesc->restart is NULL?**
> If `mdesc->restart` is NULL, `arm_pm_restart` is not set and remains NULL. When `arm_restart()` is called at reboot time, it checks `if (arm_pm_restart)` — since it's NULL, it falls back to `cpu_reset(0)`, which attempts a CPU-level software reset. On most modern ARM SoCs this doesn't actually reset the entire SoC — just the CPU core. It may hang or reset into an undefined state. That's why every production board driver provides a `mdesc->restart` implementation.

**Q3: What is enum reboot_mode and what values does it have?**
> `enum reboot_mode` distinguishes why the system is rebooting:
> - `REBOOT_COLD` — full power-off and power-on (hardware cold reset)
> - `REBOOT_WARM` — reset preserving some state (RAM contents may survive)
> - `REBOOT_HARD` — hardware reset signal asserted
> - `REBOOT_SOFT` — software-initiated reset (PSCI reset)
> - `REBOOT_GPIO` — reset via GPIO pin assertion
> Some SoCs handle these differently — e.g., BCM2835 uses the same watchdog register for all modes, but Qualcomm MSM may use different SRC bits for cold vs warm reset. The `cmd` string (e.g., "recovery", "bootloader") is passed to bootloaders via scratch registers to select boot mode.
