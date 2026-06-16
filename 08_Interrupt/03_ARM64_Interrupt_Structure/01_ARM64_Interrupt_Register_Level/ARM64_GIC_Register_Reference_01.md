
## Complete End-to-End Flow: Hardware → GIC → Linux Kernel

### **Phase 1: What Hardware Does Automatically (ARM64 CPU)**

When an interrupt signal arrives at the CPU's `nIRQ` pin and `PSTATE.I == 0` (interrupts enabled):

```
1. CPU Hardware Actions (atomic, no software involvement):
   ├─ Save return address:    ELR_EL1 ← PC
   ├─ Save processor state:   SPSR_EL1 ← PSTATE
   ├─ Mask further IRQs:      PSTATE.I ← 1
   ├─ Switch to EL1:          If coming from EL0 (userspace)
   ├─ Use kernel stack:       SP ← SP_EL1
   └─ Jump to vector:         PC ← VBAR_EL1 + offset

Offset determination:
- From EL0 (userspace): VBAR_EL1 + 0x480 → el0_irq
- From EL1 (kernel):    VBAR_EL1 + 0x280 → el1_irq
```

**Key Point**: This is **CPU hardware behavior** — no software executes until the vector entry point.

---

### **Phase 2: GPIO Hardware → GIC Distributor**

```
GPIO Button Press (Example: GPIO pin 5 on PL061 controller)
   ↓
1. GPIO Controller Hardware:
   - Pin voltage drops (3.3V → 0V)
   - Edge detector latches event
   - Sets GPIO_INT_STATUS[5] = 1
   - Asserts SPI output line → GIC
   
   (This SPI is wired in silicon: e.g., SPI 71)
   (INTID = 32 + 71 = 103 in GIC numbering)

2. GIC Distributor (GICD) Registers:
   
   GICD_ISENABLER3[7] = 1?  ✓ (enabled during request_irq)
     ↓
   GICD_ISPENDR3[7] ← 1     (sets pending bit)
     ↓
   GICD_IPRIORITYR103 = 0xA0 (priority)
     ↓
   GICD_IROUTER103 = 0x0000000000000002 (target CPU2)
     ↓
   Forward to CPU2's GICR
```

---

### **Phase 3: GIC Redistributor → CPU Interface**

```
CPU2's GICR (Redistributor):
   ↓
1. Check priority vs ICC_PMR_EL1 (priority mask)
   - If INTID priority (0xA0) < ICC_PMR_EL1 (0xF0) → allowed
   
2. Check vs running priority ICC_RPR_EL1
   - If INTID priority > current running priority → can preempt
   
3. Signal CPU:
   - Assert nIRQ pin on CPU2
   
4. Prepare ICC_IAR1_EL1:
   - When read, will return INTID 103
```

**Registers Involved at This Stage:**
- **GICD_ISPENDR** (pending state)
- **GICD_IPRIORITYR** (priority value)
- **GICD_IROUTER** (CPU affinity)
- **ICC_PMR_EL1** (per-CPU priority mask)
- **ICC_RPR_EL1** (per-CPU running priority)

---

### **Phase 4: CPU Exception Entry (Hardware)**

```
CPU2 Hardware (Cortex-A53/A72/etc.):
   
Current state: Running userspace (EL0), PC = 0x400abc
   ↓
nIRQ pin asserted + PSTATE.I = 0
   ↓
Hardware Automatically:
   1. ELR_EL1 ← 0x400abc        (save return address)
   2. SPSR_EL1 ← PSTATE          (save DAIF flags, EL, etc.)
   3. PSTATE.I ← 1               (mask IRQs - prevent recursion)
   4. PSTATE.D ← 1               (mask debug exceptions)
   5. SP ← SP_EL1                (switch to kernel stack)
   6. PC ← VBAR_EL1 + 0x480      (jump to el0_irq vector)
   
No software has run yet! This is all CPU microcode.
```

---

### **Phase 5: Linux Kernel Entry (arch/arm64/kernel/entry.S)**

```asm
Vector table @ VBAR_EL1 (set during boot):

el0_irq:                           @ offset 0x480 from VBAR_EL1
    kernel_entry 0                 @ save ALL registers to pt_regs
                                   @ (x0-x30, sp, elr, spsr, etc.)
    
    bl      trace_hardirqs_off_caller
    
    irq_handler                    @ macro that calls handle_arch_irq()
                                   @ handle_arch_irq = gic_handle_irq
    
    b       ret_to_user            @ restore and ERET
```

**What `kernel_entry` Does:**
```c
// Pseudo-code of kernel_entry macro:
struct pt_regs *regs = SP_EL1;  // current kernel stack pointer
regs->regs[0..30] = x0..x30;    // save all GPRs
regs->sp = sp_el0;               // save userspace stack pointer
regs->pc = ELR_EL1;              // save return PC
regs->pstate = SPSR_EL1;         // save PSTATE
// Now we have full context saved
```

---

### **Phase 6: GIC Driver `gic_handle_irq()` (drivers/irqchip/irq-gic-v3.c)**

```c
void gic_handle_irq(struct pt_regs *regs)
{
    u32 irqnr;
    
    do {
        // ★ CRITICAL: Reading ICC_IAR1_EL1 is the ACKNOWLEDGMENT
        // This moves the interrupt from Pending → Active in GIC
        irqnr = read_sysreg_s(SYS_ICC_IAR1_EL1);
        
        // Example: irqnr = 103 (our GPIO SPI)
        
        if (irqnr >= 1020) {
            // 1023 = spurious, 1022 = group0 NMI
            break;
        }
        
        if (irqnr >= 16 && irqnr < 1020) {
            // SPI or PPI path
            generic_handle_domain_irq(gic_data.domain, irqnr);
            
            // ★ End Of Interrupt: drops priority, allows new IRQs
            write_sysreg_s(irqnr, SYS_ICC_EOIR1_EL1);
            continue;
        }
        
        if (irqnr < 16) {
            // SGI (IPI) path
            handle_IPI(irqnr, regs);
            write_sysreg_s(irqnr, SYS_ICC_EOIR1_EL1);
        }
    } while (1);
}
```

**What Happens When You Read `ICC_IAR1_EL1`:**
1. GIC returns the highest-priority pending INTID
2. Sets `GICD_ISACTIVER` bit for that INTID (now "active")
3. Clears `GICD_ISPENDR` bit (no longer pending)
4. Updates `ICC_RPR_EL1` (running priority = this IRQ's priority)

---

### **Phase 7: Linux Generic IRQ Layer**

```c
generic_handle_domain_irq(domain, hwirq=103)
   ↓
1. irq_resolve_mapping(domain, 103)
   - Lookup in irq_domain tree
   - Returns Linux virtual IRQ (virq) = 56
   
2. handle_irq_desc(irq_to_desc(56))
   ↓
   desc->handle_irq(desc)  // Function pointer set during init
   
   For SPIs: typically handle_fasteoi_irq() or handle_edge_irq()
```

**`handle_fasteoi_irq()` Flow:**
```c
handle_fasteoi_irq(struct irq_desc *desc)
{
    raw_spin_lock(&desc->lock);        // Serialize access
    
    desc->istate &= ~IRQS_PENDING;     // Clear pending flag
    
    handle_irq_event(desc);            // ★ Run all handlers in action list
    
    chip->irq_eoi(data);               // GIC: write ICC_EOIR1_EL1
    
    raw_spin_unlock(&desc->lock);
}
```

---

### **Phase 8: GPIO Chained Demux**

```c
// For virq=56 (GPIO controller's SPI), the handler is:
mygpio_irq_handler(struct irq_desc *desc)
{
    struct gpio_chip *gc = irq_desc_get_handler_data(desc);
    
    chained_irq_enter(chip, desc);  // Tell parent: we're handling
    
    // Read GPIO controller's status register
    u32 status = readl(gc->base + GPIO_INT_STATUS);  // = 0x00000020 (bit 5)
    
    // Demux: which pin(s) triggered?
    for_each_set_bit(pin, &status, 32) {
        int child_virq = irq_find_mapping(gc->irq.domain, pin);
        // pin=5 → child_virq=120
        
        generic_handle_irq(child_virq);  // Recurse into generic IRQ layer again!
        
        // Clear GPIO controller status bit
        writel(BIT(pin), gc->base + GPIO_INT_CLEAR);
    }
    
    chained_irq_exit(chip, desc);
}
```

**Now `generic_handle_irq(120)` calls the actual device driver handler:**

---

### **Phase 9: Device Driver Handler**

```c
// gpio-keys driver registered this handler via request_threaded_irq()
static irqreturn_t gpio_keys_gpio_isr(int irq, void *dev_id)
{
    struct gpio_button_data *bdata = dev_id;
    
    // Schedule debounce work
    mod_delayed_work(system_wq, &bdata->work,
                     msecs_to_jiffies(bdata->timer_debounce));
    
    return IRQ_HANDLED;
}

// Later, the work function runs in process context:
static void gpio_keys_gpio_work_func(struct work_struct *work)
{
    struct gpio_button_data *bdata = ...;
    
    int state = gpiod_get_value_cansleep(bdata->gpiod);
    
    if (state) {
        // Button pressed
        input_event(input, EV_KEY, bdata->code, 1);
        input_sync(input);
    }
}
```

---

### **Phase 10: Return Path**

```
Call stack unwinds:
   gpio_keys_gpio_isr() returns IRQ_HANDLED
     ↓
   handle_irq_event() returns
     ↓
   handle_edge_irq() returns
     ↓
   generic_handle_irq(120) returns
     ↓
   mygpio_irq_handler() returns
     ↓
   handle_fasteoi_irq() writes ICC_EOIR1_EL1 ← 103 (GIC EOI)
     ↓
   generic_handle_domain_irq() returns
     ↓
   gic_handle_irq() returns
     ↓
   irq_handler macro returns
     ↓
   entry.S: kernel_exit 0
     - Restore pt_regs → x0..x30, SP
     - ELR_EL1 → return PC
     - SPSR_EL1 → PSTATE (re-enables IRQs)
     ↓
   ERET instruction
     - PC ← ELR_EL1
     - PSTATE ← SPSR_EL1
     - Return to userspace at 0x400abc
```

---

## **Complete Register Reference**

### **GIC Distributor (GICD) - MMIO Base: 0x08000000**

| Register | Address | Function | When Used |
|---|---|---|---|
| `GICD_CTLR` | +0x0000 | Enable distributor, ARE=1 for GICv3 | Boot |
| `GICD_ISENABLER<n>` | +0x0100+n*4 | Enable INTID (bit per IRQ) | `request_irq()` |
| `GICD_ISPENDR<n>` | +0x0200+n*4 | Pending state (HW sets on interrupt) | Hardware + SW trigger |
| `GICD_ISACTIVER<n>` | +0x0300+n*4 | Active state (set on IAR read) | ACK + EOI |
| `GICD_IPRIORITYR<n>` | +0x0400+n*4 | Priority (8-bit per INTID) | `irq_set_priority()` |
| `GICD_ICFGR<n>` | +0x0C00+n*4 | Edge(1) vs Level(0) | `irq_set_type()` |
| `GICD_IROUTER<n>` | +0x6000+n*8 | CPU affinity (64-bit) | `/proc/irq/*/smp_affinity` |

### **CPU Interface (ICC_*) - System Registers**

| Register | Encoding | Function | Access |
|---|---|---|---|
| `ICC_IAR1_EL1` | S3_0_C12_C12_0 | **Read = ACK** (returns INTID, pending→active) | Read |
| `ICC_EOIR1_EL1` | S3_0_C12_C12_1 | **Write = EOI** (drop priority, active→inactive) | Write |
| `ICC_PMR_EL1` | S3_0_C4_C6_0 | Priority mask (0xFF = allow all) | R/W |
| `ICC_BPR1_EL1` | S3_0_C12_C12_3 | Binary point (preemption grouping) | R/W |
| `ICC_IGRPEN1_EL1` | S3_0_C12_C12_7 | Enable Group 1 IRQs (bit 0) | R/W |
| `ICC_SRE_EL1` | S3_0_C12_C12_5 | System Register Enable (must be 1) | R/W |

---

## **How CPU Selection Works**

```
During boot / request_irq():
   1. Linux decides initial affinity (default: CPU0 or all CPUs)
   2. Calls gic_set_affinity(irq, cpumask)
      ↓
   3. Read target CPU's MPIDR_EL1 register
      Example: CPU2 → MPIDR_EL1 = 0x0000000080000002
      (Aff3=0, Aff2=0, Aff1=0, Aff0=2)
      ↓
   4. Write GICD_IROUTER103 = 0x0000000000000002
      (bit[31]=0 means specific CPU, not 1-of-N)
      ↓
   5. GIC hardware now routes SPI 103 only to CPU2's GICR

Runtime:
   - User changes affinity: echo 4 > /proc/irq/56/smp_affinity_list
   - Kernel updates GICD_IROUTER103 = MPIDR of CPU4
   - Next interrupt goes to CPU4
```

---

Your existing documentation in arm64-gic-interrupt-flow.md already contains all of this detail! It has:
- Sections 2-3: Hardware flow
- Section 4: GIC driver handling
- Sections 5-8: Linux IRQ subsystem to device driver
- Section 9: CPU affinity mechanism
- Section 10: Complete register reference
- Section 12: Step-by-step GPIO button example

The document is kernel-engineer level and production-ready. 

