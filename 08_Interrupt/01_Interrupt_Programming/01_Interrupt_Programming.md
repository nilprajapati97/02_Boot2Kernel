Interrupt Programming
=========================

1. Interrupt Example Program in Linux Kernel
2. Functions Related to Interrupt
    2.1 Interrupts Flags:
3. Registering an Interrupt Handler
4. Freeing an Interrupt Handler
5. Interrupt Handler
6. Interrupt Example Program in Linux Kernel – Programming
    6.1 Triggering Hardware Interrupt through Software
    6.2 Driver Source Code
    6.3 MakeFile:
7. Building and Testing Driver
8. Problem in Linux kernel [Solution]
    8.1 Modify and Build the Linux Kernel
    8.2 Install the Modified kernel
    8.3 Driver source code for a modified kernel
----------------------------------------------------------------------------------------------------------------


-: 1. 🔹 Interrupt Example Program in Linux Kernel 🔹
-: **********************************************************************************
01. Interrupt handlers cannot sleep — avoid calling blocking or sleeping functions.
02. Use spinlocks instead of mutexes in interrupt handlers.
03. Interrupt handlers cannot exchange data with userspace directly.
04. Split interrupt processing into top half (quick) and bottom half 
        (deferred) (using softirqs, tasklets, or workqueues).
05. Handlers should be non-reentrant — disable their IRQ until they complete.
06. Interrupt handlers can be interrupted by higher-priority handlers.
07. Marking handlers as fast handlers should be used judiciously; overuse degrades performance by 
    increasing interrupt latency.



-: 2. 🔹 Functions Related to Interrupt 🔹
-: **********************************************************************************

-:  request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char *name, void *dev_id)

🔹 Parameter-wise Description 🔹:
✅ irq -:
        The interrupt number or IRQ vector you want to register the handler for.

✅ handler
        The interrupt handler function (also called ISR).
        Its prototype typically looks like:

-:      irqreturn_t handler(int irq, void *dev_id);

✅ flags
        Behavior flags or settings for the interrupt.
        Can be either zero or a bit mask of one or more of the flags defined in linux/interrupt.h
Examples:
            IRQF_SHARED — interrupt can be shared with other handlers.
            IRQF_TIMER — for timer interrupts.
            IRQF_ONESHOT — disable interrupt until handler finishes.

✅ name
        A string identifier for the interrupt.
        This shows up in files like /proc/interrupts.

✅ dev_id
        An optional unique identifier for distinguishing handlers when interrupts are shared.
        Often a pointer to your device structure.

-: void free_irq(unsigned int irq, void *dev_id);

🔹 Parameter-wise Description 🔹:
---------------------------------------------------
    irq
        The interrupt number or IRQ vector that you want to free.

    dev_id
        The same device identifier that was used during request_irq().
        This is used to identify the correct interrupt handler in case the interrupt is shared.

-:✅ enable_irq (unsigned int irq) -: 
        Re-enable interrupt disabled by disable_irq or disable_irq_nosync.

-:✅ disable_irq (unsigned int irq) -:
        Disable an IRQ from issuing an interrupt.

-:✅ disable_irq_nosync(unsigned int irq) -:
         Disable an IRQ from issuing an interrupt, but wait until there is an interrupt handler being executed.    

-:✅ in_irq()
            returns true when in interrupt handler.
-:✅ in_interrupt()
            returns true when in interrupt handler or bottom half.