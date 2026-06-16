# ARM64 Linux Interrupt Subsystem - Design Document

## Table of Contents
1. [Overview](#overview)
2. [ARM64 Exception Model](#arm64-exception-model)
3. [Generic Interrupt Controller (GIC)](#generic-interrupt-controller-gic)
4. [Linux Interrupt Framework](#linux-interrupt-framework)
5. [Interrupt Flow](#interrupt-flow)
6. [Key Data Structures](#key-data-structures)
7. [Critical Code Paths](#critical-code-paths)
8. [Performance Considerations](#performance-considerations)
9. [Interview Key Points](#interview-key-points)

---

## 1. Overview

### Purpose
This document explains how Linux handles interrupts on ARM64 architecture, covering hardware mechanisms, kernel abstractions, and the complete interrupt handling flow.

### Key Components
- **Hardware**: GIC (Generic Interrupt Controller), CPU exception levels
- **Kernel**: IRQ subsystem, irq_domain, irqchip framework
- **Entry**: Exception vectors, context switching

---

## 2. ARM64 Exception Model

### 2.1 Exception Levels (EL)
```
EL3 - Secure Monitor (ARM Trusted Firmware)
EL2 - Hypervisor (KVM)
EL1 - Kernel (Linux runs here)
EL0 - User space
```

### 2.2 Exception Types
ARM64 defines 4 exception types:
1. **Synchronous** - Caused by instruction execution (syscalls, page faults)
2. **IRQ** - Normal interrupts (maskable)
3. **FIQ** - Fast interrupts (higher priority, typically secure world)
4. **SError** - System errors (async aborts)

### 2.3 Exception Vector Table
Located at address in **VBAR_ELn** register:

```
Offset   Exception Type              Source
------   --------------              ------
0x000    Synchronous                 Current EL with SP_EL0
0x080    IRQ                         Current EL with SP_EL0
0x100    FIQ                         Current EL with SP_EL0
0x180    SError                      Current EL with SP_EL0
0x200    Synchronous                 Current EL with SP_ELx
0x280    IRQ                         Current EL with SP_ELx
0x300    FIQ                         Current EL with SP_ELx
0x380    SError                      Current EL with SP_ELx
0x400    Synchronous                 Lower EL (AArch64)
0x480    IRQ                         Lower EL (AArch64)
0x500    FIQ                         Lower EL (AArch64)
0x580    SError                      Lower EL (AArch64)
0x600    Synchronous                 Lower EL (AArch32)
0x680    IRQ                         Lower EL (AArch32)
0x700    FIQ                         Lower EL (AArch32)
0x780    SError                      Lower EL (AArch32)
```

**Linux Implementation**: `arch/arm64/kernel/entry.S`
```assembly
.align 11
SYM_CODE_START(vectors)
    kernel_ventry 1, t, 64, sync    // Synchronous EL1t
    kernel_ventry 1, t, 64, irq     // IRQ EL1t
    kernel_ventry 1, t, 64, fiq     // FIQ EL1t
    kernel_ventry 1, t, 64, error   // Error EL1t
    
    kernel_ventry 1, h, 64, sync    // Synchronous EL1h
    kernel_ventry 1, h, 64, irq     // IRQ EL1h (most common)
    kernel_ventry 1, h, 64, fiq     // FIQ EL1h
    kernel_ventry 1, h, 64, error   // Error EL1h
    ...
SYM_CODE_END(vectors)
```

### 2.4 DAIF Register (Interrupt Masking)
Controls exception masking at CPU level:
- **D** - Debug exceptions
- **A** - SError (async abort)
- **I** - IRQ
- **F** - FIQ

```c
// Disable IRQs
local_irq_disable();  // Sets DAIF.I = 1

// Enable IRQs
local_irq_enable();   // Clears DAIF.I = 0
```

---

## 3. Generic Interrupt Controller (GIC)

### 3.1 GIC Versions
- **GICv2** - Legacy, up to 8 cores
- **GICv3/v4** - Modern, scalable, used in current ARM64 systems

### 3.2 GIC Components

#### Distributor (GICD)
- Global interrupt routing
- Interrupt priority configuration
- Enable/disable interrupts
- Manages SPIs (Shared Peripheral Interrupts)

#### CPU Interface (GICC - GICv2) / Redistributor (GICR - GICv3)
- Per-CPU interrupt delivery
- Interrupt acknowledgment
- End of Interrupt (EOI)
- Manages SGIs and PPIs

#### ITS (Interrupt Translation Service - GICv3+)
- MSI/MSI-X support for PCIe devices
- LPI (Locality-specific Peripheral Interrupts)

### 3.3 Interrupt Types

| Type | ID Range | Description | Scope |
|------|----------|-------------|-------|
| SGI  | 0-15     | Software Generated Interrupts | Per-CPU (IPI) |
| PPI  | 16-31    | Private Peripheral Interrupts | Per-CPU (timers) |
| SPI  | 32-1019  | Shared Peripheral Interrupts | Global (devices) |
| LPI  | 8192+    | Locality-specific (MSI) | GICv3+ only |

### 3.4 Interrupt States
```
Inactive → Pending → Active → Inactive
                  ↓         ↑
                  └─Active+Pending
```

- **Inactive**: No interrupt
- **Pending**: Interrupt asserted, waiting for CPU
- **Active**: CPU handling interrupt
- **Active+Pending**: Handling interrupt, another pending

### 3.5 GIC Programming Sequence

**Initialization**:
```c
// 1. Disable distributor
writel(0, GICD_CTLR);

// 2. Configure interrupt type (level/edge)
writel(config, GICD_ICFGRn);

// 3. Set priority
writeb(priority, GICD_IPRIORITYRn);

// 4. Set target CPU (GICv2) or routing (GICv3)
writel(cpu_mask, GICD_ITARGETSRn);

// 5. Enable interrupt
writel(1 << (irq % 32), GICD_ISENABLERn);

// 6. Enable distributor
writel(GICD_CTLR_ENABLE, GICD_CTLR);

// 7. Set CPU interface priority mask
writel(0xff, GICC_PMR);

// 8. Enable CPU interface
writel(GICC_CTLR_ENABLE, GICC_CTLR);
```

**Interrupt Handling**:
```c
// 1. Read IAR (Interrupt Acknowledge Register)
irq = readl(GICC_IAR);

// 2. Handle interrupt
handle_IRQ(irq);

// 3. Write EOI (End Of Interrupt)
writel(irq, GICC_EOIR);
```

---

## 4. Linux Interrupt Framework

### 4.1 IRQ Domain
Maps hardware IRQ numbers to Linux virtual IRQ numbers.

```
Hardware IRQ (hwirq) → IRQ Domain → Linux IRQ (virq)
```

**Why needed?**
- Multiple interrupt controllers
- Hierarchical interrupt routing
- Dynamic IRQ allocation

```c
struct irq_domain {
    const char *name;
    const struct irq_domain_ops *ops;
    void *host_data;
    unsigned int hwirq_max;
    struct irq_domain *parent;  // Hierarchical domains
    ...
};

// Create domain
domain = irq_domain_add_linear(node, size, &gic_irq_domain_ops, NULL);

// Map hardware IRQ to virtual IRQ
virq = irq_create_mapping(domain, hwirq);
```

### 4.2 IRQ Chip
Abstracts interrupt controller operations.

```c
struct irq_chip {
    const char *name;
    void (*irq_mask)(struct irq_data *data);
    void (*irq_unmask)(struct irq_data *data);
    void (*irq_eoi)(struct irq_data *data);
    int (*irq_set_affinity)(struct irq_data *data, const struct cpumask *dest, bool force);
    int (*irq_set_type)(struct irq_data *data, unsigned int flow_type);
    ...
};
```

**GIC Implementation**:
```c
static struct irq_chip gic_chip = {
    .name = "GICv3",
    .irq_mask = gic_mask_irq,
    .irq_unmask = gic_unmask_irq,
    .irq_eoi = gic_eoi_irq,
    .irq_set_type = gic_set_type,
    .irq_set_affinity = gic_set_affinity,
    ...
};
```

### 4.3 IRQ Descriptor
Per-IRQ metadata and state.

```c
struct irq_desc {
    struct irq_data irq_data;
    struct irqaction *action;        // Handler chain
    unsigned int status_use_accessors;
    unsigned int depth;              // Disable depth
    unsigned int irq_count;          // Statistics
    unsigned int irqs_unhandled;
    raw_spinlock_t lock;
    struct cpumask *affinity_hint;
    ...
};
```

### 4.4 IRQ Action (Handler)
```c
struct irqaction {
    irq_handler_t handler;           // Primary handler
    void *dev_id;                    // Device identifier
    struct irqaction *next;          // Shared IRQ chain
    irq_handler_t thread_fn;         // Threaded handler
    struct task_struct *thread;
    unsigned long flags;             // IRQF_* flags
    const char *name;
    ...
};
```

### 4.5 Request IRQ
```c
int request_irq(unsigned int irq,
                irq_handler_t handler,
                unsigned long flags,
                const char *name,
                void *dev_id);

// Example
request_irq(irq, my_handler, IRQF_SHARED, "mydevice", dev);

// Handler signature
irqreturn_t my_handler(int irq, void *dev_id) {
    // Handle interrupt
    return IRQ_HANDLED;
}
```

**Flags**:
- `IRQF_SHARED` - Allow IRQ sharing
- `IRQF_TRIGGER_*` - Edge/level triggering
- `IRQF_ONESHOT` - Keep IRQ masked until thread handler completes
- `IRQF_NO_SUSPEND` - Don't disable during suspend

---

## 5. Interrupt Flow

### 5.1 Complete Flow Diagram
```
[Device] → [GIC] → [CPU] → [Exception Vector] → [Kernel Handler] → [Driver Handler]
   ↓         ↓       ↓            ↓                    ↓                   ↓
 Assert   Route   Take      Save Context         Dispatch           Process
  IRQ     to CPU  Exception  & Ack IRQ           to Handler         & EOI
```

### 5.2 Detailed Steps

#### Step 1: Hardware Interrupt Assertion
```
Device asserts interrupt line → GIC Distributor marks interrupt pending
```

#### Step 2: GIC Routing
```c
// GIC selects highest priority pending interrupt
// Routes to target CPU based on affinity configuration
// CPU receives IRQ signal
```

#### Step 3: CPU Exception Entry
```assembly
// CPU jumps to vector table (VBAR_EL1 + 0x280 for EL1h IRQ)
// Hardware automatically:
// - Saves PSTATE to SPSR_EL1
// - Saves PC to ELR_EL1
// - Disables interrupts (DAIF.I = 1)
// - Switches to EL1 if from EL0
```

#### Step 4: Kernel Entry (`entry.S`)
```assembly
kernel_entry 1  // Macro that:
    // 1. Save all general purpose registers (x0-x30)
    sub sp, sp, #PT_REGS_SIZE
    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    ...
    
    // 2. Save ELR_EL1 and SPSR_EL1
    mrs x21, elr_el1
    mrs x22, spsr_el1
    stp x21, x22, [sp, #S_PC]
    
    // 3. Save original SP
    add x21, sp, #PT_REGS_SIZE
    str x21, [sp, #S_SP]
```

#### Step 5: IRQ Handler Entry
```c
// arch/arm64/kernel/entry-common.c
asmlinkage void __exception_irq_entry el1_irq(struct pt_regs *regs) {
    enter_from_kernel_mode(regs);
    
    // Call generic IRQ handler
    handle_arch_irq(regs);  // Points to gic_handle_irq
    
    exit_to_kernel_mode(regs);
}
```

#### Step 6: GIC IRQ Acknowledgment
```c
// drivers/irqchip/irq-gic-v3.c
static void __exception_irq_entry gic_handle_irq(struct pt_regs *regs) {
    u32 irqnr;
    
    do {
        // Read IAR - acknowledges interrupt and returns IRQ number
        irqnr = gic_read_iar();
        
        if (likely(irqnr > 15 && irqnr < 1020)) {
            // Convert hardware IRQ to Linux virtual IRQ
            int virq = irq_find_mapping(gic_data.domain, irqnr);
            
            // Dispatch to generic handler
            generic_handle_irq(virq);
        } else if (irqnr < 16) {
            // SGI (IPI)
            gic_handle_sgi(irqnr);
        }
        
    } while (irqnr != ICC_IAR1_EL1_SPURIOUS);
}
```

#### Step 7: Generic IRQ Handling
```c
// kernel/irq/irqdesc.c
int generic_handle_irq(unsigned int irq) {
    struct irq_desc *desc = irq_to_desc(irq);
    
    generic_handle_irq_desc(desc);
}

static inline void generic_handle_irq_desc(struct irq_desc *desc) {
    desc->handle_irq(desc);  // Flow handler
}
```

#### Step 8: Flow Handler
```c
// kernel/irq/chip.c
void handle_fasteoi_irq(struct irq_desc *desc) {
    struct irq_chip *chip = desc->irq_data.chip;
    
    raw_spin_lock(&desc->lock);
    
    // Mask if needed
    if (unlikely(irqd_irq_inprogress(&desc->irq_data))) {
        chip->irq_mask(&desc->irq_data);
    }
    
    // Mark as in progress
    desc->istate |= IRQS_INPROGRESS;
    
    raw_spin_unlock(&desc->lock);
    
    // Call handler chain
    handle_irq_event(desc);
    
    // EOI to GIC
    chip->irq_eoi(&desc->irq_data);
}
```

#### Step 9: Driver Handler Execution
```c
// kernel/irq/handle.c
irqreturn_t handle_irq_event(struct irq_desc *desc) {
    struct irqaction *action;
    irqreturn_t ret = IRQ_NONE;
    
    for_each_action_of_desc(desc, action) {
        irqreturn_t res;
        
        // Call driver's handler
        res = action->handler(irq, action->dev_id);
        
        // Wake threaded handler if needed
        if (res == IRQ_WAKE_THREAD && action->thread_fn) {
            wake_up_process(action->thread);
        }
        
        ret |= res;
    }
    
    return ret;
}
```

#### Step 10: Exception Return
```assembly
kernel_exit 1  // Macro that:
    // 1. Restore SPSR_EL1 and ELR_EL1
    ldp x21, x22, [sp, #S_PC]
    msr elr_el1, x21
    msr spsr_el1, x22
    
    // 2. Restore general purpose registers
    ldp x0, x1, [sp, #16 * 0]
    ldp x2, x3, [sp, #16 * 1]
    ...
    
    // 3. Restore SP
    add sp, sp, #PT_REGS_SIZE
    
    // 4. Return from exception
    eret  // Returns to ELR_EL1, restores PSTATE from SPSR_EL1
          // Re-enables interrupts
```

---

## 6. Key Data Structures

### 6.1 pt_regs (Register Context)
```c
// arch/arm64/include/asm/ptrace.h
struct pt_regs {
    u64 regs[31];      // x0-x30
    u64 sp;            // Stack pointer
    u64 pc;            // Program counter (ELR_EL1)
    u64 pstate;        // Processor state (SPSR_EL1)
    u64 orig_x0;       // Original x0 (for syscall restart)
    ...
};
```

### 6.2 irq_data (IRQ Information)
```c
struct irq_data {
    u32 mask;                    // Precomputed bitmask
    unsigned int irq;            // Linux virtual IRQ
    unsigned long hwirq;         // Hardware IRQ
    struct irq_common_data *common;
    struct irq_chip *chip;       // IRQ controller ops
    struct irq_domain *domain;   // IRQ domain
    struct irq_data *parent_data; // Hierarchical parent
    void *chip_data;             // Controller-specific data
};
```

### 6.3 GIC Data Structures
```c
struct gic_chip_data {
    struct irq_domain *domain;
    void __iomem *dist_base;     // Distributor base
    void __iomem *cpu_base;      // CPU interface base (GICv2)
    u32 nr_irqs;                 // Number of IRQs
    ...
};
```

---

## 7. Critical Code Paths

### 7.1 Fast Path (Hardirq Context)
```
Exception Entry (100-200 cycles)
    ↓
GIC Acknowledge (10-20 cycles)
    ↓
Handler Dispatch (50-100 cycles)
    ↓
Driver Handler (device-specific)
    ↓
GIC EOI (10-20 cycles)
    ↓
Exception Exit (100-200 cycles)
```

**Total overhead**: ~300-600 cycles (excluding driver handler)

### 7.2 Threaded Interrupts
```c
// Top half (hardirq)
irqreturn_t top_half(int irq, void *dev_id) {
    // Minimal work: read status, clear interrupt
    return IRQ_WAKE_THREAD;
}

// Bottom half (kernel thread)
irqreturn_t thread_fn(int irq, void *dev_id) {
    // Heavy processing
    return IRQ_HANDLED;
}

request_threaded_irq(irq, top_half, thread_fn, flags, name, dev);
```

### 7.3 Softirqs and Tasklets
```c
// Raise softirq from hardirq
raise_softirq(NET_RX_SOFTIRQ);

// Softirq runs after hardirq with interrupts enabled
// ksoftirqd handles deferred work
```

---

## 8. Performance Considerations

### 8.1 Interrupt Affinity
```bash
# Set IRQ affinity to CPU 2
echo 4 > /proc/irq/45/smp_affinity

# In code
irq_set_affinity_hint(irq, cpumask_of(2));
```

### 8.2 IRQ Coalescing
Reduce interrupt rate by batching:
```c
// NAPI for network devices
napi_schedule(&priv->napi);
```

### 8.3 Interrupt Mitigation
- **Interrupt moderation**: Hardware delays interrupts
- **Polling mode**: Switch to polling under high load
- **Threaded handlers**: Move work out of hardirq context

### 8.4 Cache Effects
- Keep handler code/data hot in cache
- Minimize cache line bouncing in shared IRQ scenarios
- Use per-CPU data structures

---

## 9. Interview Key Points

### 9.1 Critical Questions & Answers

**Q: What happens when an interrupt occurs on ARM64?**
```
1. Device asserts interrupt to GIC
2. GIC routes to target CPU
3. CPU takes exception, jumps to vector table (VBAR_EL1 + offset)
4. Hardware saves PC→ELR_EL1, PSTATE→SPSR_EL1, disables interrupts
5. Kernel saves registers (pt_regs)
6. GIC IAR read acknowledges interrupt
7. Generic handler dispatches to driver handler
8. Driver processes interrupt
9. GIC EOI signals completion
10. Kernel restores registers and executes ERET
```

**Q: Difference between GICv2 and GICv3?**
```
GICv2:
- CPU interface memory-mapped (GICC)
- Limited to 8 CPUs
- No MSI support

GICv3:
- CPU interface via system registers (ICC_*)
- Scalable to 100+ CPUs
- Redistributor per CPU (GICR)
- ITS for MSI/MSI-X
- LPI support
```

**Q: What is irq_domain and why is it needed?**
```
Maps hardware IRQ numbers to Linux virtual IRQs.
Needed because:
- Multiple interrupt controllers
- Dynamic IRQ allocation
- Hierarchical interrupt routing (GIC → GPIO controller)
- Device tree/ACPI abstraction
```

**Q: Explain interrupt context vs process context**
```
Interrupt Context (Hardirq):
- Cannot sleep
- Cannot call schedule()
- Minimal stack
- Interrupts disabled
- Fast execution required

Process Context:
- Can sleep
- Can be preempted
- Full stack
- Interrupts enabled
- Can take locks
```

**Q: How does Linux handle shared IRQs?**
```c
// Multiple handlers on same IRQ line
request_irq(irq, handler1, IRQF_SHARED, "dev1", dev1);
request_irq(irq, handler2, IRQF_SHARED, "dev2", dev2);

// Kernel calls all handlers, each checks if their device triggered
irqreturn_t handler(int irq, void *dev_id) {
    if (!my_device_triggered())
        return IRQ_NONE;  // Not my interrupt
    
    handle_my_device();
    return IRQ_HANDLED;
}
```

**Q: What are SGI, PPI, and SPI?**
```
SGI (0-15): Software Generated Interrupts
- IPIs (inter-processor interrupts)
- Used for CPU synchronization, TLB shootdown

PPI (16-31): Private Peripheral Interrupts
- Per-CPU interrupts
- Arch timer, PMU, local watchdog

SPI (32-1019): Shared Peripheral Interrupts
- Global device interrupts
- UART, Ethernet, PCIe devices
```

**Q: How do you debug interrupt issues?**
```bash
# Check interrupt counts
cat /proc/interrupts

# Check IRQ affinity
cat /proc/irq/*/smp_affinity_list

# Trace interrupts
trace-cmd record -e irq
trace-cmd report

# Check for spurious interrupts
dmesg | grep -i "nobody cared"

# GIC registers
cat /sys/kernel/debug/irqchip/gic-*
```

### 9.2 Advanced Topics

**Nested Interrupts**:
ARM64 disables interrupts on exception entry. Linux keeps them disabled during hardirq to prevent nesting and stack overflow.

**Priority Inversion**:
GIC priority masking (PMR) prevents lower priority interrupts from preempting higher priority handlers.

**Interrupt Latency**:
- Hardware latency: GIC routing + CPU exception entry (~100-200ns)
- Software latency: Kernel dispatch + driver handler (varies)
- Reduce by: IRQ affinity, threaded handlers, PREEMPT_RT

**MSI/MSI-X**:
PCIe devices write to memory address, GIC ITS translates to LPI.

**CPU Hotplug**:
Migrate IRQs away from offline CPU:
```c
irq_set_affinity(irq, cpu_online_mask);
```

### 9.3 Common Pitfalls

1. **Forgetting to EOI**: Interrupt stays active, no more interrupts
2. **Sleeping in hardirq**: Causes kernel panic
3. **Long handlers**: Increases latency, use threaded handlers
4. **Wrong trigger type**: Edge vs level misconfiguration
5. **Missing IRQF_SHARED**: Shared IRQ without flag fails
6. **Interrupt storm**: Device continuously asserts interrupt

---

## 10. Code Examples

### 10.1 Simple Driver with IRQ
```c
#include <linux/interrupt.h>
#include <linux/module.h>

static irqreturn_t my_irq_handler(int irq, void *dev_id) {
    struct my_device *dev = dev_id;
    u32 status;
    
    // Read interrupt status
    status = readl(dev->base + STATUS_REG);
    if (!(status & IRQ_PENDING))
        return IRQ_NONE;
    
    // Clear interrupt
    writel(status, dev->base + STATUS_REG);
    
    // Process interrupt
    handle_data(dev);
    
    return IRQ_HANDLED;
}

static int my_probe(struct platform_device *pdev) {
    struct my_device *dev;
    int irq, ret;
    
    dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
    
    irq = platform_get_irq(pdev, 0);
    
    ret = devm_request_irq(&pdev->dev, irq, my_irq_handler,
                           0, "mydevice", dev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to request IRQ\n");
        return ret;
    }
    
    return 0;
}
```

### 10.2 Threaded IRQ Handler
```c
static irqreturn_t my_hardirq(int irq, void *dev_id) {
    struct my_device *dev = dev_id;
    
    // Quick check and clear
    if (!device_has_interrupt(dev))
        return IRQ_NONE;
    
    device_clear_interrupt(dev);
    
    // Wake thread
    return IRQ_WAKE_THREAD;
}

static irqreturn_t my_thread_fn(int irq, void *dev_id) {
    struct my_device *dev = dev_id;
    
    // Heavy processing (can sleep)
    process_data(dev);
    
    return IRQ_HANDLED;
}

devm_request_threaded_irq(&pdev->dev, irq,
                          my_hardirq, my_thread_fn,
                          IRQF_ONESHOT, "mydevice", dev);
```

### 10.3 IPI (Inter-Processor Interrupt)
```c
// Send IPI to CPU 2
smp_call_function_single(2, my_ipi_handler, data, 1);

// IPI handler runs on target CPU
static void my_ipi_handler(void *info) {
    // Execute on remote CPU
    flush_tlb_local();
}
```

---

## 11. References

### Key Files in Linux Kernel
```
arch/arm64/kernel/entry.S           - Exception vectors
arch/arm64/kernel/entry-common.c    - Exception handlers
drivers/irqchip/irq-gic-v3.c        - GICv3 driver
kernel/irq/irqdesc.c                - IRQ descriptor management
kernel/irq/chip.c                   - IRQ chip operations
kernel/irq/handle.c                 - Generic IRQ handling
include/linux/interrupt.h           - IRQ API
```

### Documentation
- ARM Architecture Reference Manual (ARM ARM)
- GIC Architecture Specification
- Linux kernel Documentation/core-api/genericirq.rst
- Linux kernel Documentation/devicetree/bindings/interrupt-controller/

---

## Summary for Interview

**Key Points to Remember**:
1. ARM64 has 4 exception types, IRQ is maskable via DAIF
2. GIC routes interrupts, has 3 types: SGI/PPI/SPI
3. Exception vector at VBAR_EL1, 16 entries
4. Hardware saves PC→ELR, PSTATE→SPSR, disables IRQs
5. Kernel saves registers, acknowledges via IAR, dispatches handler
6. irq_domain maps hwirq→virq, irq_chip abstracts controller
7. Fast path: entry→ack→dispatch→handler→EOI→exit
8. Use threaded handlers for heavy work
9. Shared IRQs need IRQF_SHARED, handlers return IRQ_NONE if not theirs
10. Debug with /proc/interrupts, trace-cmd, and GIC debugfs

**Be Ready to Discuss**:
- GICv3 improvements over GICv2
- Interrupt latency optimization
- Real-time considerations (PREEMPT_RT)
- MSI/MSI-X and LPI
- CPU affinity and load balancing
- Interrupt storms and mitigation
- Nested interrupts and priority
- Softirqs vs tasklets vs workqueues

Good luck with your Nvidia interview!
