================================================================================
              TASKLETS — LINUX KERNEL BOTTOM HALF MECHANISM
================================================================================


OVERVIEW
--------------------------------------------------------------------------------
Tasklets are a bottom half mechanism in the Linux kernel, built on top of
softirqs. They provide a simple way for device drivers to defer non-critical
interrupt processing to a safer execution context while guaranteeing that the
same tasklet never runs simultaneously on multiple CPUs.


================================================================================
1. WHAT ARE TASKLETS?
================================================================================

Key Properties:
    - Built on top of softirqs (uses TASKLET_SOFTIRQ and HI_SOFTIRQ)
    - Dynamically created — any kernel module can create them
    - Same tasklet NEVER runs on two CPUs simultaneously (serialized)
    - Different tasklets CAN run on different CPUs simultaneously
    - Execute in softirq context (cannot sleep)
    - Lighter weight than work queues, simpler than raw softirqs


================================================================================
2. INTERNAL STRUCTURE
================================================================================

    struct tasklet_struct {
        struct tasklet_struct *next;       /* Linked list pointer           */
        unsigned long state;               /* TASKLET_STATE_SCHED,          */
                                           /* TASKLET_STATE_RUN             */
        atomic_t count;                    /* Reference counter (0=enabled) */
        void (*func)(unsigned long);       /* Handler function              */
        unsigned long data;                /* Argument passed to handler    */
    };

Field Descriptions:
    +----------+-------------------------------------------------------------+
    | Field    | Description                                                 |
    +----------+-------------------------------------------------------------+
    | next     | Points to next tasklet in the per-CPU linked list           |
    | state    | Bitmask tracking whether tasklet is scheduled or running    |
    | count    | When 0 the tasklet is enabled; nonzero means disabled       |
    | func     | Pointer to the callback function (bottom half handler)      |
    | data     | Opaque unsigned long passed as argument to func()           |
    +----------+-------------------------------------------------------------+

State Bits:
    TASKLET_STATE_SCHED  —  Tasklet has been scheduled and is pending
    TASKLET_STATE_RUN    —  Tasklet is currently executing on some CPU


================================================================================
3. STATE MACHINE
================================================================================

                        tasklet_schedule()
                              |
                              v
    +----------+       +--------------+       +-----------+
    |          |       |              |       |           |
    |   IDLE   |------>|  SCHEDULED   |------>|  RUNNING  |
    |          |       |  (pending)   |       |           |
    +----------+       +--------------+       +-----+-----+
         ^                                          |
         |              Completed                   |
         +------------------------------------------+

    IDLE      :  Tasklet is not scheduled and not running
    SCHEDULED :  Tasklet is on a per-CPU list, waiting to execute
    RUNNING   :  Tasklet handler function is currently executing
    Completed :  Handler returns, tasklet goes back to IDLE


================================================================================
4. DETAILED CODE FLOW — DRIVER EXAMPLE
================================================================================


4.1  STEP 1 — Define the Tasklet Handler (Bottom Half Work)
--------------------------------------------------------------------------------

    void my_tasklet_handler(unsigned long data)
    {
        struct my_device *dev = (struct my_device *)data;

        /* This runs in softirq context — CANNOT sleep */
        /* Process the data that top half saved */

        printk("Processing data: %d\n", dev->buffer_data);

        /* Parse data */
        parse_device_data(dev);

        /* Update statistics */
        dev->stats.rx_packets++;

        /* Notify userspace if needed */
        wake_up_interruptible(&dev->wait_queue);
    }


4.2  STEP 2 — Declare the Tasklet
--------------------------------------------------------------------------------

    Method A — Static declaration:

        DECLARE_TASKLET(my_tasklet, my_tasklet_handler,
                        (unsigned long)&my_device);

    Method B — Dynamic initialization:

        struct tasklet_struct my_tasklet;
        tasklet_init(&my_tasklet, my_tasklet_handler,
                     (unsigned long)&my_device);


4.3  STEP 3 — Top Half ISR (Interrupt Service Routine)
--------------------------------------------------------------------------------

    irqreturn_t my_device_isr(int irq, void *dev_id)
    {
        struct my_device *dev = dev_id;
        unsigned int status;

        /* Read interrupt status register */
        status = readl(dev->base + INT_STATUS_REG);

        /* Check if this interrupt is for us */
        if (!(status & MY_DEVICE_IRQ_BIT))
            return IRQ_NONE;

        /* Acknowledge interrupt in hardware */
        writel(status, dev->base + INT_ACK_REG);

        /* Save data quickly (time-critical) */
        dev->buffer_data = readl(dev->base + DATA_REG);

        /* Schedule tasklet for bottom half processing */
        tasklet_schedule(&my_tasklet);    /* <-- TASKLET_SOFTIRQ */

        return IRQ_HANDLED;
    }


4.4  STEP 4 — Register ISR in Probe Function
--------------------------------------------------------------------------------

    static int my_device_probe(struct platform_device *pdev)
    {
        int ret;

        ret = request_irq(dev->irq, my_device_isr,
                          IRQF_SHARED, "my_device", dev);
        if (ret) {
            dev_err(&pdev->dev, "Failed to request IRQ\n");
            return ret;
        }

        return 0;
    }


4.5  STEP 5 — Cleanup on Module/Device Removal
--------------------------------------------------------------------------------

    static int my_device_remove(struct platform_device *pdev)
    {
        tasklet_kill(&my_tasklet);    /* Wait for completion & disable */
        free_irq(dev->irq, dev);
        return 0;
    }

    Note: tasklet_kill() ensures the tasklet is not scheduled and is not
    currently running before it returns. Always call it during cleanup to
    prevent use-after-free bugs.


================================================================================
5. INTERNAL KERNEL EXECUTION FLOW
================================================================================

    Hardware Interrupt fires
            |
            v
    my_device_isr()              [TOP HALF — hardirq context]
            |
            |  tasklet_schedule(&my_tasklet)
            |         |
            |         v
            |  +----------------------------------------------+
            |  | 1. Test-and-set TASKLET_STATE_SCHED bit      |
            |  |    - If already set, return (already queued) |
            |  | 2. Add tasklet to this CPU's tasklet list    |
            |  | 3. raise_softirq(TASKLET_SOFTIRQ)            |
            |  +----------------------------------------------+
            |
            v
    Return from hardirq
            |
            v
    irq_exit()
            |
            v
    __do_softirq()               [Checks pending softirq bits]
            |
            v
    tasklet_action()             [TASKLET_SOFTIRQ handler]
            |
            v
    +-----------------------------------------------+
    | For each tasklet in this CPU's tasklet list:   |
    |                                                |
    | 1. Test TASKLET_STATE_RUN bit                  |
    |    (Is it running on another CPU?)             |
    |                                                |
    | 2. If STATE_RUN is set:                        |
    |    -> Put tasklet back in list, try later      |
    |                                                |
    | 3. If STATE_RUN is clear:                      |
    |    a. Set STATE_RUN bit (claim ownership)      |
    |    b. Clear STATE_SCHED bit                    |
    |    c. Call tasklet->func(tasklet->data)        |
    |       (This is my_tasklet_handler())           |
    |    d. Clear STATE_RUN bit (release ownership)  |
    +-----------------------------------------------+


================================================================================
6. SERIALIZATION GUARANTEE — SMP BEHAVIOR
================================================================================

    Scenario: Same tasklet is scheduled on both CPU 0 and CPU 1.

    CPU 0                               CPU 1
      |                                   |
      v                                   v
    tasklet_action()                    tasklet_action()
      |                                   |
      v                                   v
    Check my_tasklet                    Check my_tasklet
    STATE_RUN = 0                       STATE_RUN = 1 (set by CPU 0)
      |                                   |
      v                                   v
    Set STATE_RUN = 1                   Already running on CPU 0!
    Execute handler()                   Put back in list, retry later
      |
      v
    handler() completes
    Clear STATE_RUN = 0
                                        (Next softirq round on CPU 1
                                         can now pick it up if needed)

    RULE:  Same tasklet NEVER runs simultaneously on 2 CPUs.
    RULE:  Different tasklets CAN run simultaneously on different CPUs.

    This serialization means you do NOT need spinlocks to protect data
    accessed exclusively within a single tasklet's handler. However, if
    data is shared between different tasklets or other contexts, you
    still need proper locking.


================================================================================
7. TASKLET API REFERENCE
================================================================================

    +---------------------------------------+------------------------------------+
    | Function / Macro                      | Description                        |
    +---------------------------------------+------------------------------------+
    | DECLARE_TASKLET(name, func, data)     | Static tasklet declaration         |
    | DECLARE_TASKLET_DISABLED(name, f, d)  | Static declaration (disabled)      |
    | tasklet_init(t, func, data)           | Dynamic initialization             |
    | tasklet_schedule(t)                   | Schedule on TASKLET_SOFTIRQ        |
    | tasklet_hi_schedule(t)                | Schedule on HI_SOFTIRQ (higher     |
    |                                       | priority, runs before normal)      |
    | tasklet_disable(t)                    | Disable (increment count)          |
    | tasklet_disable_nosync(t)             | Disable without waiting            |
    | tasklet_enable(t)                     | Enable (decrement count)           |
    | tasklet_kill(t)                       | Wait for completion and disable    |
    | tasklet_trylock(t)                    | Try to set STATE_RUN (internal)    |
    | tasklet_unlock(t)                     | Clear STATE_RUN (internal)         |
    +---------------------------------------+------------------------------------+

    Note on disable/enable:
        tasklet_disable() increments the count field. The tasklet will not
        run while count > 0. Each tasklet_disable() must be paired with a
        tasklet_enable() call.

    Note on hi vs normal:
        tasklet_hi_schedule() places the tasklet on the HI_SOFTIRQ list,
        which is processed before TASKLET_SOFTIRQ. Use sparingly — only
        when low latency is critical.


================================================================================
8. WHEN TO USE TASKLETS
================================================================================

    USE TASKLETS WHEN:
    -----------------------------------------------------------------------
    [+] Writing a device driver that needs simple deferred work
    [+] Need deferred work in atomic context (cannot sleep)
    [+] Want automatic serialization for the same tasklet
    [+] Bottom half work is short and fast
    [+] Simple interrupt handling scenarios
    [+] No need for complex scheduling or priority control

    DO NOT USE TASKLETS WHEN:
    -----------------------------------------------------------------------
    [-] Need to sleep in the bottom half
        (e.g., allocate memory with GFP_KERNEL, copy to/from userspace,
         acquire a mutex, perform blocking I/O)
        --> Use work queues or threaded IRQs instead

    [-] Need very high performance with per-CPU processing
        --> Use softirqs instead (e.g., networking, block I/O)

    [-] Long-running processing is required
        --> Use work queues to avoid starving other softirqs

    [-] Need to run on a specific CPU or with specific scheduling policy
        --> Use threaded IRQs or dedicated kernel threads


================================================================================
9. TASKLETS VS OTHER BOTTOM HALF MECHANISMS
================================================================================

    +------------------+---------+-----------+-------------+----------------+
    | Feature          | Tasklet | Softirq   | Work Queue  | Threaded IRQ   |
    +------------------+---------+-----------+-------------+----------------+
    | Context          | Softirq | Softirq   | Process     | Process        |
    | Can sleep        | No      | No        | Yes         | Yes            |
    | Same-instance    | No      | Yes       | Depends     | No             |
    |   concurrency    |         |           |             |                |
    | Dynamic creation | Yes     | No        | Yes         | Yes            |
    | Complexity       | Low     | High      | Medium      | Low            |
    | Performance      | High    | Highest   | Lower       | Medium         |
    | Typical use      | Drivers | Net, Blk  | Filesystems | Modern drivers |
    +------------------+---------+-----------+-------------+----------------+

    Note on deprecation:
        There is ongoing discussion in the kernel community about
        deprecating tasklets in favor of threaded IRQs and work queues.
        New drivers are encouraged to use threaded IRQs
        (request_threaded_irq) when possible.


================================================================================
10. COMMON PITFALLS AND BEST PRACTICES
================================================================================

    PITFALL 1:  Sleeping inside a tasklet handler
    -----------------------------------------------------------------------
    Tasklets run in softirq context. Calling schedule(), msleep(),
    kmalloc(GFP_KERNEL), mutex_lock(), or copy_to_user() will cause
    a BUG or undefined behavior.

    PITFALL 2:  Forgetting tasklet_kill() on module removal
    -----------------------------------------------------------------------
    If the module is unloaded while a tasklet is still scheduled, the
    kernel will call into freed memory. Always call tasklet_kill() in
    your cleanup path.

    PITFALL 3:  Sharing data without proper locking
    -----------------------------------------------------------------------
    While the same tasklet is serialized against itself, data shared
    between the tasklet and the top half ISR still needs protection
    (e.g., spin_lock_irqsave / spin_unlock_irqrestore).

    PITFALL 4:  Scheduling a tasklet from within itself
    -----------------------------------------------------------------------
    A tasklet can re-schedule itself by calling tasklet_schedule() from
    within its handler. This is valid but be careful to ensure it does
    not run indefinitely, starving other processing.

    BEST PRACTICE:  Keep tasklet handlers short
    -----------------------------------------------------------------------
    Softirq processing has a time limit. If softirqs run too long,
    they are deferred to the ksoftirqd kernel thread, which runs at
    normal scheduling priority and may introduce latency.


================================================================================
11. COMPLETE EXECUTION TIMELINE
================================================================================

    TIME   EVENT                                      CONTEXT
    -----  -----------------------------------------  ----------------------
    T0     Hardware raises interrupt line              Hardware
    T1     CPU jumps to IDT entry, calls do_IRQ()     Hardirq (IRQs off)
    T2     my_device_isr() executes                   Hardirq (IRQs off)
    T3       -> Reads status, acks hardware            Hardirq
    T4       -> Copies data from device register       Hardirq
    T5       -> tasklet_schedule(&my_tasklet)          Hardirq
    T6       -> Returns IRQ_HANDLED                    Hardirq
    T7     irq_exit() called                           Transition
    T8     __do_softirq() checks pending bits          Softirq (IRQs on)
    T9     TASKLET_SOFTIRQ bit is set                  Softirq
    T10    tasklet_action() iterates per-CPU list      Softirq
    T11    my_tasklet_handler() executes               Softirq
    T12      -> Processes buffered data                Softirq
    T13      -> Updates statistics                     Softirq
    T14      -> Wakes up userspace waiters             Softirq
    T15    tasklet_action() clears RUN and SCHED       Softirq
    T16    Returns to interrupted context              Previous context


================================================================================
                        END OF DOCUMENT
================================================================================
