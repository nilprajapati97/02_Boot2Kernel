# GIC — Generic Interrupt Controller

## Overview

The **GIC-v3 (Generic Interrupt Controller version 3)** is the interrupt controller on SDM660. It routes interrupts from all peripherals, timers, and IPIs (Inter-Processor Interrupts) to the correct CPU core. Every hardware event that needs CPU attention goes through the GIC.

---

## GIC-v3 Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    GIC-v3 Architecture                        │
│                                                              │
│  ┌──────────────────────────────────────────┐                │
│  │          GICD (Distributor)               │                │
│  │          @ 0x17A0_0000                    │                │
│  │                                          │                │
│  │  Manages SPI (Shared Peripheral Int):    │                │
│  │  ├── 300+ SPIs from SoC peripherals      │                │
│  │  ├── Priority assignment                 │                │
│  │  ├── Target CPU routing                  │                │
│  │  └── Enable/disable control              │                │
│  └──────────────┬───────────────────────────┘                │
│                 │                                            │
│    ┌────────────┼────────────────────────────┐               │
│    │            │            │               │               │
│  ┌─▼──────┐ ┌──▼─────┐ ┌───▼────┐  ... (8 cores total)    │
│  │ GICR0  │ │ GICR1  │ │ GICR2  │          │               │
│  │(Redist)│ │(Redist)│ │(Redist)│          │               │
│  │@ +0x0  │ │@ +0x20K│ │@ +0x40K│          │               │
│  │        │ │        │ │        │          │               │
│  │ PPI:   │ │ PPI:   │ │ PPI:   │          │               │
│  │ Timer  │ │ Timer  │ │ Timer  │          │               │
│  │ Maint  │ │ Maint  │ │ Maint  │          │               │
│  │ SGI:   │ │ SGI:   │ │ SGI:   │          │               │
│  │ IPI    │ │ IPI    │ │ IPI    │          │               │
│  └────────┘ └────────┘ └────────┘          │               │
│     │          │          │                 │               │
│  ┌──▼──┐   ┌──▼──┐   ┌──▼──┐             │               │
│  │CPU0 │   │CPU1 │   │CPU2 │   ... CPU7  │               │
│  └─────┘   └─────┘   └─────┘             │               │
└──────────────────────────────────────────────────────────────┘
```

---

## Interrupt Types

| Type | ID Range | Scope | Examples |
|------|----------|-------|---------|
| **SGI** (Software Generated) | 0-15 | Per-CPU | IPI (inter-processor interrupt), TLB flush |
| **PPI** (Private Peripheral) | 16-31 | Per-CPU | Timer (29), maintenance (25) |
| **SPI** (Shared Peripheral) | 32-1019 | Shared | UART (107), I2C (97), USB (131) |

---

## SDM660 Interrupt Map (Key SPIs)

| SPI # | GIC ID (SPI+32) | Source | DT Specification |
|-------|-----------------|--------|------------------|
| 97 | 129 | BLSP1_QUP3 (I2C Bus 3) | `<GIC_SPI 97 IRQ_TYPE_LEVEL_HIGH>` |
| 107 | 139 | BLSP1_UART1 (Debug UART) | `<GIC_SPI 107 IRQ_TYPE_LEVEL_HIGH>` |
| 131 | 163 | USB3 DWC3 | `<GIC_SPI 131 IRQ_TYPE_LEVEL_HIGH>` |
| 165 | 197 | SDCC1 (eMMC) | `<GIC_SPI 165 IRQ_TYPE_LEVEL_HIGH>` |
| 208 | 240 | TLMM summary (GPIO) | `<GIC_SPI 208 IRQ_TYPE_LEVEL_HIGH>` |
| 265 | 297 | UFS HC | `<GIC_SPI 265 IRQ_TYPE_LEVEL_HIGH>` |

---

## GIC in Device Tree

```dts
intc: interrupt-controller@17a00000 {
    compatible = "arm,gic-v3";
    reg = <0x17a00000 0x10000>,     /* GICD (Distributor) */
          <0x17b00000 0x100000>;    /* GICR (Redistributors) */
    interrupt-controller;
    #interrupt-cells = <3>;
    /* Cell 1: type (0=SPI, 1=PPI) */
    /* Cell 2: interrupt number */
    /* Cell 3: flags (1=rising, 2=falling, 4=high, 8=low) */
};
```

---

## GPIO Interrupts via TLMM → GIC

GPIOs (like BMI160's interrupt on GPIO 23) don't connect directly to GIC SPIs. Instead, the TLMM aggregates all GPIO interrupts into a single SPI:

```
BMI160 sensor (hardware)
    │
    ├── INT pin → GPIO 23 (TLMM)
    │
    ▼
TLMM Interrupt Controller (pinctrl)
    │
    ├── GPIO 23 fires → TLMM sets GPIO 23 interrupt status
    ├── TLMM generates summary interrupt → GIC SPI 208
    │
    ▼
GIC routes SPI 208 → CPU
    │
    ▼
Linux IRQ handler
    │
    ├── TLMM irq_chip demux handler reads which GPIO fired
    └── Calls GPIO 23 handler → BMI160 driver IRQ handler
```

### DT for GPIO Interrupt (BMI160)

```dts
bmi160@68 {
    compatible = "bosch,bmi160";
    reg = <0x68>;
    interrupt-parent = <&tlmm>;     /* TLMM is the IRQ controller */
    interrupts = <23 IRQ_TYPE_EDGE_RISING>;  /* GPIO 23, rising edge */
};
```

---

## GIC Initialization in Kernel

```
start_kernel() → init_IRQ() → irqchip_init() → of_irq_init()
    │
    ▼
GIC driver matches "arm,gic-v3"
    │
    ├── Map GICD registers
    ├── Map GICR registers (one per CPU)
    ├── Configure GICD: enable, set default priority
    ├── Configure GICR: enable PPIs, configure priorities
    ├── Register irq_domain (IRQ mapping)
    │   └── Maps hardware IRQ numbers to Linux IRQ numbers
    └── Ready to handle interrupts

TLMM pinctrl driver probes later:
    ├── Registers as gpio-controller + interrupt-controller
    ├── Requests GIC SPI 208 as summary IRQ
    └── Installs chained IRQ handler for GPIO demux
```

---

## Interrupt Flow (Runtime)

```
Hardware event (e.g., I2C transfer complete):
─────────────────────────────────────────────
1. I2C QUP3 controller asserts SPI 97
2. GICD receives SPI 97
3. GICD checks: is it enabled? → YES
4. GICD checks priority → above threshold
5. GICD routes to target CPU (usually CPU0 or any online CPU)
6. GICR on target CPU presents interrupt
7. CPU takes IRQ exception → vector table → IRQ handler

Linux IRQ handling:
8. gic_handle_irq() reads IAR register → gets IRQ ID (129)
9. Maps to Linux IRQ number via irq_domain
10. Calls registered handler: i2c_qup_interrupt()
11. Handler processes I2C completion
12. EOI (End of Interrupt) written to GIC
```

---

## Debugging Interrupts

```bash
# View all IRQs and their counts
adb shell cat /proc/interrupts

# Example output:
#            CPU0  CPU1  CPU2  CPU3  CPU4  CPU5  CPU6  CPU7
#  129:       42     0     0     0     0     0     0     0  GICv3  97 Level  78b7000.i2c
#  208:       15     0     0     0     0     0     0     0  GICv3  208 Level  tlmm
#  350:        3     0     0     0     0     0     0     0  tlmm   23 Edge   bmi160

# Check IRQ affinity (which CPUs can handle it)
adb shell cat /proc/irq/129/smp_affinity_list
# 0-7 (any CPU)

# Spurious interrupt check
adb shell cat /proc/interrupts | grep ERR
# ERR: 0
```

---

## Related Documents

- [02_Device_Tree_Processing.md](02_Device_Tree_Processing.md) — Interrupt DT bindings
- [04_Pinctrl_TLMM.md](04_Pinctrl_TLMM.md) — GPIO interrupts through TLMM
- [07_PSCI_CPU_Bringup.md](07_PSCI_CPU_Bringup.md) — IPI for secondary CPU boot
