# mdesc_init_early — ARM32 vs ARM64 Design Details

## 1. ARM32: Machine Descriptor Callbacks Are Central

ARM32's machine descriptor (`struct machine_desc`) provides a complete board lifecycle:

```c
struct machine_desc {
    /* Identity */
    const char *name;
    const char *const *dt_compat;

    /* Hardware setup callbacks - in order of invocation */
    void (*init_early)(void);    /* 1. end of setup_arch() */
    void (*init_irq)(void);      /* 2. init_IRQ() */
    void (*init_time)(void);     /* 3. time_init() */
    void (*init_machine)(void);  /* 4. arch_initcall() */
    void (*init_late)(void);     /* 5. late_initcall() */

    /* Topology */
    unsigned int nr_irqs;

    /* Power management */
    void (*restart)(enum reboot_mode, const char *);
    const struct smp_operations *smp;
    bool (*smp_init)(void);

    /* I/O memory */
    void (*map_io)(void);
    void (*reserve)(void);
    void (*video_start)(unsigned long);
    void (*video_end)(unsigned long);
};
```

This callback table drives the entire board initialization lifecycle.

---

## 2. ARM64: No Machine Descriptor (No init_early Concept)

ARM64 `setup_arch()` does NOT call `mdesc->init_early()`. ARM64 has no machine descriptor. Board-specific early initialization on ARM64 is done through:

1. **Device tree + DT-driven clock/power initialization**
2. **Firmware (PSCI/ATF)** handles power domain setup
3. **ACPI** on server-class systems

```c
/* arch/arm64/kernel/setup.c */
void __init setup_arch(char **cmdline_p)
{
    ...
    unflatten_device_tree();
    psci_dt_init();
    xen_early_init();
    efi_init();
    ...
    /* No mdesc->init_early() equivalent */
    /* Board-specific init via DT match → driver probe later */
}
```

---

## 3. ARM64 Equivalent: OF Platform Populate

On ARM64, board-specific device setup happens via the **OF (Open Firmware) platform** subsystem:

```c
/* arch/arm64/kernel/setup.c */
static int __init arm64_device_init(void)
{
    of_platform_default_populate(NULL, NULL, NULL);
    /* Creates platform_device for all DT nodes with compatible strings */
    /* This is the ARM64 equivalent of mdesc->init_machine() */
    return 0;
}
arch_initcall_sync(arm64_device_init);
```

All ARM64 "board initialization" is done by probing DT-described devices via the driver model — no board-specific C code outside drivers.

---

## 4. The Philosophy Difference

### ARM32: Explicit Board Code

```
Board file (arch/arm/mach-omap2/board-omap4panda.c):
  → Explicitly calls OMAP2-specific functions
  → Board engineer writes C code knowing OMAP2 internals
  → Tight coupling: board file #includes omap2-specific headers

Result:
  arch/arm/mach-*/  ← hundreds of board files
  Mainline kernel: ~100+ machine descriptors
```

### ARM64: Configuration-Only

```
Board DTS file (arch/arm64/boot/dts/amlogic/meson-g12a.dtsi):
  → Describes hardware: clock nodes, power domains, CPUs
  → No C code: just property=value
  → Standard compatible strings: "arm,gic-v3", "amlogic,meson-clkc"

Result:
  Generic drivers match compatible strings
  Board-specific behavior encoded in DT properties
  No arch/arm64/mach-* directory exists at all
```

---

## 5. ARM32 Machine Descriptor Callback Timeline

```
Power on
  ↓
setup_arch():
  mdesc->reserve()        ← reserve board-specific memblock regions
  ...
  mdesc->init_early()     ← LAST in setup_arch()
  ↓
start_kernel():
  init_IRQ() →
    mdesc->init_irq()     ← GIC + board IRQ controller setup
  time_init() →
    mdesc->init_time()    ← timer/clocksource setup
  rest_init() → kernel_init():
    customize_machine() →
      mdesc->init_machine() ← register platform_device, I2C, SPI
  late_initcall():
    mdesc->init_late()    ← optional board late init
```

---

## 6. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| init_early() | Yes (mdesc callback) | No |
| Machine descriptor | Required per board | Not used |
| Board lifecycle | Explicit C callbacks | DT-driven driver probe |
| arch/arm/mach-*/ | Yes (many files) | Does not exist |
| Clock init in init_early | Yes (SoC-specific) | Via clk provider DT nodes |
| Power domain in init_early | Yes (SoC-specific) | Via power-domains DT + ATF |
| init_machine equivalent | mdesc->init_machine() | of_platform_populate() |
| Board-specific C code | Hundreds of files | None (DT + generic drivers) |
