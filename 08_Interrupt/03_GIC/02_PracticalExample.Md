Great — let’s do one concrete, end-to-end example and follow the interrupt from the moment a peripheral raises it, through the GIC, into the CPU, and back out after the ISR finishes.
I’ll use a **UART RX interrupt** on an ARM SoC with a GIC (typical real-world example). I’ll show both the **hardware/GIC** view and the **Linux driver** view (plus a tiny bare-metal GIC snippet). Ready — deep dive below.

# Example overview

* **Peripheral:** UART (data received into FIFO → raises RX interrupt).
* **Interrupt type:** SPI (Shared Peripheral Interrupt) — has a global ID on the GIC (e.g., IRQ ID **45** in this example).
* **Controller:** ARM GIC (Distributor + CPU interface).
* **Target CPU:** CPU0 (chosen by GICD\_ITARGETSR or by default routing).
* **Software:** Linux kernel with a UART device driver registered for IRQ 45.

# 1) High level block flow (sequence)

```
[UART peripheral] --(IRQ line)--> [GIC Distributor (GICD)] --(selected)--> [GIC CPU Interface (GICC)] --(signal)--> [CPU core exception] --> [Kernel IRQ core] --> [Driver ISR] --> [Write EOI to GICC] --> [GIC clears active/pending]
```

# 2) Hardware / GIC detailed timeline (step-by-step)

1. **Peripheral asserts IRQ (hardware)**

   * UART receives data; UART sets a “data ready” flag and asserts its IRQ line (for level IRQ: holds line high until cleared by device read).
2. **GIC Distributor (GICD) latches IRQ**

   * Distributor detects the asserted interrupt and sets the corresponding **Pending** bit.
   * GICD checks if the interrupt is **enabled** (GICD\_ISENABLER). If disabled → it stays pending but won’t be forwarded.
3. **Priority resolution & target selection**

   * GICD reads the priority (GICD\_IPRIORITYR for that IRQ).
   * GICD reads **ITARGETSR** to know which CPU(s) can handle it (affinity/route). For SPIs you configure which CPU(s) to deliver to. In our example it routes to **CPU0**.
4. **GIC CPU Interface checks priority threshold**

   * On CPU0 the **GICC\_PMR** register holds a priority cutoff: only interrupts with higher urgency (lower numeric priority value) are signaled. If priority passes, GICC asserts the CPU IRQ line.
5. **CPU receives interrupt (exception entry)**

   * CPU finishes current instruction, saves necessary state (stack/frame behavior depends on architecture/exception model), and jumps to the IRQ exception vector.
6. **Kernel IRQ core / GIC acknowledge**

   * The kernel’s low-level IRQ entry code reads **GICC\_IAR** (Interrupt Acknowledge Register) to obtain the **Interrupt ID** (the GIC’s identifier for the active interrupt, e.g., 45). This simultaneously tells the GIC which IRQ the CPU is handling.
7. **Top half—dispatch to driver ISR**

   * Kernel looks up the registered handler for IRQ 45 (via irq\_desc/irqaction) and calls the driver’s top-half ISR.
8. **Driver services device**

   * ISR does minimal, latency-sensitive work: e.g., read all bytes from UART RBR into an in-kernel buffer (reading clears the UART’s “data ready” condition). It may schedule a bottom half (workqueue/tasklet) for heavy processing.
9. **End Of Interrupt (EOI)**

   * After the ISR returns, kernel writes **GICC\_EOIR** with the interrupt ID to inform the GIC the CPU is finished with this interrupt. GIC marks the interrupt inactive and can de-assert or reissue if still pending.
10. **If more interrupts pending**

* If other interrupts are pending, GIC will evaluate and signal them (higher priority may preempt if allowed).

# 3) Important GIC concepts / registers (what to mention in an interview)

* **GICD\_ISENABLER / GICD\_ICENABLER** → enable/disable interrupts.
* **GICD\_IPRIORITYR** → per-IRQ priority. Lower numeric value = higher urgency (implementation detail varies).
* **GICD\_ITARGETSR** → which CPU(s) the SPI will be delivered to.
* **GICC\_PMR** → CPU priority mask: interrupts with lower priority than PMR are filtered out.
* **GICC\_IAR** → CPU reads to acknowledge and get interrupt ID.
* **GICC\_EOIR** → CPU writes to signal end of interrupt.
* **Interrupt ranges** (typical for GICv2): SGIs 0–15, PPIs 16–31 (private to CPU), SPIs 32+ (shared).

# 4) Example: Linux UART driver flow (concise code + explanation)

Below is a **simplified** Linux kernel ISR pattern (not a full driver). It demonstrates how a driver registers and handles a UART RX interrupt.

```c
/* Simplified kernel-style handler (conceptual) */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/io.h>

#define UART_IER   0x01 /* interrupt enable reg offset (example) */
#define UART_IIR   0x02 /* interrupt id reg offset (example) */
#define UART_RBR   0x00 /* receive buffer reg offset (example) */
#define UART_LSR   0x05 /* line status (example) */

struct my_uart {
    void __iomem *mmio;    /* mapped register base */
    int irq;               /* irq number (e.g., 45) */
    /* add buffers / kfifo etc. */
};

/* top-half: minimal work */
static irqreturn_t my_uart_irq(int irq, void *dev_id)
{
    struct my_uart *u = dev_id;
    u8 iir;

    /* read interrupt-id register to discover cause */
    iir = ioread8(u->mmio + UART_IIR);

    /* while RX ready, read bytes into buffer (clears UART's interrupt) */
    while (ioread8(u->mmio + UART_LSR) & /*LSR_RX_READY*/ 0x01) {
        u8 c = ioread8(u->mmio + UART_RBR);
        /* push into buffer / kfifo (very fast) */
    }

    /* schedule bottom-half if heavy processing needed */
    return IRQ_HANDLED;
}

static int __init myuart_init(void)
{
    int ret;
    struct my_uart *u = /* allocate & map mmio, platform_get_irq etc. */;
    /* ... */
    ret = request_irq(u->irq, my_uart_irq, IRQF_TRIGGER_HIGH, "my_uart", u);
    /* IRQF_TRIGGER_* often comes from device-tree; shown here for clarity */
    return ret;
}
```

Notes:

* Driver calls `request_irq()` (or `devm_request_irq()`) to register handler.
* The kernel abstracts GICC\_IAR/GICC\_EOIR — drivers don’t touch GIC registers directly. Kernel’s IRQ core handles acknowledge/EOI to the GIC.
* `ioread8`/`iowrite8` access UART registers; reading RBR typically clears the RX condition for level-triggered IRQs.

# 5) Bare-metal / low-level GIC acknowledge & EOI (concept)

On systems without a full OS, your IRQ entry often directly reads/writes the GIC CPU interface:

```c
/* Pseudo-code (bare-metal) */
uint32_t int_id = GICC_Read_IAR();   /* get active interrupt id */
if (int_id < 1020) {
    handlers[int_id]();              /* call C handler for that ID */
}
GICC_Write_EOIR(int_id);             /* signal end-of-interrupt */
```

* `GICC_Read_IAR()` returns the interrupt ID and some implementations mark it active.
* `GICC_Write_EOIR(int_id)` clears the active state.

# 6) Extra details & corner cases (what can break or complicate things)

* **Level vs Edge**: For *level* interrupts, the device must be cleared (e.g., read RBR or clear status) or the interrupt remains asserted and retriggers. For *edge*, the pulse is latched in GIC pending.
* **Spurious interrupts**: IAR may return a special value for spurious — kernel must handle.
* **Shared interrupts**: IRQF\_SHARED drivers must cooperate (multiple drivers on same irq line).
* **Nested interrupts / preemption**: GIC supports interrupt preemption based on priorities; a higher-priority interrupt can preempt a current lower-priority handler (if CPU and kernel allow).
* **Interrupt affinity**: `GICD_ITARGETSR` and Linux `irq_set_affinity()` control which CPU(s) receive the interrupt. Useful for load balancing or keeping IRQs on specific cores.
* **Priority masking**: If `GICC_PMR` is set high, lower-priority interrupts will be held off; used to protect critical sections.
* **EOI ordering**: Write EOI after you’ve finished the device-specific work that would reassert the interrupt; order matters to avoid lost interrupts or live-lock.

# 7) Debug checklist (practical steps)

* Verify device is asserting IRQ physically or logically (oscilloscope or pin toggles).
* `dmesg` / kernel logs — driver probe/IRQ messages.
* `cat /proc/interrupts` — see counts, assigned CPUs, and if IRQ is being served.
* Check GIC configuration (enable bits, target CPUs, priorities) in board init / device-tree.
* Make sure driver clears device interrupt source (e.g., read FIFO or clear status reg).

# 8) Interview whiteboard bullets (quick talking points)

* Start with "Peripheral -> Distributor -> CPU interface -> CPU vector -> IRQ top-half -> bottom-half -> EOI".
* Mention the `IAR`/`EOI` handshake and `ITARGETSR` routing.
* Show example IRQ numbers (SGI 0–15, PPI 16–31, SPI 32+).
* Explain how level IRQs require device clearing; edge IRQs do not.
* Mention nested interrupts, affinity, and use of threaded IRQs for long handlers.

# Short summary

* The GIC centralizes interrupts, performs priority/route decisions, and presents an IRQ to a CPU.
* The CPU/kernel acknowledges (IAR), driver handles minimal work, and the kernel writes EOI to finish (EOIR).
* Proper device clearing, priority configuration, and correct EOI ordering are critical to avoid missed or repeated interrupts.

---

If you want, I can now:

* draw a **clean whiteboard-style SVG/diagram** that you can use in interviews (peripheral → GICD → GICC → CPU → ISR → EOI), **or**
* produce a **full sample Linux UART driver** (longer, real code, platform resource mapping + DT support + bottom half) that you can compile/test — tell me which you'd prefer and I’ll produce it right away.
