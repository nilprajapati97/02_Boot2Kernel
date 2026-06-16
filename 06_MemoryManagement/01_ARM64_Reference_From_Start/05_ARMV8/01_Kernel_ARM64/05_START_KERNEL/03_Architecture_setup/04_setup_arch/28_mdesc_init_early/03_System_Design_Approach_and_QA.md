# mdesc_init_early — System Design Approach and Q&A

## 1. Why a Callback Table Design (vs #ifdef CONFIG_ARCH_OMAP)?

Before machine descriptors were introduced (early ARM Linux, ~2005), board-specific code used `#ifdef`:

```c
/* Old approach (bad) */
void __init init_IRQ(void)
{
#ifdef CONFIG_ARCH_OMAP
    omap_init_irq();
#elif CONFIG_ARCH_IMX
    imx_init_irq();
#elif CONFIG_ARCH_BCM2835
    bcm2835_init_irq();
#endif
}
```

Problems:
- Every board required patching core arch code
- Kernel binary included all boards' code
- Can't compile multi-board kernel
- Scaling: 500+ ARM boards means 500 `#ifdef` chains

The machine descriptor callback table solved this:
- One binary can support multiple boards (matched via DT `compatible`)
- Board-specific code isolated in `arch/arm/mach-*/`
- Core arch code has zero board-specific `#ifdef`
- Clean separation of concerns

---

## 2. The Journey: ATAG → Machine Descriptor → DT → DT-only (ARM64)

```
ARM Linux evolution:
  1990s: x86 BIOS-style detection (register-based, arch-specific)
  2003: ATAG (ARM Tags) — bootloader passes param list
  2005: Machine numbers + machine descriptor (matchedby machine ID)
  2007: Device Tree introduced in ARM (PowerPC used it since 2005)
  2011: DT matching via .dt_compat[] in machine descriptor
  2012: ARM32 "DT-only" — machine desc still needed but very thin
  2013: ARM64 launched with DT-only, no machine descriptor
  2015: ARM32 legacy board files deprecated (still maintained)
  Today: ARM32 maintains both DT+mdesc and legacy ATAG boards
          ARM64: pure DT (or ACPI), no mdesc at all
```

`init_early()` is a product of the machine descriptor era — it survives in ARM32 because thousands of boards still use it.

---

## 3. Dependency Graph

```
[DT: compatible = "ti,omap4-panda" matches machine_desc]
        │
[setup_machine_fdt() sets mdesc = &omap4_panda_machine]
        │
[All of setup_arch() runs (paging, memblock, DT, SMP, PSCI...)]
        │
[if (mdesc->init_early) mdesc->init_early()]
  └── omap4_panda_init_early()
        └── omap2_init_common_infrastructure()
              → PRCM mapped (power/clock manager MMIO)
              → clock domains enabled
              → power domains stable
        │
[setup_arch() returns]
        │
[start_kernel() → init_IRQ() → mdesc->init_irq()]
  └── omap4_init_irq()  ← can now access GIC (clock enabled by init_early)
```

---

## 4. System Design Q&A

**Q: Is mdesc->init_early() guaranteed to be called before any clock subsystem calls?**
> Yes. `mdesc->init_early()` is the last call in `setup_arch()`. `start_kernel()` then calls subsystems in order: `setup_arch()` → `mm_init()` → `sched_init()` → `init_IRQ()` → `tick_init()` → `timekeeping_init()` → `time_init()`. The clock subsystem (`clk_init()`) and timer subsystem (`time_init()`) all run after `init_early()`. This ordering guarantee means `init_early()` can safely set up clock gates or power domains that subsequent subsystems depend on. If a board's `init_early()` fails to enable a clock, the subsequent `init_irq()` attempt to access a clock-gated peripheral would cause a synchronous abort.

**Q: Why does ARM64 not need init_early()? What handles the equivalent setup?**
> ARM64 moves board-specific initialization into three places: (1) **ATF/firmware**: Power domain initialization, DRAM initialization, clock tree configuration at EL3 — done before the kernel starts. By the time Linux runs, all power domains are up. (2) **DT + generic drivers**: The generic clock provider (`CONFIG_COMMON_CLK`), power domain (`CONFIG_PM_GENERIC_DOMAINS`), and pinctrl frameworks parse DT nodes and configure hardware without board-specific C code. A `clk-amlogic.c` driver handles all Amlogic SoCs; no `init_early()` needed. (3) **ACPI DSDT**: For ARM64 servers, ACPI DSDT methods handle power sequencing. The combination eliminates the need for hand-crafted `init_early()` callbacks entirely.

**Q: Can init_early() be NULL on a board that uses CONFIG_SMP with Cortex-A7? What's the minimum init_early does?**
> Yes, `init_early()` can be NULL on many Cortex-A7 boards (BCM2836, STM32MP1 at simple configs). If all of: (1) clock framework (`CONFIG_COMMON_CLK`) provides clock drivers that initialize themselves from DT during `subsys_initcall`, (2) the GIC is a standard `arm,gic-400` that needs no power domain setup before `irq_domain_add_*()`, (3) no custom SRAM mapping or early MMIO needed, then `init_early()` is unnecessary. The "minimum" that `init_early()` must do is whatever would otherwise cause a fault in `init_irq()` or `time_init()` if omitted. As ARM SOC vendors moved their clock/power code to standard framework drivers, the need for `init_early()` on new boards has essentially disappeared — it's primarily legacy infrastructure.
