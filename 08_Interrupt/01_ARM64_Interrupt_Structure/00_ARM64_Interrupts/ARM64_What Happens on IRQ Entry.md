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

