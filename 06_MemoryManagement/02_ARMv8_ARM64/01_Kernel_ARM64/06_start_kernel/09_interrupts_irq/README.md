# Phase 9: Interrupts and IRQ Initialization

## Overview

This phase sets up the full interrupt handling infrastructure and then enables hardware interrupts for the first time since boot. It's a landmark moment: before `local_irq_enable()`, the CPU has been running in a completely interrupt-free environment since the bootloader handoff.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`early_irq_init()`](early_irq_init/README.md) | `kernel/irq/irqdesc.c` | Allocate IRQ descriptor table |
| 2 | [`init_IRQ()`](init_IRQ/README.md) | `arch/x86/kernel/irqinit.c` | Architecture IRQ setup (APIC/PIC) |
| 3 | [`tick_init()`](tick_init/README.md) | `kernel/time/tick-common.c` | Initialize timer tick framework |
| 4 | [`rcu_init_tasks_generic()`](../08_rcu/README.md) | `kernel/rcu/tasks.trace.c` | Tasks-RCU setup |
| 5 | [`softirq_init()`](softirq_init/README.md) | `kernel/softirq.c` | Initialize software interrupt system |
| 6 | `local_irq_enable()` | `arch/x86/include/asm/irqflags.h` | **Enable hardware interrupts** |

## IRQ State

- **Entry**: **DISABLED** (as it has been since CPU reset)
- **After `local_irq_enable()`**: **ENABLED** ← first time in boot

## The IRQ Enable Moment

```c
// In start_kernel(), approximately line 1000:
local_irq_enable();
// From this point forward: hardware interrupts can arrive
// The timer interrupt will fire ~1ms later (jiffies starts counting)
```

## What These Calls Set Up

### IRQ Descriptor Table (`early_irq_init`)

The kernel's software representation of all interrupt lines (not the hardware IDT). Each `irq_desc` maps a Linux IRQ number to its handler chain.

### APIC/PIC (`init_IRQ`)

On x86: programs the APIC (Advanced Programmable Interrupt Controller) to route hardware interrupt lines to CPU vectors. This connects physical interrupt lines (PCI, USB, etc.) to IDT vectors.

### Tick Framework (`tick_init`)

Sets up the `tick_device` per CPU — the per-CPU timer that drives the scheduler tick, jiffies update, and timer wheel.

### Softirq (`softirq_init`)

Initializes the software interrupt mechanism — deferred interrupt processing for networking (`NET_RX_SOFTIRQ`), timers (`TIMER_SOFTIRQ`), block I/O (`BLOCK_SOFTIRQ`), etc.

## Function Index

- [early_irq_init/](early_irq_init/README.md)
- [init_IRQ/](init_IRQ/README.md)
- [tick_init/](tick_init/README.md)
- [softirq_init/](softirq_init/README.md)
