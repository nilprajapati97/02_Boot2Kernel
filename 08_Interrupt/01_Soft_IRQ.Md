================================================================================
                    SOFTIRQS (Software Interrupts) - Deep Dive
================================================================================

--------------------------------------------------------------------------------
1. WHAT ARE SOFTIRQS?
--------------------------------------------------------------------------------

    - Statically defined at compile time in the kernel
    - Highest priority bottom half mechanism
    - Can run simultaneously on multiple CPUs
    - Limited to fixed number (currently ~10 types)
    - Only core kernel developers should add new softirqs
    - Requires careful locking due to parallel execution

--------------------------------------------------------------------------------
2. SOFTIRQ TYPES DEFINED IN KERNEL
--------------------------------------------------------------------------------

    File: include/linux/interrupt.h

    enum {
        HI_SOFTIRQ = 0,        // High-priority tasklets
        TIMER_SOFTIRQ,          // Timers
        NET_TX_SOFTIRQ,         // Network transmit
        NET_RX_SOFTIRQ,         // Network receive
        BLOCK_SOFTIRQ,          // Block device
        IRQ_POLL_SOFTIRQ,       // IRQ polling
        TASKLET_SOFTIRQ,        // Regular tasklets
        SCHED_SOFTIRQ,          // Scheduler
        HRTIMER_SOFTIRQ,        // High-resolution timers
        RCU_SOFTIRQ,            // RCU (Read-Copy-Update)
    };

    Total = 10 types (fixed at compile time, cannot add dynamically)

--------------------------------------------------------------------------------
3. INTERNAL EXECUTION FLOW
--------------------------------------------------------------------------------

    Step 1: Top half (hardirq handler) sets softirq pending bit
                |
                v
    Step 2: On return from hardirq, kernel checks pending softirqs
                |
                v
    Step 3: __do_softirq() is called
                |
                v
    Step 4: Iterates through all pending softirqs (bit by bit)
                |
                v
    Step 5: Calls the registered handler for each pending softirq
                |
                v
    Step 6: If more softirqs raised during processing
                |
                +--- Retry up to MAX_SOFTIRQ_RESTART (10 times)
                |
                +--- If still pending after 10 retries
                         |
                         v
                     Wake up ksoftirqd kernel thread
                     (per-CPU thread that processes remaining softirqs)

--------------------------------------------------------------------------------
4. DETAILED CODE FLOW
--------------------------------------------------------------------------------

    STEP 1: Define softirq handler function
    -----------------------------------------

        static void my_net_rx_action(struct softirq_action *a)
        {
            /* Process received network packets */
            struct sk_buff *skb;

            while ((skb = dequeue_packet())) {
                process_packet(skb);    // Protocol processing
            }
        }

    STEP 2: Register softirq (done once during kernel boot)
    ---------------------------------------------------------

        void __init net_dev_init(void)
        {
            open_softirq(NET_RX_SOFTIRQ, my_net_rx_action);
        }

        - open_softirq() maps the softirq number to the handler
        - Called only during kernel initialization
        - Cannot be done from loadable modules

    STEP 3: Raise softirq from top half (hardirq handler)
    -------------------------------------------------------

        irqreturn_t network_card_isr(int irq, void *dev_id)
        {
            /* Acknowledge hardware interrupt */
            acknowledge_interrupt(dev_id);

            /* Copy data from NIC buffer to memory (quick operation) */
            copy_from_nic_buffer(dev_id);

            /* Raise softirq - schedule bottom half processing */
            raise_softirq(NET_RX_SOFTIRQ);     // Sets pending bit

            return IRQ_HANDLED;
        }

        - raise_softirq() sets a bit in per-CPU pending bitmask
        - The actual handler runs later when softirqs are processed

--------------------------------------------------------------------------------
5. EXECUTION CONTEXT DIAGRAM (MULTI-CPU)
--------------------------------------------------------------------------------

        CPU 0                           CPU 1
          |                               |
          v                               v
    +---------------+             +---------------+
    |   NET_RX      |             |   NET_RX      |
    |   softirq     |             |   softirq     |
    |   running     |             |   running     |
    +---------------+             +---------------+

    SAME softirq can run on BOTH CPUs at the SAME time!

    WARNING: This is why softirqs need very careful locking!
             You must use spin_lock / per-CPU variables to
             protect shared data.

    Comparison with Tasklets:
        - Softirq  : Same type runs on multiple CPUs simultaneously
        - Tasklet  : Same tasklet NEVER runs on two CPUs at once

--------------------------------------------------------------------------------
6. KEY KERNEL FUNCTIONS FOR SOFTIRQS
--------------------------------------------------------------------------------

    Function                    Description
    --------------------------  ------------------------------------------------
    open_softirq(nr, action)    Register handler for softirq number 'nr'
    raise_softirq(nr)           Mark softirq 'nr' as pending (schedule it)
    raise_softirq_irqoff(nr)    Same as above but when IRQs already disabled
    __do_softirq()              Process all pending softirqs
    local_bh_disable()          Disable softirq processing on current CPU
    local_bh_enable()           Re-enable softirq processing on current CPU

--------------------------------------------------------------------------------
7. WHEN DOES SOFTIRQ ACTUALLY RUN?
--------------------------------------------------------------------------------

    Softirqs are checked and executed at these points:

        1. After handling a hardware interrupt (irq_exit)
        2. In the ksoftirqd kernel thread (per-CPU)
        3. When code explicitly checks (local_bh_enable)

    Execution Priority Order:

        Hardware Interrupt (highest)
            |
            v
        Softirq Processing
            |
            v
        ksoftirqd thread (if softirqs keep coming)
            |
            v
        Normal process scheduling (lowest)

--------------------------------------------------------------------------------
8. ksoftirqd THREAD
--------------------------------------------------------------------------------

    - One per CPU: ksoftirqd/0, ksoftirqd/1, ksoftirqd/2, ...
    - Runs as a normal kernel thread (low priority)
    - Activated when softirqs are raised too frequently
    - Prevents softirqs from starving user processes

    Flow:
        __do_softirq() runs
            |
            v
        Processes pending softirqs
            |
            v
        New softirqs raised during processing?
            |
            +--- YES --> Retry (up to 10 times)
            |                |
            |                v
            |            Still pending after 10 retries?
            |                |
            |                +--- YES --> wake_up(ksoftirqd)
            |                |
            |                +--- NO  --> Done
            |
            +--- NO  --> Done

--------------------------------------------------------------------------------
9. WHEN TO USE SOFTIRQS
--------------------------------------------------------------------------------

    USE SOFTIRQS WHEN:
    ----------------------------------------
        - Extremely high performance is needed
        - You are a core kernel subsystem developer
        - Working on networking stack, block I/O, timers
        - Code must handle concurrent execution on multiple CPUs
        - You understand per-CPU data and complex locking

    DO NOT USE SOFTIRQS WHEN:
    ----------------------------------------
        - You are writing a device driver module
        - You need to sleep or block
        - You want simple serialization (use tasklets instead)
        - You are not modifying core kernel code
        - You want dynamic creation at runtime

--------------------------------------------------------------------------------
10. SOFTIRQ vs OTHER MECHANISMS (QUICK REFERENCE)
--------------------------------------------------------------------------------

    Property              Softirq         Tasklet         Work Queue
    --------------------  --------------  --------------  ---------------
    Context               Atomic          Atomic          Process
    Can Sleep             NO              NO              YES
    Parallel on CPUs      YES             NO (same one)   YES
    Creation              Static          Dynamic         Dynamic
    Performance           Highest         High            Lower
    Complexity            High            Low             Medium
    Used By               Kernel Core     Drivers         Drivers

--------------------------------------------------------------------------------
11. PRACTICAL EXAMPLE: NETWORK PACKET RECEIVE
--------------------------------------------------------------------------------

    Physical Flow:
    ----------------------------------------

        Network packet arrives at NIC hardware
                    |
                    v
        NIC copies packet to ring buffer in RAM (DMA)
                    |
                    v
        NIC raises hardware interrupt
                    |
                    v
        TOP HALF: network_card_isr() runs [HARDIRQ CONTEXT]
            - Acknowledge interrupt to NIC
            - Copy packet reference from ring buffer
            - Call raise_softirq(NET_RX_SOFTIRQ)
            - Return IRQ_HANDLED
                    |
                    v
        irq_exit() checks pending softirqs
                    |
                    v
        __do_softirq() finds NET_RX_SOFTIRQ pending
                    |
                    v
        BOTTOM HALF: net_rx_action() runs [SOFTIRQ CONTEXT]
            - Dequeue packets from per-CPU queue
            - Process each packet through protocol stack
            - TCP/IP header parsing
            - Checksum verification
            - Route lookup
            - Deliver to socket receive buffer
                    |
                    v
        Application reads data via recv() / read()

    Why Softirq Here:
    ----------------------------------------
        - Network cards generate thousands of interrupts per second
        - Each interrupt may have multiple packets
        - Processing must be extremely fast
        - Must run on multiple CPUs for throughput
        - Cannot afford context switch overhead of work queues

================================================================================
                              END OF SOFTIRQ SECTION
================================================================================
