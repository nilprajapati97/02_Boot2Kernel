# restart_handler_registration — ARM32 vs ARM64 Design Details

## 1. Key Difference: mdesc vs PSCI/SMCCC

| | ARM32 | ARM64 |
|--|-------|-------|
| Primary restart source | `mdesc->restart` function pointer | PSCI `SYSTEM_RESET` call (via SMC) |
| Fallback | `cpu_reset(0)` (soft CPU reset) | `cpu_park_loop()` (infinite WFI) |
| Machine descriptor | Required per-board | Not used (firmware provides restart) |
| EFI restart | Optional (if arm_efi_init succeeded) | Common (most ARM64 systems are UEFI) |
| PSCI restart | Rare on ARM32 | Standard on ARM64 |

---

## 2. ARM32 restart Registration

```c
/* arch/arm/kernel/setup.c */
if (mdesc->restart)
    arm_pm_restart = mdesc->restart;

register_restart_handler(&arm_restart_nb);
```

ARM32 boards provide restart via the machine descriptor. Every SoC family has its own watchdog/reset controller mechanism:

```c
/* Freescale i.MX6 example */
static void imx6q_restart(enum reboot_mode mode, const char *cmd)
{
    struct device_node *np;
    void __iomem *wdog_base;

    np = of_find_compatible_node(NULL, NULL, "fsl,imx6q-wdt");
    wdog_base = of_iomap(np, 0);

    /* Assert watchdog reset */
    writew_relaxed(0, wdog_base + IMX2_WDT_WCR);
    writew_relaxed(IMX2_WDT_WCR_WDE, wdog_base + IMX2_WDT_WCR);

    udelay(1000);
    /* Should not reach here */
}
```

---

## 3. ARM64 restart Registration

On ARM64, `setup_arch()` does NOT call `register_restart_handler()` explicitly for a machine descriptor. Instead, restart is handled in order:

**PSCI restart (primary):** Registered in `psci_init()` with priority 255:

```c
/* drivers/firmware/psci/psci.c */
static int psci_sys_reset(struct notifier_block *nb, unsigned long action, void *data)
{
    if (pm_cold_boot_atom)
        invoke_psci_fn(PSCI_0_2_FN64_SYSTEM_RESET2, PSCI_SYSTEM_RESET2_TYPE_VENDOR_HW_STATE_RESET, 0, 0);
    else
        invoke_psci_fn(PSCI_0_2_FN64_SYSTEM_RESET, 0, 0, 0);
    return NOTIFY_DONE;
}

static struct notifier_block psci_sys_reset_nb = {
    .notifier_call  = psci_sys_reset,
    .priority       = 129,
};
```

`invoke_psci_fn` issues an `SMC #0` (Secure Monitor Call) to EL3 firmware (ATF — ARM Trusted Firmware), which performs the actual hardware reset.

**EFI restart (secondary, priority 70):**

```c
/* drivers/firmware/efi/reboot.c */
static struct notifier_block efi_reboot_nb = {
    .notifier_call  = efi_reboot,
    .priority       = 70,
};
```

If PSCI is unavailable, EFI `ResetSystem()` runtime service is called.

---

## 4. PSCI SYSTEM_RESET SMC Flow (ARM64 Only)

```
ARM64 kernel (EL1):
  invoke_psci_fn(PSCI_0_2_FN64_SYSTEM_RESET, ...)
    ↓
  SMC #0  (generates secure monitor call exception)
    ↓ [hardware exception, CPU mode switches to EL3]
ATF (ARM Trusted Firmware) at EL3:
  psci_handler()
    → SYSTEM_RESET command
    → PMU/GIC/DRAM controller reset sequence
    → Assert SoC-level reset signal
    ↓
SoC: Power management IC or reset controller asserts RST_N
    ↓
System cold reset: Boot ROM starts
```

ARM32 doesn't use SMC for restart (no ATF typically). ARM64 ARM Trusted Firmware is the universal reset controller.

---

## 5. reboot_mode and Scratch Register Usage

ARM64 adds a standardized way to pass reboot reason to the bootloader:

```c
/* arch/arm64/kernel/reboot.c */
void arm64_restart(enum reboot_mode reboot_mode, const char *cmd)
{
    /* Encode reboot reason in PSCI reset2 types */
    if (reboot_mode == REBOOT_WARM && psci_system_reset2_supported)
        invoke_psci_fn(PSCI_FN_SYSTEM_RESET2,
                       PSCI_RESET2_TYPE_VENDOR_HW_STATE, 0, 0);
    else
        invoke_psci_fn(PSCI_0_2_FN64_SYSTEM_RESET, 0, 0, 0);
}
```

ARM32 boards often used SoC-specific scratch registers (e.g., Qualcomm's `TCSR_BOOT_MISC_DETECT`) to pass "recovery", "fastboot", "download" strings to bootloaders. ARM64 standardizes this via PSCI RESET2 vendor-specific types or UEFI `ResetSystem(EfiResetWarm, ...)`.

---

## 6. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Restart registration | `register_restart_handler()` in `setup_arch()` | PSCI registers in `psci_init()` |
| Function pointer | `arm_pm_restart` (global) | N/A (PSCI via SMC) |
| Hardware access | Directly maps watchdog MMIO | Via ATF/EL3 SMC (no direct MMIO) |
| Priority | 128 | PSCI=129, EFI=70 |
| Warm reboot support | Board-specific | PSCI RESET2 or EFI warm reset |
| PSCI usage | Uncommon | Standard |
| EFI usage | Rare | Common |
| Default fallback | cpu_reset(0) | cpu_park_loop() |
