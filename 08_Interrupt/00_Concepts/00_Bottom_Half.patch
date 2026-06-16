================================================================================
          TOP HALF AND BOTTOM HALF — LINUX KERNEL INTERRUPT HANDLING
================================================================================


OVERVIEW
--------------------------------------------------------------------------------
In Linux kernel interrupt handling, interrupt processing is split into two
parts for efficiency. The "top half" handles the immediate, time-critical
work, while the "bottom half" defers the bulk of processing to a later,
safer time.


================================================================================
1. TOP HALF (INTERRUPT HANDLER)
================================================================================

Definition:
    The immediate, time-critical part of interrupt processing.
    Runs in interrupt context with interrupts disabled.
    Must execute quickly and return as fast as possible.

Responsibilities:
    - Acknowledge the interrupt to the hardware
    - Copy essential data from hardware buffers
    - Schedule the bottom half for deferred processing
    - Return control as quickly as possible

Characteristics:
    +------------------+----------------------------------------------+
    | Feature          | Detail                                       |
    +------------------+----------------------------------------------+
    | Speed            | Must be very fast                            |
    | Context          | Interrupt context                            |
    | Can sleep?       | No                                           |
    | Interrupts       | Disabled (partially or fully)                |
    | Priority         | High                                         |
    +------------------+----------------------------------------------+


================================================================================
2. BOTTOM HALF (DEFERRED WORK)
================================================================================

Definition:
    The deferred, less time-critical part of interrupt processing.
    Runs later when it is safer and more convenient.
    Handles the bulk of the processing work.
    Allows other interrupts to occur during execution.

Characteristics:
    +------------------+----------------------------------------------+
    | Feature          | Detail                                       |
    +------------------+----------------------------------------------+
    | Speed            | Can take longer                              |
    | Context          | Depends on mechanism used                    |
    | Can sleep?       | Depends on mechanism used                    |
    | Interrupts       | Enabled                                      |
    | Priority         | Lower than top half                          |
    +------------------+----------------------------------------------+


================================================================================
3. BOTTOM HALF MECHANISMS
================================================================================

3.1 Softirqs
    - Statically defined at compile time
    - High performance
    - Used for networking, block I/O, timers
    - Cannot sleep
    - Can run concurrently on multiple CPUs

3.2 Tasklets
    - Built on top of softirqs
    - Dynamically created at runtime
    - Simpler API than raw softirqs
    - Cannot sleep
    - A given tasklet runs on only one CPU at a time (serialized)

3.3 Work Queues
    - Run in process context (kernel threads)
    - CAN sleep (suitable for work needing locks, memory allocation, etc.)
    - More overhead than softirqs/tasklets
    - Most flexible mechanism

3.4 Threaded IRQs
    - Interrupt handling moved into dedicated kernel threads
    - CAN sleep
    - Used with request_threaded_irq()
    - Common in RT (real-time) kernels


================================================================================
4. EXAMPLE — NETWORK CARD INTERRUPT
================================================================================

Step 1: TOP HALF
    - Hardware raises an interrupt
    - CPU invokes the registered interrupt handler
    - Handler acknowledges the interrupt on the NIC
    - Handler copies the packet from NIC buffer into RAM (DMA/ring buffer)
    - Handler schedules the bottom half (e.g., raises NET_RX_SOFTIRQ)
    - Handler returns quickly

Step 2: BOTTOM HALF
    - Softirq or NAPI poll function runs when safe
    - Processes the packet through the network protocol stack
    - Handles Ethernet, IP, TCP/UDP header parsing
    - Delivers data to the appropriate socket receive queue
    - Wakes up any userspace process waiting on that socket


================================================================================
5. WHY SPLIT INTERRUPT HANDLING?
================================================================================

Without splitting:
    Interrupt arrives --> Long processing --> Other interrupts MISSED

With splitting:
    Interrupt arrives --> Quick top half --> Return --> Bottom half later

Benefits:
    - Reduced interrupt latency
    - Better overall system responsiveness
    - Prevents lost interrupts
    - Allows concurrent processing of multiple events
    - Keeps critical sections (interrupts-disabled time) minimal


================================================================================
6. SUMMARY DIAGRAM
================================================================================

    Hardware Interrupt
           |
           v
    +-------------+
    |  TOP HALF   |  <-- Fast, minimal work, interrupts disabled
    |             |      Runs in interrupt context
    +------+------+
           |
           | schedules
           v
    +-------------+
    | BOTTOM HALF |  <-- Deferred, bulk processing, interrupts enabled
    |             |      Runs via softirq / tasklet / workqueue / thread
    +-------------+


================================================================================
7. QUICK COMPARISON TABLE
================================================================================

    +----------------+-----------+-----------+------------+--------------+
    | Mechanism      | Context   | Can Sleep | Concurrency| Use Case     |
    +----------------+-----------+-----------+------------+--------------+
    | Top Half       | Interrupt | No        | N/A        | HW ack, copy |
    | Softirq        | Softirq   | No        | Multi-CPU  | Net, Block   |
    | Tasklet        | Softirq   | No        | Serialized | Simple defer |
    | Work Queue     | Process   | Yes       | Multi-CPU  | Complex work |
    | Threaded IRQ   | Process   | Yes       | Serialized | RT, sleepy   |
    +----------------+-----------+-----------+------------+--------------+

================================================================================
              Bottom Half Mechanisms - Deep Dive
================================================================================


Hardware Interrupt (IRQ)
         │
         ▼
┌─────────────────┐
│    TOP HALF      │  (Hardirq Context)
│  (ISR Handler)   │
└────────┬────────┘
         │ Schedules one of these:
         ▼
┌──────────────────────────────────────────────┐
│              BOTTOM HALF MECHANISMS          │
│                                              │
│  ┌──────────┐  ┌─────────┐  ┌──────────────┐ │
│  │ Softirqs │  │Tasklets │  │ Work Queues  │ │
│  │          │  │(on top  │  │              │ │
│  │          │  │of softirq)││              │ │
│  └──────────┘  └─────────┘  └──────────────┘ │
│                                              │
│  ┌──────────────┐                            │
│  │Threaded IRQs │                            │
│  └──────────────┘                            │
└──────────────────────────────────────────────┘
================================================================================
END OF DOCUMENT
================================================================================
