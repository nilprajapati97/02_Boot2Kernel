# `local_irq_disable()` — Disabling Interrupts on Boot CPU

## Overview

| Attribute    | Value                                          |
|-------------|--------------------------------------------------|
| **Function** | `local_irq_disable(void)` then `early_boot_irqs_disabled = true` |
| **Source**   | `include/linux/irqflags.h`, arch-specific assembly |
| **Purpose**  | Explicitly ensure all local CPU interrupts are disabled; set the kernel flag that prevents IRQ enable/disable tracing during early boot |

---

## Why It Exists

By the time `start_kernel()` is called, interrupts are **usually** already disabled by the architecture-specific assembly prologue. However:

1. **Defensive programming**: `local_irq_disable()` makes the intent explicit and ensures the state is correct regardless of the arch
2. **Lockdep and IRQ tracing**: By setting `early_boot_irqs_disabled = true`, the kernel tells lockdep/IRQ tracing to **not complain** about IRQ state violations during early boot (when the IRQ state tracking structures are not yet set up)
3. **Documentation**: Makes the code self-documenting — readers know interrupts are intentionally off

---

## Implementation

```c
// start_kernel:
local_irq_disable();
early_boot_irqs_disabled = true;
```

### `local_irq_disable()` — x86 Implementation

```c
// arch/x86/include/asm/irqflags.h
static __always_inline void native_irq_disable(void)
{
    asm volatile("cli" : : : "memory");
}

#define local_irq_disable()             \
    do {                                \
        raw_local_irq_disable();        \
        trace_hardirqs_off();           \
    } while (0)
```

`cli` (Clear Interrupt Flag) sets IF=0 in the EFLAGS/RFLAGS register. Hardware will not deliver any maskable interrupt to this CPU until `sti` (Set Interrupt Flag) is called.

### ARM64 Implementation

```c
// arch/arm64/include/asm/irqflags.h
static __always_inline void arch_local_irq_disable(void)
{
    asm volatile(
        "msr    daifset, #3"    // set D, A, I, F bits in DAIF register
        : : : "memory");
}
```

DAIF = Debug, SError, IRQ, FIQ — setting all 4 masks all exceptions/interrupts.

### `early_boot_irqs_disabled` Flag

```c
// kernel/irq/irq.c
bool early_boot_irqs_disabled __read_mostly;
```

This flag is used by `trace_hardirqs_on()` / `trace_hardirqs_off()` to suppress spurious lockdep warnings:

```c
void trace_hardirqs_on(void)
{
    if (unlikely(early_boot_irqs_disabled))
        return;    // don't track IRQ state yet
    // ...
}
```

The flag is cleared at the end of Phase 8 before `local_irq_enable()`:
```c
early_boot_irqs_disabled = false;
local_irq_enable();
```

---

## Interrupt State Diagram

```
start_kernel() entry:
  RFLAGS.IF = 0 (from arch assembly prologue)
      │
      ▼
  local_irq_disable()          ← explicit, sets IF=0 again (idempotent)
  early_boot_irqs_disabled=true
      │
      ▼
  [All initialization phases 1-8 with IRQs disabled]
  [120+ function calls in this disabled window]
      │
      ▼
  early_boot_irqs_disabled = false    ← allow IRQ tracking
  local_irq_enable()                  ← cli → sti, IF=1
      │
      ▼
  [Rest of init with IRQs enabled: console_init, fork_init, rest_init...]
```

---

## Sub-Topics

- [01_flags_register_and_cli](01_flags_register_and_cli/README.md) — x86 RFLAGS register, CLI/STI instructions, and interrupt masking mechanisms

---

## Interview Q&A

### Q1: What is the difference between maskable and non-maskable interrupts (NMI)?
**A:** Maskable interrupts are regular device interrupts (keyboard, network card, timer) that can be disabled with `cli` (x86) or by setting DAIF mask bits (ARM). NMI (Non-Maskable Interrupt) cannot be masked with `cli` — it always arrives. On x86, NMI is used for: machine check exceptions (MCE), hardware watchdog timers, performance monitoring counters, and IPI-based NMIs for CPU hang detection. During early boot with `cli`, NMIs still arrive — this is why the NMI handler must be set up carefully and must be able to run with a potentially invalid kernel state.

### Q2: What happens if an interrupt arrives during the early boot IRQ-disabled window?
**A:** If it's maskable, it's **held pending** in the interrupt controller (APIC/GIC). The CPU won't execute the interrupt handler. When `local_irq_enable()` is finally called, all pending interrupts are delivered in order. This is expected behavior. For the first ~100ms of boot, no device interrupts matter — the kernel hasn't registered device drivers yet anyway.

For NMI: the NMI handler in the IDT (set up by `trap_init()`) handles it. Before `trap_init()`, NMIs would cause a triple fault. This is acceptable because early hardware rarely sends NMIs before the OS has initialized.

### Q3: What is `raw_local_irq_disable()` vs `local_irq_disable()`?
**A:** `local_irq_disable()` wraps the raw disable with `trace_hardirqs_off()` — which tracks the IRQ disable event for lockdep and IRQ state tracing. `raw_local_irq_disable()` only does the hardware operation (`cli` on x86) with no tracing. Use `raw_` variants in code that runs while IRQ tracing structures are not yet ready (early boot, NMI handlers) to avoid recursive calls into tracing code.
