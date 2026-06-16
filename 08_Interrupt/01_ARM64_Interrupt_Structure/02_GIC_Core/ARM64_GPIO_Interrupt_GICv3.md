Below is a kernel-engineer-level explanation of a GPIO interrupt on ARM64 with GICv3 and Linux, starting from power-on initialization through interrupt handling and return to userspace/kernel code.

---

# ARM64 Linux Interrupt Flow (GPIO Example)

## Example System

Assume:

* ARM Cortex-A76 SoC
* GICv3 Interrupt Controller
* Linux Kernel
* GPIO controller generates interrupt
* GPIO line 12 connected to a button
* GPIO controller output connected to GIC SPI 89

```text
Button Press
    |
    v
GPIO Controller
    |
IRQ Output
    |
    v
GIC SPI 89
    |
    v
CPU2
    |
Linux IRQ Handler
```

---

# Part 1: Linux Boot Initialization

Before any interrupt can occur, Linux must initialize the GIC.

---

## Step 1: Device Tree Discovery

Linux parses Device Tree.

Example:

```dts
gic: interrupt-controller@8000000 {
        compatible = "arm,gic-v3";
        interrupt-controller;
        #interrupt-cells = <3>;
        reg = <0x0 0x08000000 0 0x10000>,
              <0x0 0x080A0000 0 0x200000>;
};
```

Kernel detects:

```text
GIC Version = GICv3
Distributor Base = 0x08000000
Redistributor Base = 0x080A0000
```

Driver loaded:

```text
drivers/irqchip/irq-gic-v3.c
```

---

## Step 2: GIC Distributor Initialization

Linux configures Distributor.

Important registers:

| Register         | Purpose            |
| ---------------- | ------------------ |
| GICD_CTLR        | Distributor enable |
| GICD_TYPER       | Number of IRQs     |
| GICD_ISENABLERn  | Enable IRQ         |
| GICD_ICENABLERn  | Disable IRQ        |
| GICD_IPRIORITYRn | Priority           |
| GICD_ICFGRn      | Edge/Level         |
| GICD_IROUTERn    | CPU routing        |

Linux reads:

```c
GICD_TYPER
```

Example result:

```text
1020 Interrupts Supported
```

Then:

```c
GICD_CTLR = Enable
```

Distributor starts accepting interrupts.

---

## Step 3: Redistributor Initialization

Every CPU has a Redistributor.

```text
CPU0 -> GICR0
CPU1 -> GICR1
CPU2 -> GICR2
CPU3 -> GICR3
```

Linux initializes:

```text
GICR_CTLR
GICR_WAKER
```

Wake redistributors.

---

## Step 4: CPU Interface Setup

Linux enables CPU interrupt interface.

System registers:

```text
ICC_SRE_EL1
ICC_CTLR_EL1
ICC_PMR_EL1
ICC_IGRPEN1_EL1
```

Example:

```text
ICC_PMR_EL1 = 0xff
```

Accept all priorities.

Enable interrupt groups:

```text
ICC_IGRPEN1_EL1 = 1
```

Now CPU can receive interrupts.

---

## Step 5: Exception Vector Installation

Linux installs exception vectors.

Register:

```text
VBAR_EL1
```

contains:

```text
Linux Exception Vector Address
```

Example:

```assembly
msr VBAR_EL1, vector_table
```

Now IRQ exceptions enter Linux.

---

# Part 2: GPIO Driver Initialization

Assume GPIO driver probes.

Driver requests IRQ.

```c
irq = platform_get_irq(pdev, 0);

request_irq(
        irq,
        gpio_button_irq,
        IRQF_TRIGGER_RISING,
        "button",
        dev);
```

---

## IRQ Domain Mapping

Linux maintains:

```text
Hardware IRQ -> Linux IRQ
```

Example:

```text
GIC SPI 89
      |
      v
Linux IRQ 245
```

Stored in:

```c
struct irq_desc
```

---

## Configure Trigger Type

Linux programs GIC.

Register:

```text
GICD_ICFGR
```

Example:

```text
SPI89 = Edge Rising
```

---

## Enable Interrupt

Linux writes:

```text
GICD_ISENABLER
```

Example:

```text
Bit 89 = 1
```

Interrupt now armed.

---

# Part 3: Actual Interrupt Occurs

User presses button.

---

## Step 1: GPIO Detects Event

GPIO hardware sees:

```text
LOW -> HIGH
```

GPIO controller sets internal status.

Example register:

```text
GPIO_INT_STATUS
```

```text
Bit12 = 1
```

Then GPIO controller asserts IRQ output.

```text
GPIO ----> GIC
```

Linux still unaware.

---

## Step 2: GIC Receives Interrupt

Interrupt enters Distributor.

SPI number:

```text
89
```

GIC checks:

```text
Enabled?
Priority?
Routing?
```

Registers involved:

```text
GICD_ISENABLER
GICD_IPRIORITYR
GICD_IROUTER
```

---

## Step 3: GIC Marks Pending

State transition:

```text
Inactive
    |
    v
Pending
```

Internal pending bit set.

Visible through:

```text
GICD_ISPENDR
```

```text
SPI89 Pending = 1
```

---

# Part 4: CPU Selection

Now GIC decides destination CPU.

---

## Routing Register

GICv3 uses:

```text
GICD_IROUTER89
```

Example:

```text
Target = CPU2
```

Affinity value:

```text
Aff3 Aff2 Aff1 Aff0
```

stored in IROUTER.

Example:

```text
CPU2
```

So interrupt goes only to CPU2.

---

## Affinity in Linux

Linux may have configured:

```bash
cat /proc/irq/245/smp_affinity
```

Example:

```text
00000004
```

Means:

```text
CPU2 only
```

Linux wrote corresponding routing into GIC.

---

# Part 5: Interrupt Signal to CPU

GIC sends interrupt request.

```text
GIC
 |
IRQ Exception
 |
CPU2
```

CPU checks:

```text
PSTATE.I
```

If:

```text
PSTATE.I = 0
```

interrupts enabled.

CPU accepts interrupt.

---

# Part 6: ARM64 Hardware Exception Entry

Pure hardware operation.

Linux not running yet.

CPU automatically saves:

```text
ELR_EL1
```

Return address.

Example:

```text
0xffff800010123456
```

---

CPU also saves:

```text
SPSR_EL1
```

Processor state.

Example:

```text
Interrupt masks
Condition flags
Current EL
```

---

CPU jumps to:

```text
VBAR_EL1 + IRQ Vector Offset
```

Entering Linux.

---

# Part 7: Linux Low-Level IRQ Entry

File:

```text
arch/arm64/kernel/entry.S
```

Entry point:

```assembly
el1_irq
```

Flow:

```text
el1_irq
   |
kernel_entry
   |
irq_handler
```

---

## Save Context

Linux pushes:

```text
x0-x30
SP
PSTATE
```

onto current task stack.

Current task suspended.

---

# Part 8: Linux Reads Interrupt Number

Now Linux must identify source.

GIC CPU Interface register:

```text
ICC_IAR1_EL1
```

Interrupt Acknowledge Register.

Linux executes:

```assembly
mrs x0, ICC_IAR1_EL1
```

Example result:

```text
89
```

Meaning:

```text
SPI89 triggered
```

State transition:

```text
Pending -> Active
```

---

# Part 9: GIC Driver Handling

Function:

```c
gic_handle_irq()
```

File:

```text
drivers/irqchip/irq-gic-v3.c
```

Receives:

```text
HWIRQ = 89
```

Calls:

```c
generic_handle_domain_irq()
```

---

# Part 10: IRQ Domain Translation

Linux converts:

```text
HW IRQ 89
```

to

```text
Virtual IRQ 245
```

using:

```c
struct irq_domain
```

Framework.

```text
89 ---> 245
```

---

# Part 11: Generic IRQ Layer

Linux locates:

```c
irq_desc[245]
```

Structure:

```c
struct irq_desc
```

Contains:

```text
handler
state
affinity
statistics
action list
```

---

# Part 12: Flow Handler Selection

Based on trigger type.

For edge interrupt:

```c
handle_edge_irq()
```

For level interrupt:

```c
handle_level_irq()
```

Selected during IRQ setup.

---

# Part 13: Driver ISR Execution

Linux eventually calls:

```c
gpio_button_irq()
```

Example:

```c
irqreturn_t gpio_button_irq(
        int irq,
        void *dev)
{
        status = readl(GPIO_INT_STATUS);

        writel(status,
               GPIO_INT_CLEAR);

        return IRQ_HANDLED;
}
```

Now driver code executes.

---

# Part 14: Device Interrupt Clear

Critical step.

Driver clears GPIO interrupt.

Register:

```text
GPIO_INT_CLEAR
```

Without this:

```text
IRQ line remains asserted
```

leading to interrupt storm.

---

# Part 15: Interrupt Statistics Update

Linux updates:

```text
/proc/interrupts
```

Example:

```text
245: 1024  GPIO Button
```

Counter incremented.

---

# Part 16: End Of Interrupt (EOI)

Linux tells GIC:

```text
Interrupt processing complete
```

Register:

```text
ICC_EOIR1_EL1
```

Linux writes:

```assembly
msr ICC_EOIR1_EL1, x0
```

where:

```text
x0 = interrupt ID 89
```

State transition:

```text
Active -> Inactive
```

---

## Optional Deactivate

Some GIC modes additionally use:

```text
ICC_DIR_EL1
```

Deactivate register.

Final state:

```text
Inactive
```

Ready for next interrupt.

---

# Part 17: IRQ Exit

Linux executes:

```c
irq_exit()
```

Checks:

* softirq pending?
* task reschedule needed?
* timer events?

May run:

```text
NET_RX softirq
RCU callbacks
Task scheduler
```

before returning.

---

# Part 18: Context Restore

Linux restores:

```text
x0-x30
SP
PSTATE
```

from stack.

---

# Part 19: Return to Interrupted Code

ARM instruction:

```assembly
eret
```

uses:

```text
ELR_EL1
SPSR_EL1
```

to restore execution.

CPU resumes exactly where it was interrupted.

---

# Complete End-to-End Timeline

```text
1. Button Pressed
        |
2. GPIO detects edge
        |
3. GPIO_INT_STATUS set
        |
4. GPIO asserts IRQ output
        |
5. GIC Distributor receives SPI89
        |
6. GICD_ISPENDR bit set
        |
7. GIC checks priority
        |
8. GIC checks GICD_IROUTER89
        |
9. Routes to CPU2
        |
10. CPU2 receives IRQ exception
        |
11. Hardware saves ELR_EL1/SPSR_EL1
        |
12. Jump to VBAR_EL1
        |
13. Linux el1_irq entry
        |
14. Save registers
        |
15. Read ICC_IAR1_EL1
        |
16. Get HWIRQ 89
        |
17. gic_handle_irq()
        |
18. irq_domain translation
        |
19. HWIRQ89 -> Linux IRQ245
        |
20. handle_edge_irq()
        |
21. gpio_button_irq()
        |
22. Clear GPIO_INT_STATUS
        |
23. Return IRQ_HANDLED
        |
24. Write ICC_EOIR1_EL1
        |
25. irq_exit()
        |
26. Restore registers
        |
27. eret
        |
28. Original code resumes
```

This is the exact path a kernel engineer typically traces while debugging ARM64 GPIO interrupts using GICv3, `/proc/interrupts`, ftrace, irq_domain, and the `irq-gic-v3.c` driver.
