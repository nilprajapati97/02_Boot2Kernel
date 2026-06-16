================================================================================
       BOTTOM HALF MECHANISMS — LINUX KERNEL INTERRUPT HANDLING DEEP DIVE
================================================================================


OVERVIEW
--------------------------------------------------------------------------------
When a hardware interrupt occurs, the Linux kernel splits the processing into
two parts: a "top half" that runs immediately in interrupt context with
interrupts disabled, and a "bottom half" that defers the bulk of processing
to a later, safer time. The kernel provides four main bottom half mechanisms,
each with different trade-offs in terms of context, latency, and capability.

    Hardware Interrupt (IRQ)
             |
             v
    +-------------------+
    |     TOP HALF       |    (Hardirq Context — fast, cannot sleep)
    |   (ISR Handler)    |
    +---------+---------+
              |
              | Schedules one of these:
              v
    +--------------------------------------------------+
    |           BOTTOM HALF MECHANISMS                  |
    |                                                   |
    |  +-----------+  +----------+  +--------------+   |
    |  | Softirqs  |  | Tasklets |  | Work Queues  |   |
    |  |           |  | (on top  |  |              |   |
    |  |           |  | of       |  |              |   |
    |  |           |  | softirq) |  |              |   |
    |  +-----------+  +----------+  +--------------+   |
    |                                                   |
    |  +----------------+                               |
    |  | Threaded IRQs  |                               |
    |  +----------------+                               |
    +--------------------------------------------------+


================================================================================
================================================================================
==                                                                            ==
==                    SECTION 1: SOFTIRQS (SOFTWARE INTERRUPTS)               ==
==                                                                            ==
================================================================================
================================================================================


1.1  WHAT ARE SOFTIRQS?
--------------------------------------------------------------------------------

Key Properties:
    - Statically defined at compile time in the kernel source
    - Highest priority bottom half mechanism
    - Can run SIMULTANEOUSLY on multiple CPUs (same softirq type)
    - Limited to a fixed number of types (currently approximately 10)
    - Reserved for core kernel subsystems only
    - Require careful per-CPU locking due to concurrent execution
    - Cannot sleep — run in softirq (atomic) context


1.2  SOFTIRQ TYPES DEFINED IN THE KERNEL
--------------------------------------------------------------------------------

    /* File: include/linux/interrupt.h */

    enum {
        HI_SOFTIRQ = 0,            /* High-priority tasklets         */
        TIMER_SOFTIRQ,              /* Timer processing               */
        NET_TX_SOFTIRQ,             /* Network packet transmit        */
        NET_RX_SOFTIRQ,             /* Network packet receive         */
        BLOCK_SOFTIRQ,              /* Block device completion        */
        IRQ_POLL_SOFTIRQ,           /* IRQ polling                    */
        TASKLET_SOFTIRQ,            /* Regular (normal) tasklets      */
        SCHED_SOFTIRQ,              /* Scheduler load balancing       */
        HRTIMER_SOFTIRQ,            /* High-resolution timers         */
        RCU_SOFTIRQ,                /* RCU (Read-Copy-Update)         */
    };

    +---------------------+---------------------------------------------+
    | Softirq Type        | Purpose                                     |
    +---------------------+---------------------------------------------+
    | HI_SOFTIRQ          | High-priority tasklets (tasklet_hi_schedule)|
    | TIMER_SOFTIRQ       | Kernel timer expiration processing          |
    | NET_TX_SOFTIRQ      | Network transmit completions                |
    | NET_RX_SOFTIRQ      | Network receive processing (NAPI)           |
    | BLOCK_SOFTIRQ       | Block device I/O completion                 |
    | IRQ_POLL_SOFTIRQ    | IRQ-based polling for block devices         |
    | TASKLET_SOFTIRQ     | Normal-priority tasklet execution           |
    | SCHED_SOFTIRQ       | Scheduler inter-CPU load balancing          |
    | HRTIMER_SOFTIRQ     | High-resolution timer callbacks             |
    | RCU_SOFTIRQ         | RCU grace period and callback processing    |
    +---------------------+---------------------------------------------+

    Note: You CANNOT add new softirq types from a loadable module.
    Adding a new type requires modifying the kernel source and recompiling.
    This is by design — softirqs are reserved for core subsystems.


1.3  INTERNAL EXECUTION FLOW
--------------------------------------------------------------------------------

    Step 1: Top half (hardirq handler) sets a softirq pending bit
            |
            v
    Step 2: On return from hardirq, kernel checks pending softirqs
            (in irq_exit())
            |
            v
    Step 3: __do_softirq() is called
            |
            v
    Step 4: Iterates through all pending softirq bits (0 through 9)
            |
            v
    Step 5: Calls the registered handler for each pending softirq
            |
            v
    Step 6: If more softirqs were raised during processing:
            |
            +--- Retry up to MAX_SOFTIRQ_RESTART (10 times)
            |
            +--- If STILL pending after 10 retries:
                 Wake ksoftirqd kernel thread to handle remaining
                 (prevents softirqs from monopolizing the CPU)


1.4  DETAILED CODE FLOW
--------------------------------------------------------------------------------

    /* STEP 1: Define the softirq handler function */

    static void my_net_rx_action(struct softirq_action *a)
    {
        /* Process received network packets */
        struct sk_buff *skb;
        while ((skb = dequeue_packet())) {
            process_packet(skb);      /* Protocol stack processing */
        }
    }


    /* STEP 2: Register the softirq handler (done once during boot) */

    void __init net_dev_init(void)
    {
        open_softirq(NET_RX_SOFTIRQ, my_net_rx_action);
    }

    Note: open_softirq() simply stores the handler function pointer
    in the softirq_vec[] array at the given index. This is a one-time
    boot-time operation.


    /* STEP 3: Raise the softirq from the top half handler */

    irqreturn_t network_card_isr(int irq, void *dev_id)
    {
        /* Acknowledge hardware interrupt */
        acknowledge_interrupt(dev_id);

        /* Copy data from NIC to memory */
        copy_from_nic_buffer(dev_id);

        /* Raise softirq — schedule bottom half processing */
        raise_softirq(NET_RX_SOFTIRQ);     /* Sets pending bit */

        return IRQ_HANDLED;
    }

    What raise_softirq() does internally:
        1. Disables local interrupts
        2. Sets the bit for NET_RX_SOFTIRQ in the per-CPU pending bitmask
           (__softirq_pending on this CPU)
        3. Re-enables local interrupts

    The actual handler runs later when irq_exit() checks the pending mask.


1.5  EXECUTION CONTEXT — SMP BEHAVIOR
--------------------------------------------------------------------------------

    CPU 0                              CPU 1
      |                                  |
      v                                  v
    +--------------+              +--------------+
    | NET_RX       |              | NET_RX       |
    | softirq      |              | softirq      |     SAME softirq type
    | running      |              | running      |     runs on BOTH CPUs
    +--------------+              +--------------+     simultaneously!

    WARNING: This is why softirq handlers need very careful locking!

    Since the same softirq handler can execute concurrently on multiple
    CPUs, any shared data must be protected with appropriate locking
    (typically per-CPU data structures or spin_lock/spin_lock_bh).

    This is the fundamental difference from tasklets, where the same
    tasklet instance is serialized across CPUs.


1.6  THE ksoftirqd THREAD
--------------------------------------------------------------------------------

    Each CPU has a dedicated kernel thread: ksoftirqd/N (where N is the
    CPU number).

    Purpose:
        - Handles softirqs that could not be processed inline
        - Prevents softirq processing from starving user processes
        - Runs at normal scheduling priority (SCHED_NORMAL)

    When ksoftirqd runs:
        - After __do_softirq() has retried MAX_SOFTIRQ_RESTART (10) times
          and softirqs are still pending
        - This prevents a flood of softirqs from monopolizing the CPU

    Visible in ps output:
        ksoftirqd/0
        ksoftirqd/1
        ksoftirqd/2
        ...


1.7  WHEN TO USE SOFTIRQS
--------------------------------------------------------------------------------

    USE SOFTIRQS WHEN:
    -----------------------------------------------------------------------
    [+] Extremely high performance is needed
    [+] You are a core kernel subsystem developer
    [+] Building networking stack, block I/O layer, or timer subsystem
    [+] Code must handle concurrent execution on multiple CPUs
    [+] Per-CPU data structures are used to minimize locking
    [+] Processing must happen with minimal latency after the interrupt

    DO NOT USE SOFTIRQS WHEN:
    -----------------------------------------------------------------------
    [-] You are writing a device driver or loadable module
        (Cannot add new softirq types from modules)
    [-] You need to sleep or block in the handler
    [-] You want simple serialization without complex locking
    [-] Your use case is not a core kernel subsystem


================================================================================
================================================================================
==                                                                            ==
==                          SECTION 2: TASKLETS                               ==
==                                                                            ==
================================================================================
================================================================================


2.1  WHAT ARE TASKLETS?
--------------------------------------------------------------------------------

Key Properties:
    - Built on top of softirqs (uses TASKLET_SOFTIRQ and HI_SOFTIRQ)
    - Dynamically created — any kernel module can create them at runtime
    - Same tasklet NEVER runs on two CPUs simultaneously (serialized)
    - Different tasklets CAN run on different CPUs simultaneously
    - Cannot sleep — run in softirq (atomic) context
    - Simpler API than raw softirqs
    - Suitable for device driver bottom half processing


2.2  INTERNAL STRUCTURE
--------------------------------------------------------------------------------

    struct tasklet_struct {
        struct tasklet_struct *next;       /* Per-CPU linked list pointer */
        unsigned long state;               /* TASKLET_STATE_SCHED,        */
                                           /* TASKLET_STATE_RUN            */
        atomic_t count;                    /* Reference counter            */
                                           /* (0 = enabled, >0 = disabled) */
        void (*func)(unsigned long);       /* Handler function             */
        unsigned long data;                /* Argument passed to handler   */
    };

    +--------+---------------------------------------------------------------+
    | Field  | Description                                                   |
    +--------+---------------------------------------------------------------+
    | next   | Points to next tasklet in the per-CPU tasklet linked list     |
    | state  | Bitmask: TASKLET_STATE_SCHED (queued), TASKLET_STATE_RUN      |
    |        | (currently executing)                                         |
    | count  | When 0, tasklet is enabled. When >0, tasklet is disabled.     |
    |        | Each tasklet_disable() increments, tasklet_enable() decrements|
    | func   | Pointer to the handler function                               |
    | data   | Opaque unsigned long argument passed to func()                |
    +--------+---------------------------------------------------------------+


2.3  STATE MACHINE
--------------------------------------------------------------------------------

                            tasklet_schedule()
                                  |
                                  v
        +----------+       +--------------+       +-----------+
        |          |       |              |       |           |
        |   IDLE   |------>|  SCHEDULED   |------>|  RUNNING  |
        |          |       |  (pending)   |       |           |
        +----------+       +--------------+       +-----+-----+
             ^                                          |
             |                Completed                 |
             +------------------------------------------+


2.4  DETAILED CODE FLOW — DRIVER EXAMPLE
--------------------------------------------------------------------------------

    /* STEP 1: Define the tasklet handler (bottom half work) */

    void my_tasklet_handler(unsigned long data)
    {
        struct my_device *dev = (struct my_device *)data;

        /* This runs in softirq context — CANNOT sleep */

        printk("Processing data: %d\n", dev->buffer_data);

        /* Parse device data */
        parse_device_data(dev);

        /* Update statistics */
        dev->stats.rx_packets++;

        /* Notify userspace if needed */
        wake_up_interruptible(&dev->wait_queue);
    }


    /* STEP 2: Declare the tasklet */

    /* Static method: */
    DECLARE_TASKLET(my_tasklet, my_tasklet_handler,
                    (unsigned long)&my_device);

    /* Dynamic method: */
    struct tasklet_struct my_tasklet;
    tasklet_init(&my_tasklet, my_tasklet_handler,
                 (unsigned long)&my_device);


    /* STEP 3: Top half ISR */

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
        tasklet_schedule(&my_tasklet);     /* TASKLET_SOFTIRQ */

        return IRQ_HANDLED;
    }


    /* STEP 4: Register ISR in probe function */

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


    /* STEP 5: Cleanup on removal */

    static int my_device_remove(struct platform_device *pdev)
    {
        tasklet_kill(&my_tasklet);        /* Wait for completion, disable */
        free_irq(dev->irq, dev);
        return 0;
    }


2.5  INTERNAL KERNEL EXECUTION FLOW
--------------------------------------------------------------------------------

    Hardware Interrupt fires
            |
            v
    my_device_isr()                  [TOP HALF — hardirq context]
            |
            |  tasklet_schedule(&my_tasklet)
            |         |
            |         v
            |  +-----------------------------------------------+
            |  | 1. Test-and-set TASKLET_STATE_SCHED bit       |
            |  |    - If already set: return (already queued)   |
            |  | 2. Add tasklet to this CPU's per-CPU list      |
            |  | 3. raise_softirq(TASKLET_SOFTIRQ)             |
            |  +-----------------------------------------------+
            |
            v
    Return from hardirq
            |
            v
    irq_exit()
            |
            v
    __do_softirq()                   [Checks pending softirq bits]
            |
            v
    tasklet_action()                 [TASKLET_SOFTIRQ handler]
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


2.6  SERIALIZATION GUARANTEE — SMP BEHAVIOR
--------------------------------------------------------------------------------

    CPU 0                                   CPU 1
      |                                       |
      v                                       v
    tasklet_action()                        tasklet_action()
      |                                       |
      v                                       v
    Check my_tasklet                        Check my_tasklet
    STATE_RUN = 0                           STATE_RUN = 1 (set by CPU 0)
      |                                       |
      v                                       v
    Set STATE_RUN = 1                       Already running on CPU 0!
    Execute handler()                       Put back in list, retry later
      |
      v
    handler() completes
    Clear STATE_RUN = 0

    RULE:  Same tasklet NEVER runs simultaneously on 2 CPUs.
    RULE:  Different tasklets CAN run simultaneously on different CPUs.


2.7  WHEN TO USE TASKLETS
--------------------------------------------------------------------------------

    USE TASKLETS WHEN:
    -----------------------------------------------------------------------
    [+] Writing a device driver that needs simple deferred work
    [+] Need deferred work in atomic context (cannot sleep)
    [+] Want automatic serialization for the same tasklet
    [+] Bottom half work is short and fast
    [+] Simple interrupt handling scenarios

    DO NOT USE TASKLETS WHEN:
    -----------------------------------------------------------------------
    [-] Need to sleep (allocate memory with GFP_KERNEL, mutex, I/O)
    [-] Need very high performance with per-CPU concurrency (use softirqs)
    [-] Long-running processing is required (use work queues)
    [-] Writing a new driver (prefer threaded IRQs for modern code)


================================================================================
================================================================================
==                                                                            ==
==                        SECTION 3: WORK QUEUES                              ==
==                                                                            ==
================================================================================
================================================================================


3.1  WHAT ARE WORK QUEUES?
--------------------------------------------------------------------------------

Key Properties:
    - Run in process context (as kernel threads called "workers")
    - CAN SLEEP — this is the single biggest advantage
    - Can allocate memory, perform I/O, acquire mutexes
    - Managed by special kernel threads (kworker/*)
    - Support delayed execution via kernel timers
    - Support custom queues with tunable flags and concurrency
    - Work items are deduplicated (same item not queued twice)


3.2  ARCHITECTURE
--------------------------------------------------------------------------------

    +-----------------------------------------------------------+
    |                  WORKQUEUE SUBSYSTEM                       |
    |                                                           |
    |  +---------------+         +-------------------------+    |
    |  |  Workqueue     |         |    Worker Pool           |    |
    |  |  (logical)     |-------->|    (per-CPU or unbound)  |    |
    |  |                |         |                           |    |
    |  |  system_wq     |         |  +---------------------+ |    |
    |  |  system_long   |         |  |  Worker Thread       | |    |
    |  |  custom_wq     |         |  |  (kworker/0:1)       | |    |
    |  +---------------+         |  +---------------------+ |    |
    |                             |  +---------------------+ |    |
    |                             |  |  Worker Thread       | |    |
    |                             |  |  (kworker/0:2)       | |    |
    |                             |  +---------------------+ |    |
    |                             +-------------------------+    |
    +-----------------------------------------------------------+

    Workqueue:    Logical named queue that drivers submit work items to.
    Worker Pool:  Physical pool of kernel threads that execute work items.
    Worker:       Individual kworker/* kernel thread.

    Relationship: Many workqueues --> One worker pool --> Many workers


3.3  INTERNAL STRUCTURES
--------------------------------------------------------------------------------

    /* Work item structure */
    struct work_struct {
        atomic_long_t data;               /* Internal flags and pool info */
        struct list_head entry;           /* Linked list in worklist      */
        work_func_t func;                /* Handler function             */
    };

    /* Delayed work structure (with timer) */
    struct delayed_work {
        struct work_struct work;          /* Embedded work item           */
        struct timer_list timer;          /* Kernel timer for delay       */
        struct workqueue_struct *wq;      /* Target workqueue             */
        int cpu;                          /* Target CPU                   */
    };


3.4  TYPES OF WORK QUEUES
--------------------------------------------------------------------------------

    +------------------------+----------------------------------------------+
    | Type                   | Description                                  |
    +------------------------+----------------------------------------------+
    | system_wq              | Default general-purpose workqueue            |
    |                        | Used by schedule_work()                      |
    +------------------------+----------------------------------------------+
    | system_highpri_wq      | High-priority workers                        |
    +------------------------+----------------------------------------------+
    | system_long_wq         | For long-running work items                  |
    +------------------------+----------------------------------------------+
    | system_unbound_wq      | Not bound to specific CPU                    |
    +------------------------+----------------------------------------------+
    | Custom workqueue       | Created by driver with alloc_workqueue()     |
    +------------------------+----------------------------------------------+


3.5  DETAILED CODE FLOW — COMPLETE DRIVER EXAMPLE
--------------------------------------------------------------------------------

    #include <linux/workqueue.h>
    #include <linux/interrupt.h>
    #include <linux/slab.h>

    struct my_device {
        void __iomem *base;
        int irq;
        struct work_struct my_work;               /* Regular work        */
        struct delayed_work my_delayed_work;      /* Delayed work        */
        struct workqueue_struct *my_wq;           /* Custom workqueue    */
        char *data_buffer;
        int data_len;
        struct mutex lock;
    };


    /* STEP 1: Define work handler (RUNS IN PROCESS CONTEXT) */

    static void my_work_handler(struct work_struct *work)
    {
        struct my_device *dev = container_of(work, struct my_device,
                                              my_work);
        char *kernel_buffer;

        /* WE CAN SLEEP HERE! */

        /* Allocate memory (may sleep) */
        kernel_buffer = kmalloc(4096, GFP_KERNEL);       /* Can sleep */
        if (!kernel_buffer)
            return;

        /* Take mutex (may sleep) */
        mutex_lock(&dev->lock);                            /* Can sleep */

        /* Process data */
        memcpy(kernel_buffer, dev->data_buffer, dev->data_len);
        process_complex_data(kernel_buffer, dev->data_len);

        /* Write to file/storage (may sleep) */
        write_to_log_file(kernel_buffer, dev->data_len);  /* Can sleep */

        mutex_unlock(&dev->lock);
        kfree(kernel_buffer);

        printk(KERN_INFO "Work completed for device\n");
    }


    /* STEP 2: Define delayed work handler */

    static void my_delayed_handler(struct work_struct *work)
    {
        struct delayed_work *dwork = to_delayed_work(work);
        struct my_device *dev = container_of(dwork, struct my_device,
                                              my_delayed_work);

        printk("Delayed work executing after timeout\n");
        check_device_health(dev);

        /* Reschedule for periodic work (every 1 second) */
        schedule_delayed_work(&dev->my_delayed_work,
                              msecs_to_jiffies(1000));
    }


    /* STEP 3: Top half ISR */

    static irqreturn_t my_device_isr(int irq, void *dev_id)
    {
        struct my_device *dev = dev_id;
        unsigned int status;

        /* Read and acknowledge interrupt */
        status = readl(dev->base + INT_STATUS_REG);
        if (!(status & MY_IRQ_BIT))
            return IRQ_NONE;

        writel(status, dev->base + INT_ACK_REG);

        /* Quick data copy in top half */
        dev->data_len = readl(dev->base + DATA_LEN_REG);
        memcpy_fromio(dev->data_buffer, dev->base + DATA_REG,
                       dev->data_len);

        /* Schedule work on custom workqueue */
        queue_work(dev->my_wq, &dev->my_work);

        return IRQ_HANDLED;
    }


    /* STEP 4: Probe — Initialize everything */

    static int my_device_probe(struct platform_device *pdev)
    {
        struct my_device *dev;
        int ret;

        dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
        if (!dev)
            return -ENOMEM;

        dev->data_buffer = kmalloc(MAX_DATA_SIZE, GFP_KERNEL);

        mutex_init(&dev->lock);
        INIT_WORK(&dev->my_work, my_work_handler);
        INIT_DELAYED_WORK(&dev->my_delayed_work, my_delayed_handler);

        dev->my_wq = alloc_workqueue("my_device_wq",
                                      WQ_UNBOUND | WQ_HIGHPRI, 4);
        if (!dev->my_wq) {
            ret = -ENOMEM;
            goto err_wq;
        }

        ret = request_irq(dev->irq, my_device_isr, IRQF_SHARED,
                          "my_device", dev);
        if (ret)
            goto err_irq;

        return 0;

    err_irq:
        destroy_workqueue(dev->my_wq);
    err_wq:
        kfree(dev->data_buffer);
        return ret;
    }


    /* STEP 5: Remove — Cleanup */

    static int my_device_remove(struct platform_device *pdev)
    {
        struct my_device *dev = platform_get_drvdata(pdev);

        free_irq(dev->irq, dev);
        cancel_work_sync(&dev->my_work);
        cancel_delayed_work_sync(&dev->my_delayed_work);
        destroy_workqueue(dev->my_wq);
        kfree(dev->data_buffer);

        return 0;
    }


3.6  INTERNAL KERNEL EXECUTION FLOW
--------------------------------------------------------------------------------

    Hardware Interrupt
            |
            v
    my_device_isr()                      [hardirq context]
            |
            |  queue_work(my_wq, &my_work)
            |         |
            |         v
            |  +-------------------------------------------+
            |  | 1. Test WORK_STRUCT_PENDING bit            |
            |  |    - If set: return false (already queued) |
            |  | 2. Set WORK_STRUCT_PENDING bit             |
            |  | 3. Select appropriate worker pool          |
            |  | 4. Add work to pool's worklist             |
            |  | 5. Wake up idle worker thread              |
            |  |    OR create new one if all busy           |
            |  +-------------------------------------------+
            |
            v
    Return from ISR
            |
            v
    Meanwhile, on a worker thread (kworker/0:1):
            |
            v
    +----------------------------------------------+
    | worker_thread() loop:                         |
    |                                               |
    | 1. Check worklist for pending work            |
    | 2. Dequeue work item                          |
    | 3. Clear PENDING flag                         |
    | 4. Call work->func(work)                      |
    |    (IN PROCESS CONTEXT — CAN SLEEP!)          |
    | 5. Go back to sleep if no more work           |
    +----------------------------------------------+


3.7  WORKQUEUE FLAGS EXPLAINED
--------------------------------------------------------------------------------

    /* WQ_UNBOUND — not bound to specific CPU */
    alloc_workqueue("unbound_wq", WQ_UNBOUND, 0);
    /* Scheduler can migrate workers to any CPU */

    /* WQ_HIGHPRI — high priority workers */
    alloc_workqueue("highpri_wq", WQ_HIGHPRI, 0);
    /* Work items serviced before normal-priority items */

    /* WQ_MEM_RECLAIM — guaranteed forward progress under memory pressure */
    alloc_workqueue("memreclaim_wq", WQ_MEM_RECLAIM, 0);
    /* Allocates a rescuer thread as fallback */

    /* WQ_FREEZABLE — participates in system suspend */
    alloc_workqueue("freezable_wq", WQ_FREEZABLE, 0);
    /* Work paused during freeze, resumed after thaw */

    /* Combination with max active limit */
    alloc_workqueue("combo_wq",
                    WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM,
                    4);       /* max 4 active work items */


3.8  WHEN TO USE WORK QUEUES
--------------------------------------------------------------------------------

    USE WORK QUEUES WHEN:
    -----------------------------------------------------------------------
    [+] Bottom half needs to SLEEP
    [+] Need to allocate memory with GFP_KERNEL
    [+] Need to acquire mutex/semaphore
    [+] Need to do I/O operations
    [+] Need to copy data to/from userspace
    [+] Long-running processing required
    [+] Need to use most kernel APIs that may sleep

    DO NOT USE WORK QUEUES WHEN:
    -----------------------------------------------------------------------
    [-] Ultra-low latency required (use softirq/tasklet)
    [-] Very simple processing (context switch overhead is unnecessary)


================================================================================
================================================================================
==                                                                            ==
==                        SECTION 4: THREADED IRQs                            ==
==                                                                            ==
================================================================================
================================================================================


4.1  WHAT ARE THREADED IRQs?
--------------------------------------------------------------------------------

Key Properties:
    - Modern PREFERRED approach for interrupt handling
    - Combines top half and bottom half in a clean single API call
    - Bottom half runs as a dedicated kernel thread (irq/XX-name)
    - Uses request_threaded_irq() instead of request_irq()
    - Thread runs in full process context — CAN SLEEP
    - Supports IRQF_ONESHOT to prevent interrupt storms
    - Compatible with PREEMPT_RT real-time kernels
    - Top half handler can be NULL (thread-only mode)


4.2  ARCHITECTURE
--------------------------------------------------------------------------------

    request_threaded_irq(irq, handler, thread_fn, flags, name, dev)
                                |            |
                                |            |
                          +-----+            +------+
                          v                          v
                  +----------------+        +--------------------+
                  |   TOP HALF     |        |  THREADED HANDLER  |
                  |   (hardirq)    | -----> |  (kernel thread)   |
                  |   handler()    | wakes  |  thread_fn()       |
                  |                |        |                    |
                  | Quick check    |        | Full processing    |
                  | Return         |        | CAN SLEEP          |
                  | IRQ_WAKE_      |        | Process context    |
                  | THREAD         |        |                    |
                  +----------------+        +--------------------+


4.3  DETAILED CODE FLOW — COMPLETE DRIVER EXAMPLE
--------------------------------------------------------------------------------

    #include <linux/interrupt.h>
    #include <linux/module.h>
    #include <linux/i2c.h>

    struct sensor_device {
        struct i2c_client *client;
        struct mutex lock;
        int irq;
        u8 status;
        int sensor_data[10];
    };


    /* TOP HALF: Quick hardware check */

    static irqreturn_t sensor_hard_irq(int irq, void *dev_id)
    {
        struct sensor_device *sensor = dev_id;

        /*
         * HARDIRQ CONTEXT — Cannot sleep!
         * Do minimal work here.
         *
         * For I2C/SPI devices, we CANNOT read registers here
         * (bus access requires sleeping).
         */

        /* For memory-mapped devices: verify interrupt is ours */
        sensor->status = readl(sensor->base + STATUS_REG);
        if (!(sensor->status & SENSOR_IRQ_ACTIVE))
            return IRQ_NONE;              /* Not our interrupt */

        /* Acknowledge interrupt at hardware level */
        writel(SENSOR_IRQ_ACK, sensor->base + ACK_REG);

        return IRQ_WAKE_THREAD;           /* Wake the threaded handler */
    }


    /* BOTTOM HALF: Full processing in thread */

    static irqreturn_t sensor_thread_fn(int irq, void *dev_id)
    {
        struct sensor_device *sensor = dev_id;
        int ret, i;
        u8 data[20];

        /*
         * PROCESS CONTEXT — CAN SLEEP!
         * Full kernel API available.
         */

        /* Take mutex (sleeps if contended) */
        mutex_lock(&sensor->lock);

        /* Read sensor data via I2C (SLEEPS!) */
        ret = i2c_smbus_read_i2c_block_data(
            sensor->client, SENSOR_DATA_REG, sizeof(data), data);

        if (ret < 0) {
            dev_err(&sensor->client->dev, "Failed to read sensor\n");
            mutex_unlock(&sensor->lock);
            return IRQ_HANDLED;
        }

        /* Process data */
        for (i = 0; i < 10; i++)
            sensor->sensor_data[i] = (data[i*2] << 8) | data[i*2+1];

        /* Log data (may involve file I/O — sleeps) */
        log_sensor_reading(sensor);

        /* Notify userspace via sysfs */
        sysfs_notify(&sensor->client->dev.kobj, NULL, "sensor_data");

        mutex_unlock(&sensor->lock);

        return IRQ_HANDLED;
    }


    /* PROBE: Register threaded IRQ */

    static int sensor_probe(struct i2c_client *client)
    {
        struct sensor_device *sensor;
        int ret;

        sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
        if (!sensor)
            return -ENOMEM;

        sensor->client = client;
        sensor->irq = client->irq;
        mutex_init(&sensor->lock);

        /* Option A: Both top half and bottom half */
        ret = request_threaded_irq(
            sensor->irq,
            sensor_hard_irq,              /* Top half handler    */
            sensor_thread_fn,             /* Bottom half thread  */
            IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
            "sensor_irq",
            sensor
        );

        /* Option B: Thread-only (NULL top half) */
        ret = request_threaded_irq(
            sensor->irq,
            NULL,                         /* Default handler     */
            sensor_thread_fn,
            IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
            "sensor_irq",
            sensor
        );

        /* Option C: devm version (auto-cleanup) */
        ret = devm_request_threaded_irq(
            &client->dev,
            sensor->irq,
            sensor_hard_irq,
            sensor_thread_fn,
            IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
            "sensor_irq",
            sensor
        );

        i2c_set_clientdata(client, sensor);
        return 0;
    }


    /* REMOVE */

    static void sensor_remove(struct i2c_client *client)
    {
        struct sensor_device *sensor = i2c_get_clientdata(client);

        /* If NOT using devm version: */
        free_irq(sensor->irq, sensor);
    }


4.4  INTERNAL KERNEL EXECUTION FLOW
--------------------------------------------------------------------------------

    Hardware Interrupt fires
            |
            v
    Generic IRQ handling code (kernel/irq/handle.c)
            |
            v
    handle_irq_event_percpu()
            |
            v
    sensor_hard_irq()                       [HARDIRQ CONTEXT]
            |
            | Returns IRQ_WAKE_THREAD
            |
            v
    __irq_wake_thread()
            |
            v
    +------------------------------------------------+
    | 1. Find the irqaction's dedicated thread        |
    |    (created during request_threaded_irq)        |
    | 2. Set IRQTF_RUNTHREAD flag                     |
    | 3. wake_up_process(action->thread)              |
    |                                                 |
    | If IRQF_ONESHOT:                                |
    |    IRQ line stays MASKED until thread            |
    |    completes (prevents interrupt storm)          |
    +------------------------------------------------+
            |
            v
    irq_thread()                            [PROCESS CONTEXT]
    (kernel thread: irq/XX-sensor_irq)
            |
            v
    +------------------------------------------------+
    | Kernel thread loop:                             |
    |                                                 |
    | 1. Wait for IRQTF_RUNTHREAD flag                |
    | 2. Call sensor_thread_fn()                       |
    |    (IN PROCESS CONTEXT — CAN SLEEP!)            |
    | 3. If IRQF_ONESHOT: unmask IRQ line             |
    | 4. Go back to sleep                              |
    +------------------------------------------------+


4.5  IRQF_ONESHOT EXPLAINED
--------------------------------------------------------------------------------

    WITHOUT IRQF_ONESHOT (Problem — Interrupt Storm):
    -----------------------------------------------------------------------

    Time --->

    IRQ line: --+    +--+    +--+    +--   (interrupt keeps firing!)
                |    |  |    |  |    |
                +----+  +----+  +----+

    hardirq:    [H1]   [H2]   [H3]        (top half runs repeatedly)
    thread:     [.........T1........]      (thread cannot keep up!)

    The device keeps interrupting because the thread has not cleared
    the interrupt source yet. This creates an INTERRUPT STORM.


    WITH IRQF_ONESHOT (Solution):
    -----------------------------------------------------------------------

    Time --->

    IRQ line: --+                         +--
                |                         |
                +-------------------------+
                MASKED                    UNMASKED

    hardirq:    [H1]
    thread:           [.........T1........]
                                          ^
                                     Thread completes,
                                     clears interrupt source,
                                     kernel unmasks IRQ line

    IRQ stays masked until thread completes processing.
    No interrupt storm possible.


4.6  WHEN TO USE THREADED IRQs
--------------------------------------------------------------------------------

    USE THREADED IRQs WHEN:
    -----------------------------------------------------------------------
    [+] Writing modern device drivers (PREFERRED approach)
    [+] Device uses slow bus (I2C, SPI) — must sleep to read
    [+] Need clean top-half / bottom-half separation
    [+] Want automatic thread management by the kernel
    [+] Need IRQF_ONESHOT for level/edge-triggered interrupts
    [+] Real-time (PREEMPT_RT) kernel compatibility needed

    DO NOT USE THREADED IRQs WHEN:
    -----------------------------------------------------------------------
    [-] Ultra-high frequency interrupts (networking at line rate)
    [-] Simple interrupt that can be handled entirely in top half


================================================================================
================================================================================
==                                                                            ==
==                     SECTION 5: COMPLETE COMPARISON TABLE                   ==
==                                                                            ==
================================================================================
================================================================================

    +--------------+-----------+-----------+------------+--------------+
    | Feature      | Softirq   | Tasklet   | Work Queue | Threaded IRQ |
    +--------------+-----------+-----------+------------+--------------+
    | Context      | Softirq   | Softirq   | Process    | Process      |
    |              | (atomic)  | (atomic)  |            |              |
    +--------------+-----------+-----------+------------+--------------+
    | Can Sleep?   | NO        | NO        | YES        | YES          |
    +--------------+-----------+-----------+------------+--------------+
    | Can use      | NO        | NO        | YES        | YES          |
    | mutex?       |           |           |            |              |
    +--------------+-----------+-----------+------------+--------------+
    | Memory       | GFP_      | GFP_      | GFP_KERNEL | GFP_KERNEL   |
    | allocation   | ATOMIC    | ATOMIC    |            |              |
    |              | only      | only      |            |              |
    +--------------+-----------+-----------+------------+--------------+
    | Parallel     | YES       | NO        | Depends on | Per-IRQ      |
    | execution    | Same type | Same      | workqueue  | thread       |
    | on multi-CPU | on multi  | tasklet   | config     |              |
    |              | CPUs      | serialized|            |              |
    +--------------+-----------+-----------+------------+--------------+
    | Creation     | Static    | Dynamic   | Dynamic    | Dynamic      |
    |              | (compile) | (runtime) | (runtime)  | (runtime)    |
    +--------------+-----------+-----------+------------+--------------+
    | Latency      | Lowest    | Low       | Higher     | Higher       |
    +--------------+-----------+-----------+------------+--------------+
    | Preemptible? | NO        | NO        | YES        | YES          |
    +--------------+-----------+-----------+------------+--------------+
    | Complexity   | High      | Low       | Medium     | Low          |
    +--------------+-----------+-----------+------------+--------------+
    | Who uses?    | Kernel    | Drivers   | Drivers,   | Drivers      |
    |              | core      |           | Subsystems |              |
    +--------------+-----------+-----------+------------+--------------+
    | Locking      | spin_lock | spin_lock | mutex OK,  | mutex OK,    |
    | required     | _bh       | _bh       | spin OK    | spin OK      |
    +--------------+-----------+-----------+------------+--------------+


================================================================================
================================================================================
==                                                                            ==
==                  SECTION 6: REAL-WORLD PRACTICAL EXAMPLES                  ==
==                                                                            ==
================================================================================
================================================================================


6.1  EXAMPLE 1 — NETWORK CARD DRIVER (Softirq + NAPI)
================================================================================

    Scenario:   High-speed network card receiving thousands of packets/sec
    Why Softirq: Maximum throughput, minimum latency
    Real Driver: Similar to Intel e1000e, Realtek r8169

    -----------------------------------------------------------------------

    /* This uses NAPI which is built on softirqs */

    struct my_nic {
        struct net_device *netdev;
        struct napi_struct napi;
        void __iomem *base;
        struct sk_buff_head rx_queue;
    };


    /* TOP HALF — Minimal work */

    static irqreturn_t nic_interrupt(int irq, void *dev_id)
    {
        struct my_nic *nic = dev_id;
        u32 status;

        status = readl(nic->base + NIC_INT_STATUS);
        if (!status)
            return IRQ_NONE;

        /* Acknowledge interrupt */
        writel(status, nic->base + NIC_INT_ACK);

        /* DISABLE further interrupts from NIC */
        writel(0, nic->base + NIC_INT_MASK);

        /* Schedule NAPI poll (uses NET_RX_SOFTIRQ) */
        napi_schedule(&nic->napi);

        return IRQ_HANDLED;
    }


    /* BOTTOM HALF — NAPI poll (runs in softirq context) */

    static int nic_poll(struct napi_struct *napi, int budget)
    {
        struct my_nic *nic = container_of(napi, struct my_nic, napi);
        int packets_processed = 0;

        /* Process up to 'budget' packets */
        while (packets_processed < budget) {
            struct sk_buff *skb;
            u32 desc_status;

            /* Check if more packets in ring buffer */
            desc_status = readl(nic->base + RX_DESC_STATUS);
            if (!(desc_status & RX_DESC_DONE))
                break;

            /* Allocate skb */
            skb = netdev_alloc_skb(nic->netdev, PKT_SIZE);
            if (!skb)
                break;

            /* Copy packet data */
            memcpy_fromio(skb->data, nic->base + RX_DATA,
                          desc_status & RX_LEN_MASK);
            skb_put(skb, desc_status & RX_LEN_MASK);

            /* Set protocol */
            skb->protocol = eth_type_trans(skb, nic->netdev);

            /* Pass to network stack */
            napi_gro_receive(napi, skb);

            packets_processed++;
        }

        /* If we processed all packets, re-enable interrupts */
        if (packets_processed < budget) {
            napi_complete(napi);
            /* Re-enable NIC interrupts */
            writel(NIC_INT_ALL, nic->base + NIC_INT_MASK);
        }

        return packets_processed;
    }


    /* PROBE */

    static int nic_probe(struct pci_dev *pdev,
                          const struct pci_device_id *id)
    {
        struct my_nic *nic;
        struct net_device *netdev;

        netdev = alloc_etherdev(sizeof(struct my_nic));
        nic = netdev_priv(netdev);
        nic->netdev = netdev;

        /* Register NAPI */
        netif_napi_add(netdev, &nic->napi, nic_poll, NAPI_POLL_WEIGHT);

        /* Request IRQ */
        request_irq(pdev->irq, nic_interrupt, IRQF_SHARED,
                     "my_nic", nic);

        /* Enable NAPI */
        napi_enable(&nic->napi);

        return register_netdev(netdev);
    }


    Execution Flow:

        Packet arrives at NIC
              |
              v
        NIC raises hardware interrupt
              |
              v
        nic_interrupt() [HARDIRQ]
          - ACK interrupt
          - Disable NIC interrupts       <-- Switch to polling mode
          - napi_schedule()              --> Raises NET_RX_SOFTIRQ
              |
              v
        Softirq runs: net_rx_action()
              |
              v
        nic_poll() [SOFTIRQ CONTEXT]
          - Process up to 64 packets (budget)
          - Pass packets to network stack
          - If done: re-enable NIC interrupts
          - If more: stay in polling mode
              |
              v
        Packets go through TCP/IP stack
              |
              v
        Data delivered to socket


6.2  EXAMPLE 2 — GPIO BUTTON DRIVER (Tasklet)
================================================================================

    Scenario:   Simple GPIO button on an embedded board
    Why Tasklet: Simple, no sleep needed, just process button event
    Real Driver: Similar to gpio_keys driver

    -----------------------------------------------------------------------

    struct button_device {
        int gpio;
        int irq;
        struct tasklet_struct tasklet;
        struct input_dev *input;
        unsigned long press_time;
        int press_count;
        bool pressed;
    };


    /* TASKLET HANDLER (bottom half) */

    static void button_tasklet_handler(unsigned long data)
    {
        struct button_device *btn = (struct button_device *)data;
        int state;

        /* Read current GPIO state (debounced) */
        state = gpio_get_value(btn->gpio);

        if (state == 0) {       /* Button pressed (active low) */
            btn->press_time = jiffies;
            btn->pressed = true;
            btn->press_count++;

            /* Report button press to input subsystem */
            input_report_key(btn->input, KEY_POWER, 1);
            input_sync(btn->input);

            printk("Button pressed! Count: %d\n", btn->press_count);

        } else {                /* Button released */
            unsigned long duration = jiffies - btn->press_time;
            btn->pressed = false;

            /* Report button release */
            input_report_key(btn->input, KEY_POWER, 0);
            input_sync(btn->input);

            printk("Button released! Duration: %lu ms\n",
                   jiffies_to_msecs(duration));

            /* Long press detection */
            if (jiffies_to_msecs(duration) > 3000) {
                printk("LONG PRESS detected - triggering shutdown\n");
            }
        }
    }


    /* TOP HALF ISR */

    static irqreturn_t button_isr(int irq, void *dev_id)
    {
        struct button_device *btn = dev_id;

        /* Schedule tasklet for processing */
        tasklet_schedule(&btn->tasklet);

        return IRQ_HANDLED;
    }


    /* PROBE */

    static int button_probe(struct platform_device *pdev)
    {
        struct button_device *btn;
        int ret;

        btn = devm_kzalloc(&pdev->dev, sizeof(*btn), GFP_KERNEL);

        /* Setup GPIO */
        btn->gpio = of_get_gpio(pdev->dev.of_node, 0);
        gpio_request(btn->gpio, "button");
        gpio_direction_input(btn->gpio);

        /* Get IRQ from GPIO */
        btn->irq = gpio_to_irq(btn->gpio);

        /* Initialize tasklet */
        tasklet_init(&btn->tasklet, button_tasklet_handler,
                     (unsigned long)btn);

        /* Setup input device */
        btn->input = input_allocate_device();
        btn->input->name = "Power Button";
        set_bit(EV_KEY, btn->input->evbit);
        set_bit(KEY_POWER, btn->input->keybit);
        input_register_device(btn->input);

        /* Request IRQ */
        ret = request_irq(btn->irq, button_isr,
                          IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                          "gpio_button", btn);

        return ret;
    }


    Execution Flow:

        User presses button
              |
              v
        GPIO pin goes LOW
              |
              v
        GPIO controller raises interrupt
              |
              v
        button_isr() [HARDIRQ]
          - tasklet_schedule()
              |
              v
        tasklet_action() [SOFTIRQ]
              |
              v
        button_tasklet_handler()
          - Read GPIO state
          - Detect press/release
          - Report to input subsystem
          - Log press duration
              |
              v
        Input event delivered to userspace
        (/dev/input/eventX)


6.3  EXAMPLE 3 — I2C TEMPERATURE SENSOR (Threaded IRQ)
================================================================================

    Scenario:   I2C temperature sensor with alert interrupt
    Why Threaded: Must use I2C bus (sleeps!) to read temperature
    Real Driver: Similar to TMP102, LM75, BME280 drivers

    -----------------------------------------------------------------------

    struct temp_sensor {
        struct i2c_client *client;
        struct mutex lock;
        int irq;
        int temperature;                 /* millidegrees Celsius */
        int alert_threshold;
        struct thermal_zone_device *tz;
        bool alert_active;
    };


    /* TOP HALF — Cannot read I2C here! */

    static irqreturn_t temp_sensor_hard_irq(int irq, void *data)
    {
        /*
         * For I2C devices, we typically CANNOT verify if the
         * interrupt is ours (would need I2C read = sleep!).
         * So we just wake the thread unconditionally.
         */
        return IRQ_WAKE_THREAD;
    }


    /* BOTTOM HALF — Full processing in thread context */

    static irqreturn_t temp_sensor_thread_fn(int irq, void *data)
    {
        struct temp_sensor *sensor = data;
        int ret;
        u8 status;
        u8 buf[2];

        /* ALL OF THIS CAN SLEEP! */

        mutex_lock(&sensor->lock);

        /* Read alert status register via I2C (SLEEPS!) */
        ret = i2c_smbus_read_byte_data(sensor->client,
                                        TEMP_STATUS_REG);
        if (ret < 0) {
            dev_err(&sensor->client->dev, "Failed to read status\n");
            mutex_unlock(&sensor->lock);
            return IRQ_NONE;
        }
        status = ret;

        /* Check if this interrupt is from our sensor */
        if (!(status & TEMP_ALERT_FLAG)) {
            mutex_unlock(&sensor->lock);
            return IRQ_NONE;          /* Not our interrupt */
        }

        /* Read temperature register via I2C (SLEEPS!) */
        ret = i2c_smbus_read_i2c_block_data(sensor->client,
                                              TEMP_DATA_REG, 2, buf);
        if (ret < 0) {
            dev_err(&sensor->client->dev, "Failed to read temp\n");
            mutex_unlock(&sensor->lock);
            return IRQ_HANDLED;
        }

        /* Convert raw data to temperature */
        sensor->temperature = (((buf[0] << 8) | buf[1]) >> 4) * 625;

        dev_info(&sensor->client->dev,
                 "Temperature alert! Current: %d.%03d C "
                 "(threshold: %d.%03d C)\n",
                 sensor->temperature / 1000,
                 sensor->temperature % 1000,
                 sensor->alert_threshold / 1000,
                 sensor->alert_threshold % 1000);

        /* Update thermal zone (may trigger cooling) */
        if (sensor->tz)
            thermal_zone_device_update(sensor->tz,
                                        THERMAL_EVENT_UNSPECIFIED);

        /* Clear alert flag via I2C (SLEEPS!) */
        i2c_smbus_write_byte_data(sensor->client,
                                   TEMP_STATUS_REG, TEMP_ALERT_CLEAR);

        /* Notify userspace */
        sysfs_notify(&sensor->client->dev.kobj, NULL, "temperature");
        kobject_uevent(&sensor->client->dev.kobj, KOBJ_CHANGE);

        sensor->alert_active = true;

        mutex_unlock(&sensor->lock);

        return IRQ_HANDLED;
    }


    /* PROBE */

    static int temp_sensor_probe(struct i2c_client *client)
    {
        struct temp_sensor *sensor;
        int ret;

        sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
        if (!sensor)
            return -ENOMEM;

        sensor->client = client;
        sensor->irq = client->irq;
        sensor->alert_threshold = 85000;      /* 85 degrees C */
        mutex_init(&sensor->lock);

        /* Configure sensor */
        i2c_smbus_write_byte_data(client, TEMP_CONFIG_REG,
                                   TEMP_ALERT_ENABLE);

        /* Set alert threshold */
        i2c_smbus_write_word_data(client, TEMP_ALERT_HIGH_REG,
                                   cpu_to_be16(85 << 4));

        /* Register threaded IRQ */
        ret = devm_request_threaded_irq(
            &client->dev,
            sensor->irq,
            temp_sensor_hard_irq,         /* Top half              */
            temp_sensor_thread_fn,        /* Bottom half (thread)  */
            IRQF_TRIGGER_LOW |            /* Active low alert pin  */
            IRQF_ONESHOT |                /* Keep masked until done*/
            IRQF_SHARED,                  /* Shared IRQ line       */
            "temp_alert",
            sensor
        );

        if (ret) {
            dev_err(&client->dev, "Failed to request IRQ %d: %d\n",
                    sensor->irq, ret);
            return ret;
        }

        /* Register thermal zone */
        sensor->tz = thermal_zone_device_register("cpu_temp", 1, 0,
                                                    sensor, &tz_ops,
                                                    NULL, 0, 1000);

        i2c_set_clientdata(client, sensor);

        dev_info(&client->dev, "Temp sensor initialized, IRQ %d\n",
                 sensor->irq);

        return 0;
    }


    Execution Flow:

        Temperature exceeds 85 degrees C
              |
              v
        Sensor pulls ALERT pin LOW
              |
              v
        GPIO controller detects level change
              |
              v
        Hardware interrupt raised
              |
              v
        temp_sensor_hard_irq() [HARDIRQ]
          - Cannot read I2C here! Just wake thread
          - Return IRQ_WAKE_THREAD
          - IRQ line stays MASKED (IRQF_ONESHOT)
              |
              v
        Kernel wakes irq/XX-temp_alert thread
              |
              v
        temp_sensor_thread_fn() [PROCESS CONTEXT]
          - mutex_lock()                           <-- SLEEPS
          - i2c_smbus_read_byte_data()             <-- SLEEPS (status)
          - i2c_smbus_read_i2c_block_data()        <-- SLEEPS (temp)
          - Convert raw data to temperature
          - Update thermal zone
          - i2c_smbus_write_byte_data()            <-- SLEEPS (clear)
          - sysfs_notify() (notify userspace)
          - mutex_unlock()
              |
              v
        Thread returns IRQ_HANDLED
              |
              v
        Kernel unmasks IRQ line (IRQF_ONESHOT complete)
              |
              v
        If temperature still high: sensor re-triggers alert


6.4  EXAMPLE 4 — USB STORAGE DRIVER (Work Queue)
================================================================================

    Scenario:   USB storage device with data processing
    Why Work Queue: Need to sleep for USB transfers, complex processing,
                    work can be triggered from multiple contexts
    Real Driver: Similar to usb-storage / uas drivers

    -----------------------------------------------------------------------

    struct usb_storage {
        struct usb_device *udev;
        struct usb_interface *intf;
        struct work_struct rx_work;
        struct delayed_work health_check;
        struct workqueue_struct *wq;
        struct urb *bulk_in_urb;
        unsigned char *bulk_in_buffer;
        size_t bulk_in_size;
        struct mutex io_mutex;
        spinlock_t buf_lock;
        bool connected;
        int errors;
    };


    /* USB COMPLETION CALLBACK (runs in interrupt context!) */

    static void usb_bulk_callback(struct urb *urb)
    {
        struct usb_storage *storage = urb->context;

        /* THIS IS LIKE A TOP HALF — interrupt context! */

        spin_lock(&storage->buf_lock);

        switch (urb->status) {
        case 0:              /* Success */
            storage->bulk_in_size = urb->actual_length;
            break;
        case -ENOENT:
        case -ECONNRESET:
        case -ESHUTDOWN:
            storage->connected = false;
            spin_unlock(&storage->buf_lock);
            return;
        default:
            storage->errors++;
            break;
        }

        spin_unlock(&storage->buf_lock);

        /* Schedule work queue for processing (CANNOT sleep here!) */
        queue_work(storage->wq, &storage->rx_work);
    }


    /* WORK HANDLER (process context — can sleep) */

    static void usb_rx_work_handler(struct work_struct *work)
    {
        struct usb_storage *storage = container_of(work,
                                        struct usb_storage, rx_work);
        unsigned char *data;
        size_t len;
        int ret;

        /* CAN SLEEP */

        mutex_lock(&storage->io_mutex);

        if (!storage->connected) {
            mutex_unlock(&storage->io_mutex);
            return;
        }

        /* Allocate processing buffer */
        data = kmalloc(storage->bulk_in_size, GFP_KERNEL);
        if (!data) {
            mutex_unlock(&storage->io_mutex);
            return;
        }

        /* Copy data from URB buffer */
        spin_lock_irq(&storage->buf_lock);
        memcpy(data, storage->bulk_in_buffer, storage->bulk_in_size);
        len = storage->bulk_in_size;
        spin_unlock_irq(&storage->buf_lock);

        /* Process SCSI command response */
        ret = process_scsi_response(storage, data, len);

        /* Write to block device (SLEEPS!) */
        if (ret == 0)
            submit_bio_and_wait(storage, data, len);

        /* Resubmit URB for next transfer */
        ret = usb_submit_urb(storage->bulk_in_urb, GFP_KERNEL);
        if (ret)
            dev_err(&storage->intf->dev,
                    "URB resubmit failed: %d\n", ret);

        kfree(data);
        mutex_unlock(&storage->io_mutex);
    }


    /* DELAYED WORK: Periodic health check */

    static void health_check_handler(struct work_struct *work)
    {
        struct delayed_work *dwork = to_delayed_work(work);
        struct usb_storage *storage = container_of(dwork,
                                        struct usb_storage, health_check);
        int ret;

                mutex_lock(&storage->io_mutex);

        if (!storage->connected) {
            mutex_unlock(&storage->io_mutex);
            return;
        }

        /* Send TEST UNIT READY SCSI command (SLEEPS!) */
        ret = usb_control_msg(storage->udev,
                               usb_sndctrlpipe(storage->udev, 0),
                               0xFE,
                               USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                               0, 0, NULL, 0,
                               5000);       /* 5 second timeout — SLEEPS */

        if (ret < 0) {
            storage->errors++;
            dev_warn(&storage->intf->dev,
                     "Health check failed: %d\n", ret);
        }

        mutex_unlock(&storage->io_mutex);

        /* Reschedule health check every 30 seconds */
        if (storage->connected) {
            queue_delayed_work(storage->wq, &storage->health_check,
                              msecs_to_jiffies(30000));
        }
    }


    /* PROBE */

    static int usb_storage_probe(struct usb_interface *intf,
                                  const struct usb_device_id *id)
    {
        struct usb_storage *storage;
        struct usb_endpoint_descriptor *bulk_in;
        int ret;

        storage = kzalloc(sizeof(*storage), GFP_KERNEL);
        if (!storage)
            return -ENOMEM;

        storage->udev = usb_get_dev(interface_to_usbdev(intf));
        storage->intf = intf;
        storage->connected = true;

        mutex_init(&storage->io_mutex);
        spin_lock_init(&storage->buf_lock);

        /* Initialize work items */
        INIT_WORK(&storage->rx_work, usb_rx_work_handler);
        INIT_DELAYED_WORK(&storage->health_check, health_check_handler);

        /* Create dedicated workqueue */
        storage->wq = alloc_workqueue("usb_storage_wq",
                                       WQ_MEM_RECLAIM | WQ_UNBOUND,
                                       2);       /* Max 2 active */
        if (!storage->wq) {
            ret = -ENOMEM;
            goto err_wq;
        }

        /* Find bulk-in endpoint */
        ret = usb_find_bulk_in_endpoint(intf->cur_altsetting, &bulk_in);
        if (ret) {
            dev_err(&intf->dev, "No bulk-in endpoint found\n");
            goto err_ep;
        }

        /* Allocate URB and buffer */
        storage->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!storage->bulk_in_urb) {
            ret = -ENOMEM;
            goto err_ep;
        }

        storage->bulk_in_buffer = kmalloc(512, GFP_KERNEL);
        if (!storage->bulk_in_buffer) {
            ret = -ENOMEM;
            goto err_buf;
        }

        /* Setup and submit URB */
        usb_fill_bulk_urb(storage->bulk_in_urb,
                           storage->udev,
                           usb_rcvbulkpipe(storage->udev,
                               bulk_in->bEndpointAddress),
                           storage->bulk_in_buffer,
                           512,
                           usb_bulk_callback,       /* Completion handler */
                           storage);                 /* Context            */

        ret = usb_submit_urb(storage->bulk_in_urb, GFP_KERNEL);
        if (ret) {
            dev_err(&intf->dev, "URB submit failed: %d\n", ret);
            goto err_submit;
        }

        /* Start periodic health check (first run after 30 seconds) */
        queue_delayed_work(storage->wq, &storage->health_check,
                          msecs_to_jiffies(30000));

        usb_set_intfdata(intf, storage);

        dev_info(&intf->dev, "USB storage device initialized\n");

        return 0;

    err_submit:
        kfree(storage->bulk_in_buffer);
    err_buf:
        usb_free_urb(storage->bulk_in_urb);
    err_ep:
        destroy_workqueue(storage->wq);
    err_wq:
        usb_put_dev(storage->udev);
        kfree(storage);
        return ret;
    }


    /* DISCONNECT — Cleanup */

    static void usb_storage_disconnect(struct usb_interface *intf)
    {
        struct usb_storage *storage = usb_get_intfdata(intf);

        /* Mark as disconnected (prevents new work from doing I/O) */
        storage->connected = false;

        /* Kill the URB (stops completion callbacks) */
        usb_kill_urb(storage->bulk_in_urb);

        /* Cancel all pending and running work */
        cancel_work_sync(&storage->rx_work);
        cancel_delayed_work_sync(&storage->health_check);

        /* Destroy custom workqueue */
        destroy_workqueue(storage->wq);

        /* Free USB resources */
        usb_free_urb(storage->bulk_in_urb);
        kfree(storage->bulk_in_buffer);
        usb_put_dev(storage->udev);

        kfree(storage);

        dev_info(&intf->dev, "USB storage device disconnected\n");
    }


    Execution Flow:

        USB device sends data
              |
              v
        USB host controller receives bulk transfer
              |
              v
        Host controller raises interrupt
              |
              v
        USB core processes URB completion
              |
              v
        usb_bulk_callback() [INTERRUPT CONTEXT]
          - Check URB status
          - Save transfer length
          - queue_work(wq, &rx_work)
              |
              v
        kworker thread picks up work
              |
              v
        usb_rx_work_handler() [PROCESS CONTEXT]
          - mutex_lock()                         <-- SLEEPS
          - kmalloc(GFP_KERNEL)                  <-- SLEEPS
          - Copy data from URB buffer
          - process_scsi_response()
          - submit_bio_and_wait()                <-- SLEEPS (block I/O)
          - usb_submit_urb() for next transfer
          - mutex_unlock()
              |
              v
        Data written to block device
              |
              v
        Ready for next USB transfer


    Cleanup Order Explained:

        1. storage->connected = false
           Prevents work handlers from doing new I/O

        2. usb_kill_urb()
           Cancels in-flight USB transfer, prevents new callbacks

        3. cancel_work_sync() / cancel_delayed_work_sync()
           Waits for any running handler to complete
           Cancels any pending (not yet started) work

        4. destroy_workqueue()
           Flushes any remaining work, destroys worker threads

        5. Free URB, buffer, device reference, structure


================================================================================
================================================================================
==                                                                            ==
==                SECTION 7: DECISION GUIDE — WHICH TO CHOOSE?                ==
==                                                                            ==
================================================================================
================================================================================


7.1  DECISION FLOWCHART
--------------------------------------------------------------------------------

    Start: You have interrupt processing to defer
              |
              v
    Does your bottom half need to SLEEP?
    (mutex, GFP_KERNEL, I2C, SPI, file I/O, USB)
              |
        +-----+-----+
        |           |
       YES          NO
        |           |
        v           v
    Is it tied     Do you need maximum
    to a specific  performance (>100K
    IRQ line?      interrupts/sec)?
        |               |
    +---+---+       +---+---+
    |       |       |       |
   YES      NO     YES      NO
    |       |       |       |
    v       v       v       v
  Threaded  Work   Softirq  Tasklet
  IRQ       Queue  (if core (simple
  (modern   (most   kernel  driver
  drivers)  flexible subsys) defer)
                    |
                    v
              Are you writing
              a new driver?
                    |
               +----+----+
               |         |
              YES        NO
               |         |
               v         v
            Threaded   Tasklet
            IRQ        (legacy,
            (preferred) still OK)


7.2  QUICK DECISION TABLE
--------------------------------------------------------------------------------

    +-------------------------------------+----------------------------------+
    | Situation                           | Use This                         |
    +-------------------------------------+----------------------------------+
    | I2C/SPI device interrupt            | Threaded IRQ (IRQF_ONESHOT)      |
    | Simple GPIO event                   | Threaded IRQ or Tasklet          |
    | Network card (high throughput)      | Softirq via NAPI                 |
    | Block device completion             | Softirq (BLOCK_SOFTIRQ)          |
    | USB device data processing          | Work Queue                       |
    | Periodic device health check        | Delayed Work Queue               |
    | Complex processing after interrupt  | Work Queue                       |
    | Need mutex in bottom half           | Work Queue or Threaded IRQ       |
    | Need GFP_KERNEL allocation          | Work Queue or Threaded IRQ       |
    | Simple register processing          | Tasklet (or Threaded IRQ)        |
    | Timer callback processing           | Softirq (TIMER_SOFTIRQ)          |
    | RCU grace period handling           | Softirq (RCU_SOFTIRQ)           |
    | New driver (any situation)          | Threaded IRQ (preferred)         |
    +-------------------------------------+----------------------------------+


7.3  MIGRATION PATH FOR LEGACY CODE
--------------------------------------------------------------------------------

    If maintaining older driver code, here is the recommended migration:

    Old Pattern                    New Pattern
    -----------------------------------------------------------------------
    request_irq() + tasklet    --> devm_request_threaded_irq()
    request_irq() + work queue --> devm_request_threaded_irq()
                                   (if work is always IRQ-triggered)
    request_irq() + work queue --> devm_request_threaded_irq()
                                   + work queue for non-IRQ work

    The kernel community actively encourages migrating from tasklets
    to threaded IRQs. Tasklets are considered semi-deprecated for
    new code.


================================================================================
================================================================================
==                                                                            ==
==                  SECTION 8: LOCKING CONSIDERATIONS                         ==
==                                                                            ==
================================================================================
================================================================================


8.1  LOCKING RULES BY CONTEXT
--------------------------------------------------------------------------------

    +---------------------+---------------------------------------------------+
    | Lock Type           | Usable In                                         |
    +---------------------+---------------------------------------------------+
    | spin_lock()         | Process context (preemption disabled)             |
    | spin_lock_bh()      | Process context (softirqs + preemption disabled)  |
    | spin_lock_irq()     | Process context (hardirqs + softirqs disabled)    |
    | spin_lock_irqsave() | ANY context (saves and disables IRQ flags)        |
    | mutex_lock()        | Process context ONLY (can sleep)                  |
    | semaphore           | Process context ONLY (can sleep)                  |
    +---------------------+---------------------------------------------------+


8.2  WHICH LOCK TO USE BETWEEN CONTEXTS
--------------------------------------------------------------------------------

    Data shared between:                  Lock to use:
    -----------------------------------------------------------------------

    Hardirq  <-->  Hardirq               spin_lock_irqsave()
                                          (on same CPU or different CPUs)

    Hardirq  <-->  Softirq/Tasklet       spin_lock_irqsave() in softirq
                                          spin_lock() in hardirq (IRQs off)

    Hardirq  <-->  Process context        spin_lock_irqsave() in process
                                          spin_lock() in hardirq

    Softirq  <-->  Softirq               spin_lock_bh() from process ctx
    (same type)                           spin_lock() from softirq ctx

    Softirq  <-->  Process context        spin_lock_bh() in process context
                                          spin_lock() in softirq context

    Tasklet  <-->  Same tasklet           No lock needed (serialized)

    Tasklet  <-->  Different tasklet      spin_lock() (both in softirq)

    Tasklet  <-->  Process context        spin_lock_bh() in process context

    Work Q   <-->  Work Q (same wq)       mutex_lock() (both process ctx)

    Work Q   <-->  Hardirq                spin_lock_irqsave() in work
                                          spin_lock() in hardirq

    Threaded <-->  Hardirq (top half)     spin_lock_irqsave() in thread
    IRQ                                   spin_lock() in hardirq

    Threaded <-->  Threaded IRQ           mutex_lock() (both process ctx)
    IRQ            (different IRQ)


8.3  COMMON LOCKING PATTERN: TOP HALF + BOTTOM HALF
--------------------------------------------------------------------------------

    /* Shared data structure */
    struct my_device {
        spinlock_t lock;          /* Protects buffer/len */
        char buffer[256];
        int len;
        struct mutex process_lock; /* Protects processing state */
        int result;
    };


    /* TOP HALF (hardirq context) */

    irqreturn_t my_isr(int irq, void *dev_id)
    {
        struct my_device *dev = dev_id;
        unsigned long flags;

        /* Use spin_lock_irqsave even though IRQs are off here,
           because this data is also accessed from process context
           which uses spin_lock_irqsave */
        spin_lock_irqsave(&dev->lock, flags);

        dev->len = readl(dev->base + LEN_REG);
        memcpy_fromio(dev->buffer, dev->base + DATA_REG, dev->len);

        spin_unlock_irqrestore(&dev->lock, flags);

        /* Schedule bottom half */
        tasklet_schedule(&dev->tasklet);
        /* OR: queue_work(dev->wq, &dev->work); */
        /* OR: return IRQ_WAKE_THREAD; */

        return IRQ_HANDLED;
    }


    /* BOTTOM HALF — Tasklet version (softirq context) */

    void my_tasklet_handler(unsigned long data)
    {
        struct my_device *dev = (struct my_device *)data;
        char local_buf[256];
        int local_len;

        /* Copy shared data under spinlock */
        spin_lock(&dev->lock);       /* No _irqsave needed in softirq
                                        if only shared with hardirq
                                        that uses _irqsave */
        memcpy(local_buf, dev->buffer, dev->len);
        local_len = dev->len;
        spin_unlock(&dev->lock);

        /* Process local copy (no lock needed) */
        process_data(local_buf, local_len);
    }


    /* BOTTOM HALF — Work queue version (process context) */

    void my_work_handler(struct work_struct *work)
    {
        struct my_device *dev = container_of(work, struct my_device,
                                              my_work);
        char local_buf[256];
        int local_len;

        /* Copy shared data under spinlock (disable IRQs because
           this data is shared with hardirq context) */
        spin_lock_irq(&dev->lock);
        memcpy(local_buf, dev->buffer, dev->len);
        local_len = dev->len;
        spin_unlock_irq(&dev->lock);

        /* Now process under mutex (can sleep) */
        mutex_lock(&dev->process_lock);
        dev->result = process_complex_data(local_buf, local_len);
        mutex_unlock(&dev->process_lock);
    }


    /* BOTTOM HALF — Threaded IRQ version (process context) */

    irqreturn_t my_thread_fn(int irq, void *dev_id)
    {
        struct my_device *dev = dev_id;
        char local_buf[256];
        int local_len;

        /* Copy shared data under spinlock */
        spin_lock_irq(&dev->lock);
        memcpy(local_buf, dev->buffer, dev->len);
        local_len = dev->len;
        spin_unlock_irq(&dev->lock);

        /* Process under mutex (can sleep) */
        mutex_lock(&dev->process_lock);
        dev->result = process_complex_data(local_buf, local_len);
        mutex_unlock(&dev->process_lock);

        return IRQ_HANDLED;
    }


================================================================================
================================================================================
==                                                                            ==
==             SECTION 9: PERFORMANCE AND LATENCY CHARACTERISTICS             ==
==                                                                            ==
================================================================================
================================================================================


9.1  LATENCY COMPARISON
--------------------------------------------------------------------------------

    Mechanism        Typical Latency     Overhead Source
    -----------------------------------------------------------------------
    Softirq          < 1 microsecond     Runs inline in irq_exit()
                                          No context switch
                                          No scheduler involvement

    Tasklet          1-5 microseconds    Runs inline in irq_exit() via
                                          softirq, slight overhead from
                                          STATE_RUN checking on SMP

    Threaded IRQ     5-50 microseconds   Requires wake_up_process()
                                          Context switch to IRQ thread
                                          Scheduler must run

    Work Queue       10-100 microseconds Requires wake_up_process()
                                          Context switch to kworker
                                          Scheduler must run
                                          May wait for other work items


9.2  THROUGHPUT COMPARISON
--------------------------------------------------------------------------------

    Mechanism        Relative Throughput  Notes
    -----------------------------------------------------------------------
    Softirq          Highest              Per-CPU parallel execution
                                          No serialization overhead
                                          Runs with IRQs enabled

    Tasklet          High                 Same-tasklet serialization
                                          limits SMP scaling

    Threaded IRQ     Medium               Thread scheduling overhead
                                          One thread per IRQ action

    Work Queue       Medium-Low           Thread scheduling overhead
                                          Shared worker pool
                                          May wait for other work


9.3  WHEN LATENCY MATTERS — REAL NUMBERS
--------------------------------------------------------------------------------

    Network card at 10 Gbps:
        - Packets arrive every ~67 nanoseconds (minimum size)
        - MUST use softirq (NAPI) — anything else cannot keep up
        - Threaded IRQ would add 5-50 us per interrupt = disaster

    I2C sensor at 400 kHz:
        - I2C transaction takes 100-500 microseconds
        - Threaded IRQ adds 5-50 us overhead = negligible
        - The I2C bus speed is the bottleneck, not the mechanism

    GPIO button press:
        - Human reaction time: ~100 milliseconds
        - Any mechanism works fine
        - Tasklet or threaded IRQ both perfectly acceptable

    Audio codec at 48 kHz:
        - Sample period: ~20.8 microseconds
        - Threaded IRQ with RT priority works well
        - Softirq would be faster but cannot sleep for I2S/DMA setup


================================================================================
================================================================================
==                                                                            ==
==                  SECTION 10: DEBUGGING AND MONITORING                      ==
==                                                                            ==
================================================================================
================================================================================


10.1  VIEWING INTERRUPT STATISTICS
--------------------------------------------------------------------------------

    /proc/interrupts:
        Shows per-CPU interrupt counts and handler names.

        $ cat /proc/interrupts
                   CPU0       CPU1       CPU2       CPU3
         16:      45032      12003      23001      34002  IO-APIC  16-fasteoi  my_nic
         47:        102          0          0          0  IO-APIC  47-level    temp_alert
        IPI0:     12345      12344      12346      12343  Rescheduling interrupts


10.2  VIEWING SOFTIRQ STATISTICS
--------------------------------------------------------------------------------

    /proc/softirqs:
        Shows per-CPU softirq execution counts by type.

        $ cat /proc/softirqs
                        CPU0       CPU1       CPU2       CPU3
              HI:          2          0          0          0
           TIMER:     234567     234568     234566     234569
          NET_TX:       1234       1235       1233       1236
          NET_RX:     456789     456790     456788     456791
           BLOCK:      12345      12346      12344      12347
        IRQ_POLL:          0          0          0          0
         TASKLET:       5678       5679       5677       5680
           SCHED:      34567      34568      34566      34569
         HRTIMER:          0          0          0          0
             RCU:      78901      78902      78900      78903


10.3  VIEWING WORKQUEUE ACTIVITY
--------------------------------------------------------------------------------

    Worker threads visible in ps:

        $ ps aux | grep kworker
        root    12  0.0  0.0  0  0  ?  S  kworker/0:0
        root    13  0.0  0.0  0  0  ?  S  kworker/0:1
        root    14  0.0  0.0  0  0  ?  S  kworker/1:0
        root    28  0.0  0.0  0  0  ?  S  kworker/u8:0     (unbound)
        root    29  0.0  0.0  0  0  ?  S  kworker/0:0H     (high-pri)


10.4  VIEWING THREADED IRQ THREADS
--------------------------------------------------------------------------------

    IRQ threads visible in ps:

        $ ps aux | grep irq/
        root    89  0.0  0.0  0  0  ?  S  irq/47-temp_alert
        root    90  0.0  0.0  0  0  ?  S  irq/16-my_nic
        root    91  0.0  0.0  0  0  ?  S  irq/23-i2c_adapter

    IRQ thread priority:

        $ chrt -p $(pgrep -f "irq/47")
        pid 89's current scheduling policy: SCHED_FIFO
        pid 89's current scheduling priority: 50

    IRQ affinity:

        $ cat /proc/irq/47/smp_affinity
        f                               (all 4 CPUs)

        $ echo 1 > /proc/irq/47/smp_affinity
        (pin to CPU 0 only)


10.5  FTRACE TRACEPOINTS
--------------------------------------------------------------------------------

    Available tracepoints for interrupt debugging:

        irq:irq_handler_entry        — Hardirq handler entered
        irq:irq_handler_exit         — Hardirq handler exited
        irq:softirq_entry            — Softirq handler entered
        irq:softirq_exit             — Softirq handler exited
        irq:softirq_raise            — Softirq was raised
        workqueue:workqueue_queue_work    — Work item queued
        workqueue:workqueue_activate_work — Work item activated
        workqueue:workqueue_execute_start — Work handler started
        workqueue:workqueue_execute_end   — Work handler finished

    Example: Trace all softirq activity:

        $ echo 1 > /sys/kernel/debug/tracing/events/irq/softirq_entry/enable
        $ echo 1 > /sys/kernel/debug/tracing/events/irq/softirq_exit/enable
        $ cat /sys/kernel/debug/tracing/trace

    Example: Trace workqueue activity:

        $ echo 1 > /sys/kernel/debug/tracing/events/workqueue/enable
        $ cat /sys/kernel/debug/tracing/trace


10.6  COMMON DEBUG TECHNIQUES
--------------------------------------------------------------------------------

    1. Check if softirqs are taking too long:
       $ watch -n 1 cat /proc/softirqs
       (If counts increase very rapidly, softirqs may be starving
        other processing — check ksoftirqd CPU usage)

    2. Check for IRQ storms:
       $ watch -n 1 cat /proc/interrupts
       (If a specific IRQ count increases thousands per second
        when device is idle, suspect missing IRQF_ONESHOT or
        failure to acknowledge interrupt in hardware)

    3. Check for stuck work items:
       $ echo 1 > /sys/kernel/debug/tracing/events/workqueue/enable
       Look for workqueue_execute_start without matching _end

    4. Check for high kworker CPU usage:
       $ top -p $(pgrep -d',' kworker)
       (High CPU on kworker suggests a work handler is doing
        too much work or is being rescheduled too frequently)


================================================================================
================================================================================
==                                                                            ==
==                        SECTION 11: SUMMARY                                 ==
==                                                                            ==
================================================================================
================================================================================


    SOFTIRQS:
        - Core kernel only, statically defined, highest performance
        - Same type runs concurrently on multiple CPUs
        - Cannot sleep, needs careful per-CPU locking
        - Used by: networking (NAPI), block I/O, timers, RCU, scheduler

    TASKLETS:
        - Built on softirqs, dynamically created, simpler API
        - Same tasklet serialized across CPUs (never concurrent)
        - Cannot sleep, suitable for simple driver bottom halves
        - Semi-deprecated — prefer threaded IRQs for new code

    WORK QUEUES:
        - Process context, CAN SLEEP, most flexible mechanism
        - Supports delayed execution, custom concurrency, priorities
        - Higher latency due to context switch to kworker thread
        - Used for: complex processing, I/O, anything needing sleep

    THREADED IRQs:
        - Modern preferred approach for device drivers
        - Dedicated kernel thread per IRQ action, CAN SLEEP
        - Clean API combining top half and bottom half registration
        - IRQF_ONESHOT prevents interrupt storms
        - Compatible with PREEMPT_RT real-time kernels
        - Used for: I2C/SPI devices, modern drivers, GPIO handlers


    Golden Rule:
        If writing a NEW device driver, use threaded IRQs
        (devm_request_threaded_irq) unless you have a specific
        reason not to. Use work queues for deferred work that is
        not directly tied to a specific interrupt. Use softirqs
        only if you are a core kernel subsystem developer.


================================================================================
END OF DOCUMENT
================================================================================
