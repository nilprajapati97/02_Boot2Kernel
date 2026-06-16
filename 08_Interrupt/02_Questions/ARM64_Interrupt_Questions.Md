1. Explain the complete ARM64 interrupt handling flow in the Linux kernel from hardware IRQ assertion to userspace wakeup.

   * Cover GICv3/GICv4, exception levels, vector tables, `el1_irq`, `handle_domain_irq()`, softirq/tasklets/workqueues, and scheduler interaction.

2. What is the difference between IRQ, FIQ, softirq, tasklet, threaded IRQ, and workqueue in the Linux kernel?

   * When would you choose one over another in a production ARM64 SoC driver?

3. How does the ARM64 Generic Interrupt Controller (GICv3) architecture work?

   * Explain Distributor, Redistributor, SGI, PPI, SPI, LPIs, affinity routing, and interrupt targeting.

4. Describe how interrupt affinity works on ARM64 SMP systems.

   * How does Linux route interrupts to CPUs, and how do `irqbalance`, CPU isolation, and `smp_affinity` affect performance?

5. What are top-half and bottom-half interrupt handlers?

   * Explain latency implications and how PREEMPT_RT changes interrupt handling behavior.

6. Explain the purpose and implementation of interrupt threading in Linux.

   * What happens internally when `request_threaded_irq()` is used?

7. What causes interrupt storms in embedded Linux systems, and how would you debug them on ARM64 hardware?

   * Mention `/proc/interrupts`, ftrace, perf, lockups, GPIO bouncing, and interrupt masking.

8. Explain how Device Tree interrupt configuration works on ARM64 Linux systems.

   * Describe `interrupt-parent`, `interrupts`, `interrupt-controller`, and GIC interrupt specifiers.

9. How does Linux handle shared interrupts?

   * What are the risks, and how do drivers properly return `IRQ_HANDLED` vs `IRQ_NONE`?

10. Explain inter-processor interrupts (IPIs) in ARM64 Linux.

* What are SGIs used for in SMP scheduling, TLB shootdown, RCU, and CPU stop operations?

11. What happens if an interrupt handler sleeps?

* Why is sleeping forbidden in hard IRQ context, and how do you move work safely to sleepable context?

12. Describe the locking considerations inside interrupt context on ARM64 Linux.

* Compare `spin_lock()`, `spin_lock_irqsave()`, raw spinlocks, mutexes, and atomic context constraints.

13. How would you measure and reduce interrupt latency on an ARM64 Linux platform?

* Discuss tracing tools, PREEMPT_RT, CPU pinning, cache locality, NAPI, and tuning techniques.

14. Explain MSI/MSI-X interrupt handling in Linux and how it differs from legacy interrupts.

* How are PCIe MSI interrupts mapped through the ARM64 ITS (Interrupt Translation Service)?

15. Walk through a real debugging scenario:

* A network driver on ARM64 shows high packet drops under load and one CPU has 100% softirq utilization.
* How would you investigate and fix it?
* Expected discussion: NAPI budget, IRQ affinity, RPS/XPS, GRO, RSS, softirq starvation, cache contention, and NUMA considerations.
