# Day 05 — GICv2 + ARM Generic Timer (Phase 1 Gate)

> **Goal**: Initialize the Generic Interrupt Controller v2 (distributor + CPU interface), enable the ARM Generic Timer's EL1 physical timer (PPI 30), and tick at 100 Hz with a counter visible on the serial console.
>
> **Why today**: Preemptive scheduling (Day 13) requires a periodic timer IRQ delivered through a real interrupt controller. Doing GIC + timer together avoids a useless intermediate state.

---

## 1. Background

### 1.1 GICv2 layout (QEMU virt)
- **Distributor (GICD)** at `0x0800_0000`: enables/priorities/targets per IRQ globally.
- **CPU interface (GICC)** at `0x0801_0000`: ack/EOI per-CPU.
- IRQ space:
  - **0–15**: SGI (software-generated, inter-processor)
  - **16–31**: PPI (private per-CPU — timer = 30)
  - **32–1019**: SPI (shared peripheral — UART = 33, virtio-mmio = 47..78)

### 1.2 Key GICD registers
| Offset | Name | Use |
|---|---|---|
| `0x000` | `CTLR` | Distributor enable |
| `0x080`+4·n | `IGROUPRn` | Group 0/1 |
| `0x100`+4·n | `ISENABLERn` | Set-enable (1 bit per IRQ) |
| `0x180`+4·n | `ICENABLERn` | Clear-enable |
| `0x400`+4·n | `IPRIORITYRn` | 8-bit priority per IRQ |
| `0x800`+4·n | `ITARGETSRn` | CPU target bitmap |
| `0xC00`+4·n | `ICFGRn` | Edge/level (2 bits/IRQ) |

### 1.3 Key GICC registers
| Offset | Name | Use |
|---|---|---|
| `0x000` | `CTLR` | CPU iface enable |
| `0x004` | `PMR` | Priority mask (set 0xFF = allow all) |
| `0x00C` | `IAR` | Read = ack (returns IRQ id) |
| `0x010` | `EOIR` | Write = end-of-interrupt |

### 1.4 ARM Generic Timer (EL1 physical)
- Sysregs: `CNTFRQ_EL0`, `CNTPCT_EL0`, `CNTP_CVAL_EL0`, `CNTP_TVAL_EL0`, `CNTP_CTL_EL0`.
- IRQ: PPI 30 (`IRQ_PHYS_TIMER`).
- Strategy: write `CNTP_TVAL_EL0 = freq/HZ`, set `CNTP_CTL_EL0.ENABLE=1`. On IRQ, reload TVAL.

---

## 2. Design

### 2.1 New files
```
arch/arm64/kernel/gic.c
arch/arm64/kernel/timer.c
include/asm-arm64/gic.h
include/asm-arm64/timer.h
include/kernel/irq.h
```

### 2.2 IRQ dispatch
```
el1_irq vector  ──►  do_irq(pt_regs*)
                       └─ gic_ack()  → irq_nr
                         └─ irq_handlers[irq_nr](irq_nr)
                       └─ gic_eoi(irq_nr)
```

---

## 3. Implementation

### 3.1 `gic.c`
```c
#include <kernel/io.h>
#include <kernel/irq.h>
#include <kernel/printk.h>

#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL

#define GICD_CTLR      (GICD_BASE + 0x000)
#define GICD_ISENABLER (GICD_BASE + 0x100)
#define GICD_IPRIORITY (GICD_BASE + 0x400)
#define GICD_ITARGETS  (GICD_BASE + 0x800)
#define GICD_ICFGR     (GICD_BASE + 0xC00)

#define GICC_CTLR  (GICC_BASE + 0x000)
#define GICC_PMR   (GICC_BASE + 0x004)
#define GICC_IAR   (GICC_BASE + 0x00C)
#define GICC_EOIR  (GICC_BASE + 0x010)

#define NR_IRQS 256
static irq_handler_t handlers[NR_IRQS];

void gic_enable_irq(unsigned irq, unsigned prio, unsigned cpu)
{
    writel(prio, GICD_IPRIORITY + (irq & ~3) ); /* byte-level writes via word: simplified */
    if (irq >= 32)
        writel(1u << (cpu & 7), GICD_ITARGETS + (irq & ~3));
    writel(1u << (irq & 31), GICD_ISENABLER + 4 * (irq / 32));
}

void gic_init(void)
{
    writel(0, GICD_CTLR);
    /* mask all priorities = allow */
    writel(0xff, GICC_PMR);
    /* enable CPU iface and distributor */
    writel(1, GICC_CTLR);
    writel(1, GICD_CTLR);
    printk(KERN_INFO "GICv2 initialized\n");
}

int irq_register(unsigned irq, irq_handler_t h)
{
    if (irq >= NR_IRQS) return -1;
    handlers[irq] = h;
    return 0;
}

/* Called from entry.S via do_irq */
void gic_handle(void)
{
    u32 iar  = readl(GICC_IAR);
    u32 irq  = iar & 0x3ff;
    if (irq == 1023) return;            /* spurious */
    if (handlers[irq]) handlers[irq](irq);
    else printk(KERN_WARN "unhandled IRQ %u\n", irq);
    writel(iar, GICC_EOIR);
}
```
Update `do_irq()` (from Day 4) to call `gic_handle()`.

### 3.2 `timer.c`
```c
#include <asm-arm64/timer.h>
#include <kernel/irq.h>
#include <kernel/printk.h>

#define HZ 100
#define IRQ_PHYS_TIMER 30

static u64 timer_freq;
static volatile u64 jiffies;

static inline u64 read_cntfrq(void)
{ u64 v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
static inline void write_tval(u64 v)
{ asm volatile("msr cntp_tval_el0, %0" :: "r"(v)); }
static inline void write_ctl(u64 v)
{ asm volatile("msr cntp_ctl_el0, %0" :: "r"(v)); }

static void timer_tick(unsigned irq)
{
    (void)irq;
    write_tval(timer_freq / HZ);            /* reload */
    jiffies++;
    if ((jiffies % HZ) == 0)
        printk(KERN_INFO "tick %lu (1s)\n", jiffies / HZ);
}

void timer_init(void)
{
    timer_freq = read_cntfrq();
    printk(KERN_INFO "Timer: %lu Hz, HZ=%d\n", timer_freq, HZ);
    irq_register(IRQ_PHYS_TIMER, timer_tick);
    gic_enable_irq(IRQ_PHYS_TIMER, 0, 0);
    write_tval(timer_freq / HZ);
    write_ctl(1);                            /* ENABLE=1, IMASK=0 */
}

u64 get_jiffies(void) { return jiffies; }
```

### 3.3 Enable IRQs at end of `kmain`
```c
gic_init();
timer_init();
asm volatile("msr daifclr, #2");   // unmask IRQ
printk(KERN_INFO "IRQs enabled\n");
for (;;) asm volatile("wfi");      // wake on IRQ
```

---

## 4. ARM64 Cheat-Sheet (Day 5)

```
CNTFRQ_EL0    counter frequency (Hz, RO from EL1, written by firmware)
CNTP_TVAL_EL0 down-count from this many ticks until IRQ
CNTP_CTL_EL0  bit0 ENABLE, bit1 IMASK, bit2 ISTATUS
daifclr #2    clear PSTATE.I  (enable IRQ)
daifset #2    set   PSTATE.I  (disable IRQ)
wfi           wait for interrupt
```

---

## 5. Pitfalls

1. **PPI vs SPI numbering**: PPI 30 ↔ GIC IRQ 30 (NOT 30+32). Don't add 32.
2. **`ITARGETSR` for PPI**: PPIs are per-CPU; bytes 0–31 of `ITARGETSR` are RO. Don't write — only set for SPI ≥ 32.
3. **Forgetting `eoir`**: handler runs forever (re-entered immediately by GIC). Always EOI even on spurious.
4. **`CNTP_CTL_EL0.IMASK`**: set after handling reduces spurious in race with EOI on slow handlers. We rely on hardware-level edge.
5. **Endless tick storms**: if your handler takes longer than HZ period, you re-enter. Add a "lost tick" counter Day 13.

---

## 6. Verification (Phase 1 Gate)

```
make run
# Expect (steady state):
# [INFO] GICv2 initialized
# [INFO] Timer: 62500000 Hz, HZ=100
# [INFO] IRQs enabled
# [INFO] tick 100 (1s)
# [INFO] tick 200 (1s)   ... every second
```

CI marker: `tick 500 \(1s\)` reached within 8 wall-seconds.

GDB: `b timer_tick`, `c`, verify firing every ~10 ms of guest time.

---

## 7. Stretch

- Switch to the **virtual** timer (`CNTV_*`) — Linux uses virtual for guest portability.
- Implement `udelay(us)` via `CNTPCT_EL0` spin-wait — needed by virtio reset Day 22.
- Hook UART IRQ (SPI 33) to a ring buffer — prep for Day 26 shell input.

---

## 8. References

- *ARM Generic Interrupt Controller Architecture Specification, v2.0* (IHI 0048).
- ARM ARM §D11 (Generic Timer).
- QEMU `hw/intc/arm_gic.c`, `hw/timer/arm_mptimer.c`.
