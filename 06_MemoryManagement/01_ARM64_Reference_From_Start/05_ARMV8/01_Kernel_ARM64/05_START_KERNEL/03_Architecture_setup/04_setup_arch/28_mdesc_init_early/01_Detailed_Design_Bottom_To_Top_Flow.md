# mdesc_init_early — Detailed Design Bottom-To-Top Flow

## 1. The Code: Last Call in setup_arch()

```c
/* arch/arm/kernel/setup.c — end of setup_arch() */
if (mdesc->init_early)
    mdesc->init_early();
```

This is the **final operation in `setup_arch()`** — the machine descriptor's early initialization callback. It allows each board to run board-specific early setup code after all core kernel subsystems (paging, memblock, DT, SMP, PSCI) have been configured but before `start_kernel()` proceeds to scheduler init, interrupt subsystem, etc.

---

## 2. Machine Descriptor: init_early

```c
/* arch/arm/include/asm/mach/arch.h */
struct machine_desc {
    ...
    void (*init_early)(void);    ← board-specific early init
    void (*init_irq)(void);      ← IRQ controller setup (later)
    void (*init_time)(void);     ← clocksource/clockevent setup (later)
    void (*init_machine)(void);  ← board device registration (later)
    void (*init_late)(void);     ← late board init (later)
    ...
};
```

The `init_early()` callback runs **before** `init_irq()`, `init_time()`, and `init_machine()` — those run much later via `time_init()` and `arch_initcall()`.

---

## 3. What Can init_early() Do?

At this point in boot, the following are available:
- ✅ Virtual memory (paging_init() done)
- ✅ memblock allocator
- ✅ Device tree (unflatten done)
- ✅ PSCI (if available)
- ✅ SMP CPU maps (cpu_logical_map[])
- ✅ iomem resource tree

But NOT available:
- ❌ IRQ subsystem (irq_domain, GIC not initialized)
- ❌ Clocksources (timers not configured)
- ❌ Buddy allocator / slab
- ❌ Scheduler
- ❌ Driver model (struct device, platform_device)

So `init_early()` is used for:
1. SoC clock initialization (for early UART baud rate)
2. SoC power domain early setup
3. SRAM reservation or remapping
4. Early hardware identification (silicon revision)
5. Setting up system clocks needed before timer init

---

## 4. Example: OMAP4 init_early()

```c
/* arch/arm/mach-omap2/board-omap4panda.c */
static void __init omap4_panda_init_early(void)
{
    omap2_init_common_infrastructure();
    omap2_init_common_devices(NULL, NULL);
    /* Initializes PRCM (Power, Reset, Clock Manager) */
    /* Sets up clock domains, power domains */
    /* Required before any peripheral driver can use clocks */
}

DT_MACHINE_START(OMAP4_DT, "OMAP4 (Flattened Device Tree)")
    ...
    .init_early = omap4_panda_init_early,
MACHINE_END
```

Without `omap2_init_common_infrastructure()` in `init_early()`, the clock framework would be unavailable and the `init_irq()` → GIC initialization would fail (GIC clock domain not enabled).

---

## 5. Example: i.MX6 init_early()

```c
/* arch/arm/mach-imx/imx6q.c */
static void __init imx6q_init_early(void)
{
    /* Map the IOMUXC (IO Multiplexer Controller) early */
    /* Required for pin muxing before driver probes */
    imx6q_map_io();

    /* Enable the SoC errata workarounds */
    imx6q_init_machine_errata();

    /* Initialize the ARM local timer */
    imx_smp_prepare_cpus(NR_CPUS);
}
```

---

## 6. Example: BCM2835 (Raspberry Pi 1) init_early()

```c
/* arch/arm/mach-bcm2835/bcm2835.c */
static void __init bcm2835_init_early(void)
{
    /* Register the BCM2835 clock provider early */
    /* Needed before any driver uses clk_get() */
}
```

---

## 7. Contrast: init_early() vs init_machine()

| | init_early() | init_machine() |
|--|-------------|---------------|
| Called from | end of setup_arch() | arch_initcall() — much later |
| When | Before interrupt, timer init | After IRQ, timer, driver model |
| Purpose | SoC clocks, early MMIO | Register board devices (platform_device) |
| Can use kmalloc? | No (buddy not ready) | Yes |
| Can register IRQ? | No (irq_domain not ready) | Yes |
| Can register platform_device? | No | Yes |

---

## 8. Interview Q&A

**Q1: What is the significance of init_early() being the LAST call in setup_arch()?**
> Placement at the end of `setup_arch()` is intentional: all core arch setup (paging, memblock, DT, PSCI, SMP) has completed before `init_early()` runs. This means board-specific code in `init_early()` can safely: use `of_find_node_by_path()` (DT ready), call `memblock_alloc()` (paging ready), read `cpu_logical_map[]` (SMP maps ready). If `init_early()` were called earlier, these services would be unavailable and board code would need complex ordering workarounds. By being last, `init_early()` gets maximum infrastructure support while still running before the interrupt/scheduler subsystems that `start_kernel()` initializes next.

**Q2: What happens if mdesc->init_early is NULL?**
> The guard `if (mdesc->init_early)` handles NULL safely — nothing happens and `setup_arch()` returns normally. Many simple boards don't need `init_early()` — they rely on standard ARM clock and power frameworks that work without board-specific early setup. For these boards, the `DT_MACHINE_START()` declaration simply omits `.init_early`. Boards that do need it (complex SoCs with proprietary clock managers like OMAP's PRCM, Samsung's CMU, Qualcomm's RPMCC) provide the callback.

**Q3: How does init_early() relate to the board's init_irq() and what's the ordering requirement?**
> `init_early()` must complete any SoC clock/power setup needed for the interrupt controller. The GIC (Generic Interrupt Controller) needs its clock domain enabled before `irqchip_init()` configures it. On OMAP, the GIC is in the MPU subsystem power domain — `omap2_init_common_infrastructure()` in `init_early()` ensures the MPU power domain is ON. Then `init_irq()` (called via `irqchip_init()` → `irq_domain_add_*()`) can safely access GIC registers. If this ordering were reversed — `init_irq()` before `init_early()` — GIC register access would generate an external abort (bus error) because the clock/power to GIC isn't enabled yet. This is a classic ARM SoC power management dependency.
