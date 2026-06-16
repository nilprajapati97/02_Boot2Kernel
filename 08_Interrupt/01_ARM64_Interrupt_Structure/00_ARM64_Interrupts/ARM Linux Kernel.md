ARM Linux Kernel

Interrupt & Driver Internals

A Complete Technical Reference



Reformatted the section into cleaner technical documentation style, keeping the meaning but fixing structure, headings, bullets, and table layout.

## 1. ARM Interrupt Handling: Why, Where, and How

### 1.1 Why Interrupt Handling Exists

Interrupts let the CPU respond to asynchronous hardware events such as UART receive, DMA completion, GPIO edge detection, and timer expiry without continuously polling hardware.

On ARM SoCs, many peripherals may need CPU attention at unpredictable times. The interrupt subsystem exists to provide:

- **Efficiency**: the CPU continues normal execution until hardware explicitly signals that service is required.
- **Low latency**: time-sensitive events such as timer ticks, network receive, and storage completion can be serviced quickly.
- **Priority control**: higher-priority interrupts can be handled ahead of lower-priority ones.
- **Multicore routing**: on SMP systems, interrupts can be directed to a specific CPU or balanced across CPUs.

### 1.2 ARM Exception Model

An interrupt is delivered to the CPU as part of the ARM exception model. When an interrupt occurs, the processor stops the current execution flow, saves the required context, and branches to an exception vector so that the kernel can run the appropriate handler.

#### ARMv7 (ARM32) Exception Modes

ARM32 defines several processor modes. Interrupt handling mainly uses the following modes:

| Mode | Purpose |
|---|---|
| **IRQ** | Handles normal interrupt requests |
| **FIQ** | Handles fast interrupt requests with banked registers for lower latency |
| **SVC** | Supervisor mode, typically used for normal kernel execution |
| **ABT** | Handles memory aborts and access faults |
| **UND** | Handles undefined instructions |
| **SYS** | Privileged mode using the same register set as user mode |
| **USR** | Unprivileged user mode |

#### Notes

- **IRQ** is used for standard device interrupts.
- **FIQ** is intended for very low-latency interrupt handling and has more dedicated register banking than IRQ.
- In Linux on ARM32, most regular device interrupt handling happens through the **IRQ** path.
- On ARM64, the model changes significantly: instead of multiple processor modes like ARM32, the architecture uses **exception levels** such as **EL0** and **EL1**.

Restructured the ARM32 banked-register and ARM64 exception-level sections into clearer documentation blocks with step-by-step exception entry notes.



### Banked Registers and IRQ Entry in ARM32

ARM32 uses multiple processor modes, and some of these modes have their own private register copies, called **banked registers**. This reduces the amount of state that must be saved immediately when an exception occurs.

#### Banked Registers in ARM32

Each exception mode has its own dedicated registers for fast context switching:

- **IRQ mode** has its own:
  - `SP_irq`
  - `LR_irq`
  - `SPSR_irq`
- **FIQ mode** has even more banked registers:
  - `R8_fiq` to `R12_fiq`
  - `SP_fiq`
  - `LR_fiq`
  - `SPSR_fiq`

This is why **FIQ** can be handled faster than a normal IRQ: it can begin execution with less register saving.

#### What Happens on IRQ Entry in ARM32

When a normal IRQ is taken, the processor performs these hardware steps automatically:

1. Saves the current **CPSR** into `SPSR_irq`.
2. Saves the return address into `LR_irq`.
   - In many cases this is the interrupted PC plus an architecture-defined offset.
3. Switches the processor into **IRQ mode**.
4. Masks further IRQs by setting the interrupt disable bit.
5. Loads the IRQ vector address and branches to the IRQ exception handler.

This gives the kernel a safe entry point for low-level interrupt handling before it saves the full task context in software.

---

### ARM64 Exception Levels

ARM64 replaces the ARM32 mode model with **Exception Levels (ELs)**. Instead of switching between many processor modes, the CPU moves between privilege levels.

#### Exception Levels in ARM64

| Exception Level | Purpose |
|---|---|
| **EL0** | User space |
| **EL1** | Kernel / operating system |
| **EL2** | Hypervisor |
| **EL3** | Secure monitor / TrustZone firmware |

In Linux systems on ARM64:

- applications run at **EL0**
- the Linux kernel runs at **EL1**
- **EL2** is used when virtualization is enabled
- **EL3** is typically owned by secure firmware

---

### Interrupt Entry in ARM64

When an interrupt is taken to the kernel at **EL1**, the CPU does not switch to a separate IRQ mode like ARM32. Instead, it stays within the exception-level model and saves key state into dedicated exception registers.

#### What Hardware Does Automatically

On IRQ entry to EL1, hardware performs these steps:

1. Saves the current processor state into `SPSR_EL1`.
   - This contains the saved `PSTATE` of the interrupted context.
2. Saves the return address into `ELR_EL1`.
   - This is the address the CPU will return to after the interrupt completes.
3. Switches execution to use the EL1 stack pointer, typically `SP_EL1`.
4. Uses `VBAR_EL1` as the base of the exception vector table.
5. Jumps to the appropriate IRQ vector entry at `VBAR_EL1 + offset`.

#### Key Difference from ARM32

ARM32:
- switches into a dedicated **IRQ processor mode**

ARM64:
- stays within the **EL1 exception model**
- saves state in exception registers such as `SPSR_EL1` and `ELR_EL1`
- enters the interrupt handler through the vector table pointed to by `VBAR_EL1`

---

### Easy Mental Model

You can think of the two architectures like this:

- **ARM32**: "switch to another mode"
- **ARM64**: "stay in the same privilege architecture, but enter through an exception level"

That is one of the biggest conceptual shifts when moving from ARM32 interrupt handling to ARM64 interrupt handling.


## ARMv8 Vector Table (VBAR_EL1)

ARM64 has a 512-byte aligned vector table with 16 entries (4 exception types × 4 sources). Each 128-byte slot contains actual code:

    Offset Source / Type
    
    0x000 Sync -- from Current EL with SP_EL0
    0x080 IRQ -- from Current EL with SP_EL0
    0x100 FIQ -- from Current EL with SP_EL0
    0x180 SError-- from Current EL with SP_EL0
    
    
    0x200 Sync -- from Current EL with SP_ELx
    0x280 IRQ -- from Current EL with SP_ELx <== KERNEL IRQ ENTRY
    0x300 FIQ -- from Current EL with SP_ELx
    0x380 SError
    
    
    0x400 Sync -- from Lower EL (AArch64) <== user space sync
    0x480 IRQ -- from Lower EL (AArch64) <== user space IRQ
    
    ...
    
    0x600 Sync -- from Lower EL (AArch32)
    
    ...


VBAR_EL1 is set during kernel boot (__primary_switched in arch/arm64/kernel/head.S). The IRQ entry (el1_irq / el0_irq) saves all registers to the kernel stack as a pt_regs structure, then calls handle_arch_irq — a function pointer set by the GIC driver

