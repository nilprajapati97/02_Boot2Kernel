================================================================================
          THREADED IRQs — LINUX KERNEL INTERRUPT HANDLING MECHANISM
================================================================================


OVERVIEW
--------------------------------------------------------------------------------
Threaded IRQs are the modern preferred approach for interrupt handling in the
Linux kernel. They provide a clean API that combines top half and bottom half
processing, where the bottom half runs as a dedicated kernel thread. Drivers
use request_threaded_irq() instead of the traditional request_irq() to
register both a hardirq handler and a threaded handler in a single call.


================================================================================
1. WHAT ARE THREADED IRQs?
================================================================================

Key Properties:
    - Modern and preferred approach for device driver interrupt handling
    - Combines top half and bottom half registration in one clean API
    - Bottom half runs as a dedicated kernel thread (irq/XX-name)
    - Thread runs in full process context — CAN SLEEP
    - Supports IRQF_ONESHOT to prevent interrupt storms
    - Compatible with PREEMPT_RT real-time kernels
    - Top half can be omitted entirely (NULL handler)
    - Automatic thread lifecycle management by the kernel
    - Each IRQ action gets its own dedicated thread


================================================================================
2. ARCHITECTURE
================================================================================

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
                  | Quick check:   |        | Full processing:   |
                  | - Verify IRQ   |        | - CAN SLEEP        |
                  |   is ours      |        | - I2C/SPI reads    |
                  | - Ack hardware |        | - Mutex locks      |
                  | - Return       |        | - Memory alloc     |
                  |   IRQ_WAKE_    |        | - File I/O         |
                  |   THREAD       |        | - Process context  |
                  +----------------+        +--------------------+

    Function Signature:

        int request_threaded_irq(
            unsigned int irq,              /* IRQ number                */
            irq_handler_t handler,         /* Top half (hardirq)       */
            irq_handler_t thread_fn,       /* Bottom half (thread)     */
            unsigned long irqflags,        /* IRQF_* flags             */
            const char *devname,           /* Name in /proc/interrupts */
            void *dev_id                   /* Passed to both handlers  */
        );

    Return Values from Top Half:
        IRQ_NONE         — Interrupt was not from this device
        IRQ_HANDLED      — Interrupt fully handled, do NOT wake thread
        IRQ_WAKE_THREAD  — Wake the threaded handler for processing

    Thread Naming:
        The kernel creates a thread named "irq/XX-devname" where XX is
        the IRQ number. Visible in ps output and /proc/interrupts.


================================================================================
3. DETAILED CODE FLOW — COMPLETE DRIVER EXAMPLE
================================================================================

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


3.1  STEP 1 — Top Half: Quick Hardware Check
--------------------------------------------------------------------------------

    static irqreturn_t sensor_hard_irq(int irq, void *dev_id)
    {
        struct sensor_device *sensor = dev_id;

        /*
         * HARDIRQ CONTEXT — Cannot sleep!
         * Do minimal work here.
         *
         * For memory-mapped devices: read status register
         * For I2C/SPI devices: we CANNOT read here (would sleep!)
         */

        /* Quick check: Is this interrupt from our device? */
        sensor->status = readl(sensor->base + STATUS_REG);
        if (!(sensor->status & SENSOR_IRQ_ACTIVE))
            return IRQ_NONE;          /* Not our interrupt */

        /* Acknowledge interrupt at hardware level */
        writel(SENSOR_IRQ_ACK, sensor->base + ACK_REG);

        return IRQ_WAKE_THREAD;       /* Wake the threaded handler */
    }

    Notes:
        - This function runs with interrupts disabled (hardirq context)
        - Must NOT call any function that may sleep
        - Should be as fast as possible
        - Returns IRQ_WAKE_THREAD to trigger the bottom half thread
        - Returns IRQ_NONE if the interrupt is not from this device
        - Returns IRQ_HANDLED if interrupt is fully handled here
          (thread will NOT be woken in this case)


3.2  STEP 2 — Bottom Half: Full Processing in Thread
--------------------------------------------------------------------------------

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
            sensor->client,            /* I2C client              */
            SENSOR_DATA_REG,           /* Starting register       */
            sizeof(data),              /* Number of bytes         */
            data                       /* Buffer                  */
        );

        if (ret < 0) {
            dev_err(&sensor->client->dev, "Failed to read sensor\n");
            mutex_unlock(&sensor->lock);
            return IRQ_HANDLED;
        }

        /* Process data */
        for (i = 0; i < 10; i++) {
            sensor->sensor_data[i] = (data[i*2] << 8) | data[i*2 + 1];
        }

        /* Log data (may involve file I/O — sleeps) */
        log_sensor_reading(sensor);

        /* Notify userspace via sysfs */
        sysfs_notify(&sensor->client->dev.kobj, NULL, "sensor_data");

        mutex_unlock(&sensor->lock);

        return IRQ_HANDLED;
    }

    Notes:
        - Runs in a dedicated kernel thread (irq/XX-sensor_irq)
        - Has full process context — can sleep, allocate memory, do I/O
        - Particularly important for I2C/SPI devices where bus access
          requires sleeping
        - If IRQF_ONESHOT is set, the IRQ line is unmasked AFTER this
          function returns


3.3  STEP 3 — Probe: Register Threaded IRQ
--------------------------------------------------------------------------------

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

        /* ─── Option A: Both top half and bottom half ─── */

        ret = request_threaded_irq(
            sensor->irq,                  /* IRQ number              */
            sensor_hard_irq,              /* Top half handler        */
            sensor_thread_fn,             /* Bottom half thread fn   */
            IRQF_TRIGGER_FALLING |        /* Trigger on falling edge */
            IRQF_ONESHOT,                 /* Keep IRQ masked until   */
                                          /* thread completes        */
            "sensor_irq",                 /* Name in /proc/interrupts*/
            sensor                        /* dev_id for both handlers*/
        );

        if (ret) {
            dev_err(&client->dev,
                    "Failed to request threaded IRQ: %d\n", ret);
            return ret;
        }


        /* ─── Option B: Thread-only (no hardirq handler) ─── */

        /*
         * Pass NULL as the top half handler.
         * The kernel provides a default top half that simply
         * returns IRQ_WAKE_THREAD.
         *
         * Use this when:
         * - You cannot check hardware in hardirq context
         *   (e.g., I2C/SPI devices)
         * - All processing must happen in the thread
         */

        ret = request_threaded_irq(
            sensor->irq,
            NULL,                         /* NULL = default handler  */
            sensor_thread_fn,             /* Thread handles everything*/
            IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
            "sensor_irq",
            sensor
        );


        /* ─── Option C: devm version (auto-cleanup) ─── */

        /*
         * devm_request_threaded_irq() automatically calls free_irq()
         * when the device is removed or the driver is unloaded.
         * No need to manually free in remove function.
         */

        ret = devm_request_threaded_irq(
            &client->dev,                 /* Device for devm mgmt   */
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


3.4  STEP 4 — Remove: Cleanup
--------------------------------------------------------------------------------

    static void sensor_remove(struct i2c_client *client)
    {
        struct sensor_device *sensor = i2c_get_clientdata(client);

        /* If NOT using devm version, manually free the IRQ */
        free_irq(sensor->irq, sensor);

        /*
         * free_irq() does the following:
         * 1. Waits for any running hardirq handler to complete
         * 2. Waits for any running threaded handler to complete
         * 3. Kills the IRQ thread
         * 4. Frees the irqaction structure
         *
         * If using devm_request_threaded_irq(), this is done
         * automatically and the remove function can be empty
         * (or omitted entirely).
         */
    }


================================================================================
4. INTERNAL KERNEL EXECUTION FLOW
================================================================================

    Hardware Interrupt fires
            |
            v
    Generic IRQ handling code (kernel/irq/handle.c)
            |
            v
    handle_irq_event_percpu()
            |
            v
    sensor_hard_irq()                   [HARDIRQ CONTEXT]
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
    |                                                 |
    | 2. Set IRQTF_RUNTHREAD flag on the action       |
    |    (signals the thread to execute)              |
    |                                                 |
    | 3. wake_up_process(action->thread)              |
    |    (wake up the sleeping irq/XX-sensor_irq)     |
    |                                                 |
    | 4. If IRQF_ONESHOT is set:                      |
    |    - IRQ line stays MASKED                      |
    |    - Prevents the same interrupt from firing    |
    |      again before the thread finishes           |
    |    - Critical for level-triggered interrupts    |
    +------------------------------------------------+
            |
            v
    Return from hardirq
            |
            |  ... scheduler picks up the IRQ thread ...
            |
            v
    irq_thread() loop                   [PROCESS CONTEXT]
    (kernel thread: irq/XX-sensor_irq)
            |
            v
    +------------------------------------------------+
    | Kernel thread main loop:                        |
    |                                                 |
    | 1. Sleep waiting for IRQTF_RUNTHREAD flag       |
    |    (thread sleeps here when no interrupt)       |
    |                                                 |
    | 2. Flag is set — thread wakes up                |
    |                                                 |
    | 3. Clear IRQTF_RUNTHREAD flag                   |
    |                                                 |
    | 4. Call sensor_thread_fn(irq, dev_id)           |
    |    +------------------------------------------+ |
    |    | RUNS IN FULL PROCESS CONTEXT             | |
    |    | - Can sleep                              | |
    |    | - Can call I2C/SPI functions             | |
    |    | - Can allocate memory (GFP_KERNEL)       | |
    |    | - Can acquire mutexes                    | |
    |    | - Can do blocking I/O                    | |
    |    | - Can interact with userspace (sysfs)    | |
    |    +------------------------------------------+ |
    |                                                 |
    | 5. Handler returns IRQ_HANDLED                  |
    |                                                 |
    | 6. If IRQF_ONESHOT:                             |
    |    - Call irq_finalize_oneshot()                |
    |    - UNMASK the IRQ line                        |
    |    - Device can now interrupt again             |
    |                                                 |
    | 7. Go back to step 1 (sleep)                    |
    +------------------------------------------------+


================================================================================
5. IRQF_ONESHOT EXPLAINED
================================================================================

5.1  The Problem: Without IRQF_ONESHOT
--------------------------------------------------------------------------------

    Time ----->

    IRQ line: --+    +--+    +--+    +--    (interrupt keeps firing!)
                |    |  |    |  |    |
                +----+  +----+  +----+
                ACTIVE  ACTIVE  ACTIVE

    hardirq:    [H1]   [H2]   [H3]         (top half runs repeatedly)

    thread:     [.........T1........]       (thread cannot keep up!)

    Problem:
        The hardware interrupt source has NOT been cleared because the
        thread (which does the I2C/SPI read to clear it) has not run yet.
        The device keeps asserting the interrupt line. The top half runs
        over and over, waking the thread repeatedly. This creates an
        INTERRUPT STORM that can lock up the system.

        This is especially problematic with:
        - Level-triggered interrupts (line stays asserted)
        - I2C/SPI devices (cannot clear source in hardirq context)


5.2  The Solution: With IRQF_ONESHOT
--------------------------------------------------------------------------------

    Time ----->

    IRQ line: --+                         +--
                |                         |
                +-------------------------+
                MASKED by IRQF_ONESHOT    UNMASKED by thread

    hardirq:    [H1]

    thread:           [.........T1........]
                                          ^
                                     Thread completes,
                                     clears interrupt source,
                                     kernel unmasks IRQ line

    Solution:
        IRQF_ONESHOT tells the kernel to keep the IRQ line MASKED after
        the top half returns IRQ_WAKE_THREAD. The line stays masked for
        the entire duration of the threaded handler. Only after the
        thread function returns does the kernel unmask the IRQ line.

        This guarantees:
        1. No interrupt storm while the thread is processing
        2. The thread has time to read from I2C/SPI to clear the source
        3. The next interrupt only fires after the current one is handled


5.3  When IRQF_ONESHOT is Required
--------------------------------------------------------------------------------

    REQUIRED when:
        - Using NULL top half (thread-only mode)
          The kernel enforces this — request will fail without it
        - Device uses a slow bus (I2C, SPI, etc.)
        - Interrupt source can only be cleared in the thread
        - Level-triggered interrupts

    NOT required when:
        - Top half can fully acknowledge and clear the interrupt source
        - Edge-triggered interrupts where re-assertion is not a concern
        - Top half returns IRQ_HANDLED (thread not involved)


================================================================================
6. THREAD CREATION AND LIFECYCLE
================================================================================

6.1  Thread Creation
--------------------------------------------------------------------------------

    When request_threaded_irq() is called:

    request_threaded_irq()
            |
            v
    __setup_irq()
            |
            v
    setup_irq_thread()
            |
            v
    +------------------------------------------------+
    | 1. Allocate a new kernel thread:               |
    |    kthread_create(irq_thread, action,          |
    |                   "irq/%d-%s", irq, devname)   |
    |                                                 |
    | 2. Thread name: irq/XX-devname                  |
    |    Example: irq/47-sensor_irq                   |
    |                                                 |
    | 3. Thread is created but NOT yet running        |
    |    (it will sleep until first interrupt)        |
    |                                                 |
    | 4. Thread scheduling policy:                    |
    |    - Default: SCHED_FIFO, priority 50           |
    |    - Can be changed via /proc/irq/XX/smp_affinity|
    |    - RT kernels: fully preemptible              |
    +------------------------------------------------+


6.2  Thread Main Loop (irq_thread)
--------------------------------------------------------------------------------

    static int irq_thread(void *data)
    {
        /* Simplified pseudocode */
        struct irqaction *action = data;

        while (!kthread_should_stop()) {

            /* Sleep until woken by hardirq handler */
            set_current_state(TASK_INTERRUPTIBLE);

            if (!test_bit(IRQTF_RUNTHREAD, &action->thread_flags))
                schedule();  /* Sleep here */

            set_current_state(TASK_RUNNING);

            /* Check if we should exit */
            if (kthread_should_stop())
                break;

            /* Clear the run flag */
            clear_bit(IRQTF_RUNTHREAD, &action->thread_flags);

            /* Call the threaded handler */
            action->thread_fn(action->irq, action->dev_id);

            /* If ONESHOT, unmask the IRQ line */
            if (action->flags & IRQF_ONESHOT)
                irq_finalize_oneshot(desc, action);
        }

        return 0;
    }


6.3  Thread Destruction
--------------------------------------------------------------------------------

    When free_irq() is called:

    free_irq()
            |
            v
    __free_irq()
            |
            v
    +------------------------------------------------+
    | 1. Synchronize with hardirq (wait for running   |
    |    hardirq handler to complete)                 |
    |                                                 |
    | 2. kthread_stop(action->thread)                 |
    |    - Sets kthread_should_stop() flag            |
    |    - Wakes up the thread if sleeping            |
    |    - Waits for thread to exit                   |
    |                                                 |
    | 3. Free the irqaction structure                 |
    +------------------------------------------------+


================================================================================
7. IRQF FLAGS REFERENCE
================================================================================

    +---------------------------+----------------------------------------------+
    | Flag                      | Description                                  |
    +---------------------------+----------------------------------------------+
    | IRQF_SHARED               | IRQ line is shared with other devices.       |
    |                           | Handler must check if interrupt is ours.     |
    +---------------------------+----------------------------------------------+
    | IRQF_ONESHOT              | Keep IRQ masked until thread completes.      |
    |                           | Required when top half is NULL.              |
    |                           | Prevents interrupt storms.                   |
    +---------------------------+----------------------------------------------+
    | IRQF_TRIGGER_RISING       | Trigger on rising edge of IRQ signal.        |
    +---------------------------+----------------------------------------------+
    | IRQF_TRIGGER_FALLING      | Trigger on falling edge of IRQ signal.       |
    +---------------------------+----------------------------------------------+
    | IRQF_TRIGGER_HIGH         | Trigger on high level of IRQ signal.         |
    +---------------------------+----------------------------------------------+
    | IRQF_TRIGGER_LOW          | Trigger on low level of IRQ signal.          |
    +---------------------------+----------------------------------------------+
    | IRQF_NO_SUSPEND           | Do not disable this IRQ during suspend.      |
    |                           | Used for wakeup-capable devices.             |
    +---------------------------+----------------------------------------------+
    | IRQF_NOBALANCING          | Exclude from IRQ balancing (irqbalance).     |
    +---------------------------+----------------------------------------------+
    | IRQF_NO_THREAD            | Do NOT convert to threaded IRQ even on       |
    |                           | PREEMPT_RT kernels. Use sparingly.           |
    +---------------------------+----------------------------------------------+
    | IRQF_EARLY_RESUME         | Resume this IRQ early during system resume.  |
    +---------------------------+----------------------------------------------+
    | IRQF_COND_SUSPEND         | Shared IRQ: auto-handle suspend behavior     |
    |                           | based on whether other sharers are wakeup.   |
    +---------------------------+----------------------------------------------+

    Common Flag Combinations:

        /* I2C sensor with dedicated IRQ line */
        IRQF_TRIGGER_FALLING | IRQF_ONESHOT

        /* Shared PCI device IRQ */
        IRQF_SHARED

        /* GPIO interrupt, wakeup capable */
        IRQF_TRIGGER_RISING | IRQF_ONESHOT | IRQF_NO_SUSPEND

        /* Level-triggered shared IRQ */
        IRQF_TRIGGER_LOW | IRQF_SHARED | IRQF_ONESHOT


================================================================================
8. API REFERENCE
================================================================================

8.1  Registration Functions
--------------------------------------------------------------------------------

    +----------------------------------------------------+----------------------+
    | Function                                           | Description          |
    +----------------------------------------------------+----------------------+
    | request_threaded_irq(irq, handler, thread_fn,      | Register threaded    |
    |                       flags, name, dev_id)         | IRQ handler          |
    +----------------------------------------------------+----------------------+
    | devm_request_threaded_irq(dev, irq, handler,       | Device-managed       |
    |                            thread_fn, flags,       | version (auto-free   |
    |                            name, dev_id)           | on device removal)   |
    +----------------------------------------------------+----------------------+
    | request_irq(irq, handler, flags, name, dev_id)     | Traditional (no      |
    |                                                    | thread, top half     |
    |                                                    | only)                |
    +----------------------------------------------------+----------------------+

    Note: request_irq() is actually a wrapper around request_threaded_irq()
    with thread_fn set to NULL.


8.2  Deregistration Functions
--------------------------------------------------------------------------------

    +----------------------------------------------------+----------------------+
    | Function                                           | Description          |
    +----------------------------------------------------+----------------------+
    | free_irq(irq, dev_id)                              | Unregister handler,  |
    |                                                    | wait for completion, |
    |                                                    | kill thread          |
    +----------------------------------------------------+----------------------+
    | devm_free_irq(dev, irq, dev_id)                    | Device-managed free  |
    |                                                    | (rarely needed)      |
    +----------------------------------------------------+----------------------+


8.3  IRQ Control Functions
--------------------------------------------------------------------------------

    +----------------------------------------------------+----------------------+
    | Function                                           | Description          |
    +----------------------------------------------------+----------------------+
    | disable_irq(irq)                                   | Disable IRQ and wait |
    |                                                    | for running handlers |
    +----------------------------------------------------+----------------------+
    | disable_irq_nosync(irq)                            | Disable IRQ, do not  |
    |                                                    | wait                 |
    +----------------------------------------------------+----------------------+
    | enable_irq(irq)                                    | Re-enable IRQ        |
    +----------------------------------------------------+----------------------+
    | synchronize_irq(irq)                               | Wait for running     |
    |                                                    | handlers to complete |
    +----------------------------------------------------+----------------------+
    | irq_set_affinity(irq, cpumask)                     | Set CPU affinity     |
    +----------------------------------------------------+----------------------+


================================================================================
9. COMPARISON: THREADED IRQ REGISTRATION PATTERNS
================================================================================

    Pattern A: Full — Both top half and threaded handler
    -----------------------------------------------------------------------

        request_threaded_irq(irq, my_hardirq, my_thread_fn,
                             IRQF_ONESHOT, "dev", data);

        Flow:  HW IRQ -> my_hardirq() -> IRQ_WAKE_THREAD -> my_thread_fn()

        Use when:
            - Can verify interrupt source in hardirq context
            - Need to ack hardware immediately
            - Memory-mapped devices with status registers


    Pattern B: Thread-only — NULL top half
    -----------------------------------------------------------------------

        request_threaded_irq(irq, NULL, my_thread_fn,
                             IRQF_ONESHOT, "dev", data);

        Flow:  HW IRQ -> default_handler(IRQ_WAKE_THREAD) -> my_thread_fn()

        Use when:
            - Cannot check hardware in hardirq context
            - I2C/SPI devices (all access requires sleeping)
            - IRQF_ONESHOT is MANDATORY with NULL handler


    Pattern C: Traditional — Top half only (no thread)
    -----------------------------------------------------------------------

        request_irq(irq, my_hardirq, IRQF_SHARED, "dev", data);

        Flow:  HW IRQ -> my_hardirq() -> IRQ_HANDLED

        Use when:
            - All work can be done in hardirq context
            - Ultra-fast interrupt handling needed
            - Simple hardware acknowledgment


    Pattern D: devm managed — Automatic cleanup
    -----------------------------------------------------------------------

        devm_request_threaded_irq(dev, irq, my_hardirq, my_thread_fn,
                                  IRQF_ONESHOT, "dev", data);

        Flow:  Same as Pattern A, but free_irq() called automatically
               when device is removed

        Use when:
            - Standard device driver development
            - Want to simplify cleanup code
            - Preferred for most new drivers


================================================================================
10. WHEN TO USE THREADED IRQs
================================================================================

    USE THREADED IRQs WHEN:
    -----------------------------------------------------------------------
    [+] Writing modern device drivers (this is the PREFERRED approach)
    [+] Device uses a slow bus (I2C, SPI, UART)
        — Must sleep to communicate with the device
    [+] Need clean separation of top half and bottom half
    [+] Want automatic thread management (no manual kthread creation)
    [+] Need IRQF_ONESHOT for level-triggered or edge-triggered IRQs
    [+] Building for PREEMPT_RT real-time kernel compatibility
        — Threaded IRQs are fully preemptible on RT kernels
    [+] Interrupt processing requires sleeping operations:
        - Mutex acquisition
        - Memory allocation with GFP_KERNEL
        - Blocking I/O
        - I2C/SPI transactions
    [+] Want per-IRQ thread priority and affinity control
    [+] Using devm API for clean resource management

    DO NOT USE THREADED IRQs WHEN:
    -----------------------------------------------------------------------
    [-] Ultra-high frequency interrupts (networking, high-speed storage)
        — Context switch overhead per interrupt is too high
        — Use softirqs or NAPI instead
    [-] Simple interrupt that can be handled entirely in top half
        — No need for a thread if hardirq handler suffices
    [-] Need to coalesce or batch many interrupts
        — NAPI polling model is better for this
    [-] Interrupt rate exceeds thousands per second
        — Thread wake-up and scheduling overhead becomes significant


================================================================================
11. THREADED IRQs VS OTHER MECHANISMS
================================================================================

    +------------------+-------------+----------+-------------+--------------+
    | Feature          | Threaded    | Tasklet  | Work Queue  | Softirq      |
    |                  | IRQ         |          |             |              |
    +------------------+-------------+----------+-------------+--------------+
    | Context          | Process     | Softirq  | Process     | Softirq      |
    | Can sleep        | Yes         | No       | Yes         | No           |
    | Dedicated thread | Yes (per    | No       | No (shared  | No           |
    |                  | IRQ action) |          | kworker)    |              |
    | Priority control | Yes (RT     | No       | Limited     | No           |
    |                  | priority)   |          |             |              |
    | PREEMPT_RT       | Native      | Forced   | Native      | Forced       |
    | compatible       |             | threaded |             | threaded     |
    | Latency          | Medium      | Low      | Higher      | Lowest       |
    | Serialization    | Per IRQ     | Per      | Configurable| Per CPU      |
    |                  | action      | tasklet  |             | (same type)  |
    | ONESHOT support  | Yes         | No       | N/A         | No           |
    | Typical use      | I2C, SPI,   | Simple   | Complex     | Networking,  |
    |                  | GPIO, modern| legacy   | deferred    | block I/O,   |
    |                  | drivers     | drivers  | work        | timers       |
    +------------------+-------------+----------+-------------+--------------+


================================================================================
12. PREEMPT_RT AND THREADED IRQs
================================================================================

    On standard kernels:
        - Top half runs in hardirq context (non-preemptible)
        - Threaded handler runs in process context (preemptible)

    On PREEMPT_RT kernels:
        - ALL interrupt handlers are forced into threads
          (unless marked with IRQF_NO_THREAD)
        - This makes the entire system fully preemptible
        - Threaded IRQs work naturally on RT kernels
        - Softirqs and tasklets are also forced into threads on RT

    This is a key reason why threaded IRQs are the preferred approach:
    they work correctly and efficiently on both standard and RT kernels
    without any code changes.

    Thread priority on RT:
        - IRQ threads run with SCHED_FIFO policy
        - Default priority: 50
        - Can be adjusted via chrt or /proc/irq/XX/smp_affinity
        - Allows fine-grained control over interrupt priorities
        - Critical for real-time applications (audio, industrial control)


================================================================================
13. COMMON PITFALLS AND BEST PRACTICES
================================================================================

    PITFALL 1:  Forgetting IRQF_ONESHOT with NULL handler
    -----------------------------------------------------------------------
    If you pass NULL as the top half handler, IRQF_ONESHOT is MANDATORY.
    The kernel will reject the request and return -EINVAL without it.

        /* WRONG — will fail */
        request_threaded_irq(irq, NULL, thread_fn, 0, "dev", data);

        /* CORRECT */
        request_threaded_irq(irq, NULL, thread_fn,
                             IRQF_ONESHOT, "dev", data);


    PITFALL 2:  Sleeping in the top half handler
    -----------------------------------------------------------------------
    The top half (handler) runs in hardirq context. Calling any function
    that may sleep (mutex_lock, kmalloc with GFP_KERNEL, I2C/SPI
    functions) will cause a BUG or system crash.

    All sleeping work must go in thread_fn.


    PITFALL 3:  Not checking IRQ_NONE for shared interrupts
    -----------------------------------------------------------------------
    With IRQF_SHARED, multiple devices share the same IRQ line. Each
    handler must check if the interrupt is from its device and return
    IRQ_NONE if not. Failing to do so causes incorrect handling of
    other devices' interrupts.


    PITFALL 4:  Race between free_irq and interrupt
    -----------------------------------------------------------------------
    free_irq() is synchronous — it waits for any running handler (both
    hardirq and thread) to complete. However, ensure no code path can
    re-enable or re-register the IRQ after free_irq() returns.


    PITFALL 5:  Interrupt storm with level-triggered without ONESHOT
    -----------------------------------------------------------------------
    If using a level-triggered interrupt without IRQF_ONESHOT, and the
    interrupt source can only be cleared in the thread, the hardirq
    handler will be called repeatedly before the thread runs. Always
    use IRQF_ONESHOT for level-triggered interrupts with threaded
    handlers.


    BEST PRACTICE:  Use devm_request_threaded_irq()
    -----------------------------------------------------------------------
    The devm variant automatically frees the IRQ when the device is
    removed. This simplifies error handling and prevents resource leaks.


    BEST PRACTICE:  Prefer threaded IRQs over tasklets for new drivers
    -----------------------------------------------------------------------
    The kernel community recommends threaded IRQs for new driver
    development. Tasklets are considered semi-deprecated. Threaded IRQs
    are cleaner, more flexible, and RT-compatible.


    BEST PRACTICE:  Use IRQF_ONESHOT by default
    -----------------------------------------------------------------------
    Unless you have a specific reason not to, include IRQF_ONESHOT.
    It prevents subtle interrupt storm bugs and is required for many
    common use cases.


================================================================================
14. COMPLETE EXECUTION TIMELINE
================================================================================

    TIME   EVENT                                      CONTEXT
    -----  -----------------------------------------  -----------------------
    T0     Device asserts interrupt line               Hardware
    T1     CPU receives interrupt                      Hardware
    T2     CPU jumps to IDT/GIC handler                Hardirq (IRQs off)
    T3     Generic IRQ dispatch code runs              Hardirq
    T4     handle_irq_event_percpu() called            Hardirq
    T5     sensor_hard_irq() executes                  Hardirq
    T6       -> Reads status register                  Hardirq
    T7       -> Acknowledges interrupt                 Hardirq
    T8       -> Returns IRQ_WAKE_THREAD                Hardirq
    T9     __irq_wake_thread() called                  Hardirq
    T10      -> Sets IRQTF_RUNTHREAD flag              Hardirq
    T11      -> wake_up_process(irq_thread)            Hardirq
    T12      -> If ONESHOT: mask IRQ line              Hardirq
    T13    Return from hardirq                         Transition
    T14    ... scheduler runs ...                      Scheduler
    T15    irq/47-sensor_irq thread scheduled          Process context
    T16    irq_thread() wakes up                       Process context
    T17    Clears IRQTF_RUNTHREAD flag                 Process context
    T18    Calls sensor_thread_fn()                    Process context
    T19      -> mutex_lock() (may sleep)               Process context
    T20      -> i2c_smbus_read() (sleeps for I2C)      Process context
    T21      -> Processes sensor data                  Process context
    T22      -> sysfs_notify() to userspace            Process context
    T23      -> mutex_unlock()                         Process context
    T24    sensor_thread_fn() returns IRQ_HANDLED      Process context
    T25    If ONESHOT: irq_finalize_oneshot()          Process context
    T26      -> Unmask IRQ line                        Process context
    T27    irq_thread() goes back to sleep             Process context
    T28    Device can now interrupt again               Hardware


================================================================================
15. VISIBILITY AND DEBUGGING
================================================================================

    /proc/interrupts:
        Shows IRQ counts, handler names, and CPU distribution
        The thread name appears in the handler column

    /proc/irq/XX/:
        Per-IRQ configuration directory
        - smp_affinity     : CPU affinity mask
        - smp_affinity_list: CPU affinity as list
        - node             : NUMA node
        - spurious         : Spurious interrupt count

    ps output:
        IRQ threads visible as:
            irq/47-sensor_irq
            irq/23-i2c_adapter
            irq/16-ehci_hcd

    ftrace / tracepoints:
        irq:irq_handler_entry    — Top half entry
        irq:irq_handler_exit     — Top half exit
        irq:irq_thread_entry     — Thread handler entry (if available)

    /sys/kernel/debug/irq/:
        Additional debug information on some kernels


================================================================================
END OF DOCUMENT
================================================================================
