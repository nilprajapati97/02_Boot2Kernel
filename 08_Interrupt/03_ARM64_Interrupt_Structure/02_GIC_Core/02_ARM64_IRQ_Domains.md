This is one of the most important Linux ARM64 interrupt concepts for kernel, BSP, GIC, PCIe MSI, and virtualization interviews.

The confusion usually comes because people mix up:

1. **Hardware interrupt numbers (HWIRQ)**
2. **Linux IRQ numbers (virq)**
3. **IRQ domains**
4. **IRQ chips**
5. **Interrupt hierarchy**
6. **IRQ routing / mapping**
7. **Device Tree interrupt specifiers**
8. **MSI domains**

Let's build it from scratch.

---

# 1. The Fundamental Problem

Imagine a simple ARM64 SoC.

```text
UART ----+
         |
GPIO ----+---- GIC ---- CPU
         |
I2C  ----+
```

The GIC receives interrupt signals.

Hardware interrupt IDs might be:

```text
UART -> SPI 33
GPIO -> SPI 45
I2C  -> SPI 62
```

where SPI means Shared Peripheral Interrupt.

---

## Hardware View

The GIC knows:

```text
Interrupt 33
Interrupt 45
Interrupt 62
```

Only hardware numbers.

---

## Linux View

Linux uses:

```c
request_irq(120,...);
request_irq(121,...);
request_irq(122,...);
```

Linux doesn't want drivers depending on hardware interrupt numbers.

Because:

```text
Board A:
UART -> SPI 33

Board B:
UART -> SPI 95
```

Same UART driver must work on both boards.

---

# Problem Without IRQ Domains

Suppose Linux directly used hardware interrupt IDs.

```text
UART Driver
    |
request_irq(33)
```

Works on Board A.

Fails on Board B.

Now every driver becomes board-specific.

Not scalable.

---

# Solution: Virtual IRQ Numbers

Linux creates:

```text
Hardware IRQ      Linux IRQ

33        ----->   120
45        ----->   121
62        ----->   122
```

Drivers only see:

```c
request_irq(120,...);
```

not

```c
request_irq(33,...);
```

This mapping is managed by IRQ domains.

---

# 2. What is an IRQ Domain?

An IRQ domain is simply:

> A translation layer between hardware interrupt numbers and Linux virtual IRQ numbers.

```text
HWIRQ
  |
  v
IRQ Domain
  |
  v
Linux IRQ (virq)
```

Example:

```text
HWIRQ 33 -> VIRQ 120
HWIRQ 45 -> VIRQ 121
HWIRQ 62 -> VIRQ 122
```

Kernel stores:

```c
struct irq_domain
```

for this purpose.

---

# Why Linux Needs It

Different interrupt controllers use different numbering.

Example:

```text
GIC:
  SPI 33

GPIO Controller:
  GPIO IRQ 5

ITS:
  EventID 10

PCIe:
  MSI EventID 4
```

Linux wants a single namespace:

```text
virq 100
virq 101
virq 102
```

IRQ domains provide this abstraction.

---

# 3. ARM64 GIC Example

Device Tree:

```dts
uart0 {
    interrupts = <0 33 4>;
};
```

Meaning:

```text
0 = SPI
33 = HWIRQ
4 = Level High
```

Kernel boot:

```text
GIC Driver
    creates IRQ domain
```

Then:

```text
HWIRQ 33
    ↓
irq_create_mapping()
    ↓
VIRQ 120
```

Driver receives:

```c
irq = platform_get_irq(pdev,0);
```

returns:

```text
120
```

Driver never sees 33.

---

# 4. IRQ Domain Data Structure

Conceptually:

```c
struct irq_domain {

    struct irq_chip *chip;

    map(hwirq -> virq);

    translate(dt_specifier);

};
```

Responsibilities:

### Translate DT interrupt specifier

```text
<0 33 4>
```

to

```text
HWIRQ 33
```

---

### Allocate Linux IRQ

```text
HWIRQ 33
    ↓
VIRQ 120
```

---

### Connect irq_chip

```text
irq_chip = gic_chip
```

---

# 5. What is irq_chip?

People confuse irq_domain and irq_chip.

They are different.

---

## irq_domain

Answers:

```text
Which Linux IRQ corresponds to HWIRQ 33?
```

Translation.

---

## irq_chip

Answers:

```text
How do I mask it?
How do I unmask it?
How do I EOI it?
```

Operations.

Example:

```c
struct irq_chip {

    irq_mask()
    irq_unmask()
    irq_ack()
    irq_eoi()

};
```

---

# Think of It Like

```text
IRQ Domain = Address Book

irq_chip = Hardware Driver
```

---

# 6. Hierarchical IRQ Domains

Modern ARM64 systems have many interrupt controllers.

Example:

```text
GPIO Controller
       |
       v
      GIC
       |
       v
      CPU
```

GPIO interrupt arrives.

```text
Button
  |
GPIO Pin 7
  |
GPIO Controller
  |
GIC SPI 45
  |
CPU
```

Now we have two interrupt numbers.

```text
GPIO IRQ 7
GIC IRQ 45
```

Need two domains.

---

# Hierarchical Domains

```text
GPIO Domain
      |
      v
GIC Domain
      |
      v
Linux IRQ
```

Hierarchy:

```text
GPIO HWIRQ 7
      |
      v
Parent HWIRQ 45
      |
      v
VIRQ 120
```

---

# Why Hierarchical Domains?

Without them:

```text
Every GPIO driver
must understand GIC internals.
```

Bad design.

Instead:

```text
GPIO Driver
     |
GPIO Domain
     |
GIC Domain
```

Each layer manages itself.

---

# 7. MSI Domains

Now consider PCIe MSI.

Earlier:

```text
GPIO -> HWIRQ
```

Easy.

MSI is different.

Device generates:

```text
(DeviceID, EventID)
```

not

```text
SPI 33
```

Example:

```text
NVMe

DeviceID = 10
EventID = 4
```

GIC doesn't understand this.

ITS translates.

---

# MSI Hierarchy

```text
PCIe Device
      |
 MSI Domain
      |
 ITS Domain
      |
 GIC Domain
      |
 CPU
```

Each layer has its own domain.

---

Example:

```text
(DeviceID=10, EventID=4)

      ↓

LPI 8192

      ↓

VIRQ 300
```

---

# 8. What are IRQ Options (Trigger Types)?

Interrupts can behave differently electrically.

Linux stores interrupt type.

Common values:

```c
IRQ_TYPE_LEVEL_HIGH
IRQ_TYPE_LEVEL_LOW

IRQ_TYPE_EDGE_RISING
IRQ_TYPE_EDGE_FALLING

IRQ_TYPE_EDGE_BOTH
```

---

## Level Interrupt

Signal remains active.

```text
______
      |
      |
______|
```

CPU handles until device clears.

Examples:

```text
UART
Ethernet
Storage
```

---

## Edge Interrupt

Only transition matters.

```text
___|‾‾‾
```

Interrupt generated once.

Examples:

```text
Button
GPIO event
```

---

# Problem Without Trigger Types

Suppose hardware is:

```text
Level High
```

But Linux configures:

```text
Edge Rising
```

Result:

```text
Interrupt storm
or
Lost interrupt
```

System misbehaves.

---

# Device Tree Example

```dts
interrupts = <0 33 4>;
```

where:

```text
1 = edge rising
2 = edge falling
4 = level high
8 = level low
```

The domain translate callback decodes this.

---

# Complete ARM64 Interrupt Flow

UART generates interrupt.

```text
UART
 |
 | HWIRQ 33
 v
GIC
 |
 v
IRQ Domain
 |
 | 33 -> 120
 v
Linux IRQ 120
 |
 v
generic_handle_irq()
 |
 v
UART ISR
```

---

# MSI Flow

```text
NVMe
 |
 | MSI(DeviceID=10, EventID=4)
 v
ITS
 |
 | LPI 8192
 v
GIC
 |
 v
IRQ Domain
 |
 v
Linux IRQ 300
 |
 v
NVMe ISR
```

---

# Interview-Level Summary

### What is IRQ Domain?

> IRQ Domain is a Linux kernel abstraction that maps hardware interrupt identifiers (hwirq) from an interrupt controller into Linux virtual IRQ numbers (virq).

---

### Why is it needed?

Without IRQ domains:

* Drivers would depend on board-specific interrupt numbers.
* Multiple interrupt controllers couldn't coexist cleanly.
* MSI and hierarchical interrupt controllers would be difficult to support.

---

### What is irq_chip?

> irq_chip contains hardware-specific operations such as mask, unmask, ack, EOI, and set_type.

---

### What is Hierarchical IRQ Domain?

> A hierarchy of interrupt domains where child interrupt controllers (GPIO, MSI, ITS) are layered on top of parent controllers (GIC).

---

### Why is ITS related to IRQ domains?

> ITS receives MSI messages containing DeviceID and EventID, translates them into LPIs, and uses hierarchical IRQ domains so Linux can map those LPIs into virtual IRQs.

---

### One sentence for interviews

> "IRQ domains separate Linux IRQ numbering from hardware interrupt numbering, enabling portable drivers, hierarchical interrupt controllers, MSI support, and scalable interrupt management on ARM64 systems."
