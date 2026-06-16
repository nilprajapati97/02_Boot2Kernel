Interrupt
====================


Table of Contents

1.Interrupts in Linux kernel
2.Interrupts
3.Polling vs Interrupts
4.What will happen when the interrupt comes?
5.Interrupts and Exceptions
    5.1 Interrupts
    5.2 Exceptions
6.Interrupt handler
7.Process Context and Interrupt Context
8.Top halves and Bottom halves
    8.1 Top half
    8.2 Bottom half

-----------------------------------------------------------------

-: 🔹 What is an interrupt?
*************************************
An interrupt is a hardware or software event that temporarily halts the normal flow of instructions to deal with something that requires immediate attention.
For example:

-:  Hardware interrupt: Network packet arrives, disk I/O finishes, a timer expires.
-:  Software interrupt: Exception, syscall, or a processor interrupt vector.


-: 🔹 Types of interrupts in Linux kernel:
-----------------------------------------------------
-:  01. Hardware interrupts (IRQ):
    -: Originating from a peripheral or a controller chip (like interrupt controller).
    -: Managed by interrupt handlers or ISRs (Interrupt Service Routines) implemented by device drivers.

-:  02. Software interrupts (SoftIRQ, Tasklet):
    -: Raise under kernel control when we need deferrable or delayed processing after interrupt context.
    -: SoftIRQs are global and statically defined.
    -: Tasklets are implemented on top of SoftIRQs — lightweight, serialized handlers.

-: 🔹 How Linux handles interrupts:
-: ========================================
When a hardware interrupt occurs:
    1. Processor signals interrupt controller (PIC or APIC).
    2. Processor saves context (PC, flags).
    3. Processor parses interrupt vector to find corresponding interrupt handler in interrupt vector table.
    4. Transfers control to kernel’s interrupt handler.

-:🔹 The interrupt handlers:
-: ========================================
    1. Implemented by driver code or kernel modules.
    2. Must be fast, non-blocking, and not perform heavy workloads (since interrupts block other interrupts).
    3. Often handlers do the minimum necessary (like acknowledging interrupt, clearing interrupt flags, reading a small amount of data).
    4. If heavy processing is needed, handlers schedule a bottom half (SoftIRQ, Tasklet, or workqueue).

-: Process Context and Interrupt Context
-: ========================================

Process context:
    Kernel code executes on behalf of a process.
    Preemptible — can be interrupted by higher-priority tasks.
Interrupt context:
    Runs asynchronously, in response to hardware interrupts.
    Not preemptible — must complete as quickly as possible.

-: 🔹 Restrictions in Interrupt Context 🔹
-: ========================================
While in interrupt context, you should not:
    ❌ Go to sleep or relinquish the processor.
    ❌ Acquire a mutex or blocking lock.
    ❌ Perform time-consuming tasks.
    ❌ Access user-space virtual memory.

-: 🔹 What If We Need More Work After interrupt? 🔹
-: ========================================
✅ Problem: We shouldn’t do heavy processing directly in interrupt handlers.

✅ Solution: Defer heavy or lengthy tasks to bottom halves, work queues, or tasklets.
             This lets interrupt handlers remain fast and responsive while the heavy 
             processing happens later in process context.

-:  Top halves and Bottom halves
-: ========================================

🔹 What is Top Half? 🔹
-: ========================================
1. Top Half refers to the interrupt handler itself — the code that executes immediately 
   when the interrupt occurs.
2. Runs in interrupt context, which is not preemptible and cannot block or sleep.
3. Should execute quickly and efficiently — typically just:
    3.1 Acknowledge or clear the interrupt at the hardware level.
    3.2 Gather or save the minimum necessary data.
    3.3 Schedule or activate the bottom half for further processing.

🔹 What is Bottom Half? 🔹
-: ========================================
1. The Bottom Half is a deferrable, delayed processing routine.
2. It runs after the interrupt handler, at a convenient time in process context or kernel thread context.
3. Operations that are time-consuming or blocking can be safely performed here.

🔹 Types of Bottom Halves 🔹 (depending on kernel version)
-: ====================================================================

✅ Softirqs
✅ Tasklets — lightweight routines implemented on softirqs
✅ Work queues — kernel threads used for heavy or blocking tasks
✅ Threaded interrupts — interrupt handlers that execute in their own kernel thread context 
    (with blocking allowed)

🔹 Why Top Half and Bottom Half? 🔹
-: ========================================
    01. To keep interrupt handlers fast and lightweight, minimizing interrupt latency for other interrupts.
    02. To enable heavy or blocking workloads to be performed later, without affecting interrupt response time.

🔹 An Example 🔹 (concept)
-: ========================================

                    // Top Half (Interrupt Handler)
                    irqreturn_t my_irq_handler(int irq, void *dev_id) {
                        // Acknowledge interrupt
                        // Capture data
                        // Schedule bottom half for further processing
                        tasklet_schedule(&my_tasklet);
                        return IRQ_HANDLED;
                    }

                    // Bottom Half (Tasklet)
                    void my_tasklet_func(unsigned long data) {
                        // Perform heavy processing here
                        // This runs with interrupts enable
                    }

✅ Summary:

Top Half — fast, non-blocking, interrupt context.

Bottom Half — heavy, may block, process context or kernel thread context.
