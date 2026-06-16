Linux Interrupt flow:

1️⃣ Hardware raises interrupt
A hardware device (UART / GPIO / DMA / NIC / display controller) asserts an interrupt line.
Example:
UART receives byte
↓
IRQ line asserted

2️⃣ Interrupt Controller receives it
The interrupt first goes to interrupt controller.
Example:
ARM → GIC (Generic Interrupt Controller)
x86 → APIC
Controller decides: interrupt ID, target CPU, priority

3️⃣ CPU stops current execution
CPU receives interrupt signal.
Current execution paused.
CPU automatically: saves PC, saves minimal state, switches to interrupt mode

4️⃣ CPU jumps to interrupt vector
CPU goes to fixed vector table entry.
Example:
vector → IRQ handler entry
Architecture-specific assembly executes first.

5️⃣ Low-level architecture handler runs
This code is architecture-dependent.
Responsibilities: save registers, identify IRQ number, call generic kernel handler 

6️⃣ Generic kernel IRQ subsystem takes over
Linux common handler:
handle_irq()
Then: generic_handle_irq()
This finds registered interrupt descriptor.

7️⃣ IRQ descriptor lookup
Kernel checks:
irq_desc
Contains: IRQ number, registered ISR, flags, affinity

8️⃣ Device ISR (top half) runs
Registered ISR executes:
irqreturn_t my_isr(int irq, void *dev)
ISR should do only critical work:
✅ acknowledge hardware
 ✅ clear interrupt source
 ✅ minimal read/write

Example ISR
irqreturn_t uart_isr(int irq, void *dev)
{
 status = read_reg();
 clear_irq();
 queue_work(...);
 return IRQ_HANDLED;
}

👉 Why ISR must be short
Because while ISR runs:
interrupts may stay masked
scheduler paused
latency increases

9️⃣ Bottom half scheduled if needed
Heavy work deferred.
Linux mechanisms:
SoftIRQ
Fast deferred processing.
Used in: networking, 
Tasklet: Built on softirq. Older mechanism.
Workqueue: Runs in process context, Can sleep, Most driver work uses this.
Example- ISR:queue_work(my_wq, &work);
Later:
worker thread executes heavy logic

🔟 Interrupt return
After ISR:
Kernel restores CPU context.
Returns to interrupted code.
Full Linux interrupt flow summary
Device raises IRQ
↓
Interrupt Controller (GIC/APIC)
↓
CPU interrupt vector
↓
Arch entry handler
↓
generic_handle_irq()
↓
ISR (top half)
↓
SoftIRQ / tasklet / workqueue
↓
Return to previous task

