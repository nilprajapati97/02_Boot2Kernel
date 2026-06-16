================================================================================
            WORK QUEUES — LINUX KERNEL BOTTOM HALF MECHANISM
================================================================================


OVERVIEW
--------------------------------------------------------------------------------
Work queues are a bottom half mechanism in the Linux kernel that execute
deferred work in process context using special kernel threads called
"workers." Their most significant advantage over softirqs and tasklets is
that work queue handlers CAN SLEEP, allowing them to allocate memory,
acquire mutexes, perform I/O, and use the full range of kernel APIs.


================================================================================
1. WHAT ARE WORK QUEUES?
================================================================================

Key Properties:
    - Run in process context (as kernel threads)
    - CAN SLEEP — this is the single biggest advantage
    - Can allocate memory, perform I/O, take mutexes
    - Managed by special kernel threads called "workers" (kworker/*)
    - Support delayed execution via timers
    - Support custom queues with tunable concurrency and priority
    - Work items are deduplicated (same item is not queued twice)


================================================================================
2. ARCHITECTURE
================================================================================

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
    |                             |  +---------------------+ |    |
    |                             |  |  Worker Thread       | |    |
    |                             |  |  (kworker/0:3)       | |    |
    |                             |  +---------------------+ |    |
    |                             +-------------------------+    |
    +-----------------------------------------------------------+

    Workqueue (logical):
        A named queue that drivers submit work items to.
        Does not own threads directly.

    Worker Pool (physical):
        A pool of kernel threads (kworker/*) that actually execute
        work items. Pools are shared across workqueues. Each CPU has
        its own pool for bound workqueues. Unbound workqueues share
        separate pools.

    Worker Thread:
        A kernel thread (visible as kworker/CPU:ID in ps output)
        that loops waiting for work, dequeues items, and calls
        their handler functions. The pool dynamically creates and
        destroys worker threads as needed.

    Relationship:
        Multiple workqueues ----> One worker pool ----> Multiple workers
        (Many-to-one-to-many)


================================================================================
3. INTERNAL STRUCTURES
================================================================================

3.1  Work Item Structure
--------------------------------------------------------------------------------

    struct work_struct {
        atomic_long_t data;               /* Internal flags and pool info  */
        struct list_head entry;           /* Linked list in worklist       */
        work_func_t func;                /* Handler function              */
    };

    +--------+------------------------------------------------------------+
    | Field  | Description                                                |
    +--------+------------------------------------------------------------+
    | data   | Stores WORK_STRUCT_PENDING flag, pointer to owning pool,   |
    |        | and color information for flush operations                 |
    | entry  | Links this work item into the worker pool's worklist       |
    | func   | Pointer to the callback function (bottom half handler)     |
    +--------+------------------------------------------------------------+


3.2  Delayed Work Structure
--------------------------------------------------------------------------------

    struct delayed_work {
        struct work_struct work;          /* Embedded work item            */
        struct timer_list timer;          /* Kernel timer for the delay    */
        struct workqueue_struct *wq;      /* Target workqueue              */
        int cpu;                          /* Target CPU                    */
    };

    Delayed work uses a kernel timer internally. When the timer fires,
    it queues the embedded work_struct onto the target workqueue. The
    handler function then executes just like a regular work item.


================================================================================
4. TYPES OF WORK QUEUES
================================================================================

    +------------------------+----------------------------------------------+
    | Type                   | Description                                  |
    +------------------------+----------------------------------------------+
    | system_wq              | Default general-purpose workqueue.           |
    |                        | Used by schedule_work(). Bound to CPUs.      |
    |                        | Do not use for long-running work.            |
    +------------------------+----------------------------------------------+
    | system_highpri_wq      | High-priority workers. Work items here       |
    |                        | are serviced before normal-priority items.   |
    +------------------------+----------------------------------------------+
    | system_long_wq         | For work items that are expected to run      |
    |                        | for a long time. Prevents blocking other     |
    |                        | work on system_wq.                           |
    +------------------------+----------------------------------------------+
    | system_unbound_wq      | Workers not bound to any specific CPU.       |
    |                        | Scheduler can place them on any CPU.         |
    |                        | Good for CPU-intensive work.                 |
    +------------------------+----------------------------------------------+
    | system_freezable_wq    | Participates in system suspend/freeze.       |
    |                        | Work is paused during suspend.               |
    +------------------------+----------------------------------------------+
    | Custom workqueue       | Created by the driver using                  |
    |                        | alloc_workqueue() with specific flags        |
    |                        | and concurrency limits.                      |
    +------------------------+----------------------------------------------+


================================================================================
5. DETAILED CODE FLOW — COMPLETE DRIVER EXAMPLE
================================================================================

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


5.1  STEP 1 — Define Work Handler (Runs in Process Context)
--------------------------------------------------------------------------------

    static void my_work_handler(struct work_struct *work)
    {
        struct my_device *dev = container_of(work, struct my_device,
                                              my_work);
        char *kernel_buffer;

        /* ============================================= */
        /* WE ARE IN PROCESS CONTEXT — WE CAN SLEEP!    */
        /* ============================================= */

        /* Allocate memory (may sleep) */
        kernel_buffer = kmalloc(4096, GFP_KERNEL);        /* Can sleep */
        if (!kernel_buffer)
            return;

        /* Take mutex (may sleep) */
        mutex_lock(&dev->lock);                            /* Can sleep */

        /* Process data */
        memcpy(kernel_buffer, dev->data_buffer, dev->data_len);

        /* Do heavy processing */
        process_complex_data(kernel_buffer, dev->data_len);

        /* Write to file/storage (may sleep) */
        write_to_log_file(kernel_buffer, dev->data_len);  /* Can sleep */

        mutex_unlock(&dev->lock);
        kfree(kernel_buffer);

        printk(KERN_INFO "Work completed for device\n");
    }


5.2  STEP 2 — Define Delayed Work Handler
--------------------------------------------------------------------------------

    static void my_delayed_handler(struct work_struct *work)
    {
        struct delayed_work *dwork = to_delayed_work(work);
        struct my_device *dev = container_of(dwork, struct my_device,
                                              my_delayed_work);

        /* This runs after the specified delay */
        printk("Delayed work executing after timeout\n");

        /* Check device status */
        check_device_health(dev);

        /* Reschedule for periodic work (every 1 second) */
        schedule_delayed_work(&dev->my_delayed_work,
                              msecs_to_jiffies(1000));
    }

    Note: Calling schedule_delayed_work() from within the handler
    creates a self-rescheduling periodic task. Ensure you call
    cancel_delayed_work_sync() during cleanup to stop the cycle.


5.3  STEP 3 — Top Half ISR
--------------------------------------------------------------------------------

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
        memcpy_fromio(dev->data_buffer,
                       dev->base + DATA_REG,
                       dev->data_len);

        /* Option A: Schedule work on custom workqueue */
        queue_work(dev->my_wq, &dev->my_work);

        /* Option B: Schedule on default system workqueue */
        /* schedule_work(&dev->my_work); */

        /* Option C: Schedule delayed work (500ms delay) */
        /* schedule_delayed_work(&dev->my_delayed_work,
                                  msecs_to_jiffies(500)); */

        return IRQ_HANDLED;
    }


5.4  STEP 4 — Probe Function (Initialize Everything)
--------------------------------------------------------------------------------

    static int my_device_probe(struct platform_device *pdev)
    {
        struct my_device *dev;
        int ret;

        dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
        if (!dev)
            return -ENOMEM;

        dev->data_buffer = kmalloc(MAX_DATA_SIZE, GFP_KERNEL);
        if (!dev->data_buffer)
            return -ENOMEM;

        /* Initialize mutex */
        mutex_init(&dev->lock);

        /* Initialize regular work */
        INIT_WORK(&dev->my_work, my_work_handler);

        /* Initialize delayed work */
        INIT_DELAYED_WORK(&dev->my_delayed_work, my_delayed_handler);

        /* Create custom workqueue */
        dev->my_wq = alloc_workqueue("my_device_wq",
                                      WQ_UNBOUND | WQ_HIGHPRI,
                                      4);  /* max 4 active work items */
        if (!dev->my_wq) {
            ret = -ENOMEM;
            goto err_wq;
        }

        /* Request IRQ */
        ret = request_irq(dev->irq, my_device_isr,
                          IRQF_SHARED, "my_device", dev);
        if (ret)
            goto err_irq;

        platform_set_drvdata(pdev, dev);
        return 0;

    err_irq:
        destroy_workqueue(dev->my_wq);
    err_wq:
        kfree(dev->data_buffer);
        return ret;
    }


5.5  STEP 5 — Remove Function (Cleanup)
--------------------------------------------------------------------------------

    static int my_device_remove(struct platform_device *pdev)
    {
        struct my_device *dev = platform_get_drvdata(pdev);

        /* Free the IRQ first (no more top half scheduling) */
        free_irq(dev->irq, dev);

        /* Cancel any pending work and wait for completion */
        cancel_work_sync(&dev->my_work);
        cancel_delayed_work_sync(&dev->my_delayed_work);

        /* Destroy custom workqueue (flushes remaining work first) */
        destroy_workqueue(dev->my_wq);

        /* Free data buffer */
        kfree(dev->data_buffer);

        return 0;
    }

    Cleanup order matters:
        1. free_irq()                    — Stop new work from being queued
        2. cancel_work_sync()            — Cancel and wait for pending work
        3. cancel_delayed_work_sync()    — Cancel and wait for delayed work
        4. destroy_workqueue()           — Destroy the custom queue
        5. Free remaining resources


================================================================================
6. INTERNAL KERNEL EXECUTION FLOW
================================================================================

    Hardware Interrupt
            |
            v
    my_device_isr()                  [hardirq context]
            |
            |  queue_work(my_wq, &my_work)
            |         |
            |         v
            |  +-------------------------------------------+
            |  | 1. Test WORK_STRUCT_PENDING bit            |
            |  |    - If already set, return false          |
            |  |      (work already queued, no duplicate)   |
            |  |                                            |
            |  | 2. Set WORK_STRUCT_PENDING bit             |
            |  |                                            |
            |  | 3. Select appropriate worker pool:         |
            |  |    - Per-CPU pool (for bound workqueues)   |
            |  |    - Unbound pool (for WQ_UNBOUND)         |
            |  |                                            |
            |  | 4. Add work_struct to pool's worklist      |
            |  |    (linked list, appended at tail)         |
            |  |                                            |
            |  | 5. Wake up an idle worker thread           |
            |  |    OR create a new worker if:              |
            |  |    - All existing workers are busy         |
            |  |    - Pool hasn't hit max concurrency       |
            |  +-------------------------------------------+
            |
            v
    Return from ISR
            |
            |
            v
    Meanwhile, on a worker thread (kworker/0:1):
            |
            v
    +----------------------------------------------+
    | worker_thread() main loop:                    |
    |                                               |
    | 1. Sleep waiting for work (idle state)        |
    |                                               |
    | 2. Wake up (signaled by queue_work)           |
    |                                               |
    | 3. Lock the pool                              |
    |                                               |
    | 4. Dequeue work item from worklist            |
    |                                               |
    | 5. Clear WORK_STRUCT_PENDING flag             |
    |    (allows same work to be re-queued now)     |
    |                                               |
    | 6. Unlock the pool                            |
    |                                               |
    | 7. Call work->func(work)                      |
    |    +----------------------------------------+ |
    |    | my_work_handler() executes             | |
    |    | IN FULL PROCESS CONTEXT                | |
    |    | - Can sleep                            | |
    |    | - Can allocate memory (GFP_KERNEL)     | |
    |    | - Can take mutexes                     | |
    |    | - Can do blocking I/O                  | |
    |    | - Has access to current->mm (if set)   | |
    |    +----------------------------------------+ |
    |                                               |
    | 8. Check for more work in worklist            |
    |    - If more work: go to step 4              |
    |    - If empty: go to step 1 (sleep)          |
    +----------------------------------------------+


================================================================================
7. DELAYED WORK EXECUTION FLOW
================================================================================

    schedule_delayed_work(&dev->my_delayed_work, msecs_to_jiffies(500))
            |
            v
    +----------------------------------------------+
    | 1. Initialize internal timer with delay       |
    | 2. Timer callback = delayed_work_timer_fn     |
    | 3. Register timer with kernel timer subsystem |
    +----------------------------------------------+
            |
            |  ... 500 milliseconds pass ...
            |
            v
    Timer interrupt fires
            |
            v
    delayed_work_timer_fn()          [softirq context]
            |
            v
    +----------------------------------------------+
    | 1. Extract the embedded work_struct           |
    | 2. Call queue_work() to add it to the         |
    |    target workqueue                           |
    | 3. From here, same flow as regular work       |
    +----------------------------------------------+
            |
            v
    Worker thread picks up and executes
    my_delayed_handler()             [process context]


================================================================================
8. WORKQUEUE FLAGS EXPLAINED
================================================================================

8.1  WQ_UNBOUND
--------------------------------------------------------------------------------

    struct workqueue_struct *wq = alloc_workqueue("unbound_wq",
                                                   WQ_UNBOUND, 0);

    - Workers are NOT bound to a specific CPU
    - Scheduler can migrate workers to any CPU
    - Good for CPU-intensive work that benefits from load balancing
    - Avoids per-CPU affinity overhead
    - Uses a shared unbound worker pool


8.2  WQ_HIGHPRI
--------------------------------------------------------------------------------

    struct workqueue_struct *wq = alloc_workqueue("highpri_wq",
                                                   WQ_HIGHPRI, 0);

    - Workers run with higher scheduling priority
    - Uses a separate high-priority worker pool (per-CPU)
    - Work items are serviced before normal-priority items
    - Good for time-sensitive deferred work


8.3  WQ_MEM_RECLAIM
--------------------------------------------------------------------------------

    struct workqueue_struct *wq = alloc_workqueue("memreclaim_wq",
                                                   WQ_MEM_RECLAIM, 0);

    - Guarantees forward progress even under memory pressure
    - Allocates a "rescuer" thread at creation time
    - If regular workers cannot be created due to low memory,
      the rescuer thread executes pending work items
    - REQUIRED if your work handler is involved in the memory
      reclaim path (e.g., filesystem writeback, block I/O)


8.4  WQ_FREEZABLE
--------------------------------------------------------------------------------

    struct workqueue_struct *wq = alloc_workqueue("freezable_wq",
                                                   WQ_FREEZABLE, 0);

    - Workers participate in system suspend/hibernate
    - Work is paused during freeze and resumed after thaw
    - Important for work that should not run while the system
      is transitioning to a sleep state


8.5  WQ_POWER_EFFICIENT
--------------------------------------------------------------------------------

    struct workqueue_struct *wq = alloc_workqueue("power_wq",
                                                   WQ_POWER_EFFICIENT, 0);

    - On systems with workqueue.power_efficient=true kernel parameter,
      this flag makes the workqueue behave as WQ_UNBOUND
    - Allows the scheduler to batch work and let CPUs stay idle longer
    - Saves power on battery-powered devices


8.6  max_active Parameter
--------------------------------------------------------------------------------

    struct workqueue_struct *wq = alloc_workqueue("limited_wq",
                                                   WQ_UNBOUND, 4);
                                                              ^
                                                              |
                                            max 4 active work items

    - Limits how many work items from this workqueue can execute
      concurrently on any given CPU (for bound) or overall (for unbound)
    - 0 means use the default (currently 256 for bound, 512 for unbound)
    - Set to 1 for strict serialization of all work items


8.7  Common Flag Combinations
--------------------------------------------------------------------------------

    /* Filesystem writeback workqueue */
    alloc_workqueue("writeback_wq",
                    WQ_MEM_RECLAIM | WQ_FREEZABLE | WQ_UNBOUND, 0);

    /* Real-time driver workqueue */
    alloc_workqueue("rt_driver_wq",
                    WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);

    /* Power-efficient periodic monitoring */
    alloc_workqueue("monitor_wq",
                    WQ_POWER_EFFICIENT | WQ_FREEZABLE, 0);


================================================================================
9. WORK QUEUE API REFERENCE
================================================================================

9.1  Initialization
--------------------------------------------------------------------------------

    +--------------------------------------------+------------------------------+
    | Function / Macro                           | Description                  |
    +--------------------------------------------+------------------------------+
    | INIT_WORK(&work, handler)                  | Initialize a work_struct     |
    | INIT_DELAYED_WORK(&dwork, handler)         | Initialize a delayed_work    |
    | DECLARE_WORK(name, handler)                | Static work declaration      |
    | DECLARE_DELAYED_WORK(name, handler)        | Static delayed_work decl.    |
    +--------------------------------------------+------------------------------+


9.2  Scheduling
--------------------------------------------------------------------------------

    +--------------------------------------------+------------------------------+
    | Function                                   | Description                  |
    +--------------------------------------------+------------------------------+
    | schedule_work(&work)                       | Queue on system_wq           |
    | schedule_delayed_work(&dwork, delay)       | Queue on system_wq after     |
    |                                            | delay jiffies                |
    | queue_work(wq, &work)                      | Queue on specific workqueue  |
    | queue_delayed_work(wq, &dwork, delay)      | Queue delayed on specific wq |
    | queue_work_on(cpu, wq, &work)              | Queue on specific CPU        |
    | mod_delayed_work(wq, &dwork, delay)        | Modify delay of pending work |
    +--------------------------------------------+------------------------------+

    Return value: true if work was successfully queued, false if it was
    already pending (WORK_STRUCT_PENDING was set).


9.3  Cancellation and Synchronization
--------------------------------------------------------------------------------

    +--------------------------------------------+------------------------------+
    | Function                                   | Description                  |
    +--------------------------------------------+------------------------------+
    | cancel_work(&work)                         | Cancel if pending (no wait)  |
    | cancel_work_sync(&work)                    | Cancel and wait for finish   |
    | cancel_delayed_work(&dwork)                | Cancel delayed (no wait)     |
    | cancel_delayed_work_sync(&dwork)           | Cancel delayed and wait      |
    | flush_work(&work)                          | Wait for specific work to    |
    |                                            | finish executing             |
    | flush_delayed_work(&dwork)                 | Force delayed work to run    |
    |                                            | immediately and wait         |
    | flush_workqueue(wq)                        | Wait for ALL work on wq      |
    |                                            | to finish                    |
    +--------------------------------------------+------------------------------+

    Important:
        cancel_work_sync() and cancel_delayed_work_sync() are safe to
        call even if the work was never queued. They block until any
        currently executing instance of the handler completes. Always
        prefer the _sync variants in cleanup paths.


9.4  Workqueue Creation and Destruction
--------------------------------------------------------------------------------

    +--------------------------------------------+------------------------------+
    | Function                                   | Description                  |
    +--------------------------------------------+------------------------------+
    | alloc_workqueue(name, flags, max_active)   | Create a new workqueue       |
    | alloc_ordered_workqueue(name, flags)       | Create strictly ordered wq   |
    |                                            | (max_active = 1)             |
    | destroy_workqueue(wq)                      | Flush and destroy workqueue  |
    +--------------------------------------------+------------------------------+


================================================================================
10. WHEN TO USE WORK QUEUES
================================================================================

    USE WORK QUEUES WHEN:
    -----------------------------------------------------------------------
    [+] Bottom half handler needs to SLEEP
    [+] Need to allocate memory with GFP_KERNEL
    [+] Need to acquire mutexes or semaphores
    [+] Need to perform blocking I/O operations
    [+] Need to copy data to/from userspace
    [+] Long-running or complex processing is required
    [+] Need to use APIs that may sleep internally
    [+] Need delayed or periodic execution of deferred work
    [+] Need custom concurrency control (max_active)

    DO NOT USE WORK QUEUES WHEN:
    -----------------------------------------------------------------------
    [-] Ultra-low latency is required
        --> Use softirqs or tasklets instead

    [-] Very simple, fast processing that never needs to sleep
        --> Tasklets have less overhead (no context switch)

    [-] Work must execute immediately after the interrupt
        --> Softirqs run right after hardirq, before any scheduling

    [-] Handler would never sleep anyway and performance is critical
        --> The overhead of waking a kernel thread and context
            switching is unnecessary


================================================================================
11. WORK QUEUES VS OTHER BOTTOM HALF MECHANISMS
================================================================================

    +------------------+------------+----------+-------------+----------------+
    | Feature          | Work Queue | Tasklet  | Softirq     | Threaded IRQ   |
    +------------------+------------+----------+-------------+----------------+
    | Context          | Process    | Softirq  | Softirq     | Process        |
    | Can sleep        | Yes        | No       | No          | Yes            |
    | Scheduling       | kworker    | ksoftirq | Inline/     | Dedicated      |
    |                  | threads    |          | ksoftirqd   | IRQ thread     |
    | Concurrency      | Tunable    | Same:No  | Same:Yes    | Serialized     |
    | control          | max_active | Diff:Yes | Diff:Yes    | per IRQ line   |
    | Latency          | Higher     | Low      | Lowest      | Medium         |
    | Delayed exec     | Yes        | No       | No          | No             |
    | Dynamic creation | Yes        | Yes      | No          | Yes            |
    | Complexity       | Medium     | Low      | High        | Low            |
    | Typical use      | FS, block, | Simple   | Networking, | Modern device  |
    |                  | complex    | drivers  | block I/O,  | drivers        |
    |                  | drivers    |          | timers      |                |
    +------------------+------------+----------+-------------+----------------+


================================================================================
12. COMMON PITFALLS AND BEST PRACTICES
================================================================================

    PITFALL 1:  Using system_wq for long-running work
    -----------------------------------------------------------------------
    The default system_wq has limited concurrency. If your handler runs
    for a long time, it blocks other work items. Use system_long_wq or
    create a custom workqueue instead.

    PITFALL 2:  Forgetting cancel_work_sync() on module removal
    -----------------------------------------------------------------------
    If the module is unloaded while work is pending or executing, the
    kernel calls into freed memory. Always cancel all work and destroy
    custom workqueues in your cleanup path.

    PITFALL 3:  Re-queuing work without checking return value
    -----------------------------------------------------------------------
    queue_work() returns false if the work is already pending. If you
    need to ensure new data is processed, use a separate flag or counter
    in your device structure rather than relying on re-queuing.

    PITFALL 4:  Cleanup order errors
    -----------------------------------------------------------------------
    Always free the IRQ before canceling work. Otherwise, the ISR can
    re-queue work after you cancel it:

        WRONG:                          CORRECT:
        cancel_work_sync(&work);        free_irq(irq, dev);
        free_irq(irq, dev);            cancel_work_sync(&work);
        destroy_workqueue(wq);          destroy_workqueue(wq);

    PITFALL 5:  Deadlock between flush and work handler
    -----------------------------------------------------------------------
    If a work handler calls flush_workqueue() on its own workqueue,
    it deadlocks. The handler is waiting for all work to complete, but
    it is itself a work item that hasn't completed.

    PITFALL 6:  Sharing data without locking
    -----------------------------------------------------------------------
    Work handlers run in process context on potentially different CPUs
    than the top half ISR. Data shared between the ISR and the handler
    needs proper synchronization:
        - spin_lock_irqsave() for data shared with hardirq context
        - mutex for data shared only between work handlers

    BEST PRACTICE:  Use container_of() to access device context
    -----------------------------------------------------------------------
    Embed work_struct inside your device structure and use container_of()
    to retrieve the device pointer. This is cleaner and safer than
    casting from a global variable.

        struct my_device *dev = container_of(work, struct my_device,
                                              my_work);

    BEST PRACTICE:  Use WQ_MEM_RECLAIM when appropriate
    -----------------------------------------------------------------------
    If your workqueue is used in any memory reclaim path (filesystem
    writeback, block device I/O, swap), always set WQ_MEM_RECLAIM.
    Without it, the system can deadlock under memory pressure when
    no worker threads can be created.

    BEST PRACTICE:  Prefer system workqueues for simple cases
    -----------------------------------------------------------------------
    Do not create a custom workqueue unless you need specific flags
    or concurrency control. schedule_work() using system_wq is
    sufficient for most simple driver use cases.


================================================================================
13. COMPLETE EXECUTION TIMELINE
================================================================================

    TIME   EVENT                                      CONTEXT
    -----  -----------------------------------------  -----------------------
    T0     Hardware raises interrupt line              Hardware
    T1     CPU enters hardirq handler                  Hardirq (IRQs off)
    T2     my_device_isr() executes                    Hardirq
    T3       -> Reads status, acks hardware            Hardirq
    T4       -> Copies data from device registers      Hardirq
    T5       -> queue_work(my_wq, &my_work)            Hardirq
    T6         -> Sets PENDING, adds to worklist       Hardirq
    T7         -> Wakes idle kworker thread            Hardirq
    T8       -> Returns IRQ_HANDLED                    Hardirq
    T9     Return from interrupt                       Transition
    T10    ...scheduler runs...                        Scheduler
    T11    kworker/0:1 thread is scheduled             Process context
    T12    worker_thread() dequeues my_work            Process context
    T13    Clears PENDING flag                         Process context
    T14    Calls my_work_handler()                     Process context
    T15      -> kmalloc(GFP_KERNEL) — may sleep        Process context
    T16      -> mutex_lock() — may sleep               Process context
    T17      -> Processes data, does I/O               Process context
    T18      -> mutex_unlock()                         Process context
    T19      -> kfree()                                Process context
    T20    my_work_handler() returns                   Process context
    T21    worker_thread() checks for more work        Process context
    T22    No more work, worker goes to sleep          Process context

    Note: The gap between T9 and T11 depends on scheduler decisions.
    The work handler does NOT run immediately after the ISR returns.
    There is a context switch involved, which adds latency compared
    to softirqs and tasklets.


================================================================================
14. ADVANCED TOPIC — CONCURRENCY MANAGED WORKQUEUE (CMWQ)
================================================================================

    Since kernel 2.6.36, the workqueue subsystem uses Concurrency Managed
    Workqueues (CMWQ). Key design points:

    1. Worker pools are SHARED across workqueues
       - Reduces total number of kernel threads
       - Multiple workqueues can share the same pool of workers

    2. Automatic concurrency management
       - The pool monitors how many workers are running vs sleeping
       - If all workers are blocked (sleeping), pool creates a new one
       - Idle workers are eventually destroyed to save resources

    3. Per-CPU pools for bound workqueues
       - Each CPU has two pools: normal priority and high priority
       - Bound work items execute on the CPU they were queued on
       - Maintains cache locality

    4. Unbound pools for WQ_UNBOUND workqueues
       - Shared across CPUs (one per NUMA node typically)
       - Scheduler handles CPU placement
       - Better for CPU-intensive work and power efficiency

    Pool structure (simplified):

        Per-CPU 0:  [Normal Pool] --> kworker/0:0, kworker/0:1, ...
                    [HighPri Pool] --> kworker/0:0H, kworker/0:1H, ...

        Per-CPU 1:  [Normal Pool] --> kworker/1:0, kworker/1:1, ...
                    [HighPri Pool] --> kworker/1:0H, kworker/1:1H, ...

        Unbound:    [Unbound Pool] --> kworker/u4:0, kworker/u4:1, ...

    Worker naming convention in ps output:
        kworker/CPU:ID      — Bound worker on specific CPU
        kworker/CPU:IDH     — High-priority bound worker
        kworker/uPOOL:ID    — Unbound worker


================================================================================
                            END OF DOCUMENT
================================================================================
