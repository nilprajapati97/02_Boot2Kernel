When a **Linux system crashes** (especially in **Embedded Linux, Android, Automotive, AMD, NVIDIA, Qualcomm, Intel, NXP**, etc.), interviewers are usually trying to evaluate **your debugging methodology**, not just whether you know specific commands.

A good kernel engineer always follows a structured approach.

---

# Debugging a System Crash

## Step 1: Identify the Type of Crash

The first step is determining **what kind of crash occurred.**

```
                     System Crash
                          |
        ------------------------------------
        |          |          |            |
      Panic      Oops      Hang        Reboot
```

Every type is debugged differently.

---

## 1. Kernel Panic

Example:

```
Kernel panic - not syncing:
Fatal exception in interrupt
```

The kernel cannot continue.

Possible reasons

* NULL pointer dereference
* Stack corruption
* Scheduler corruption
* Memory corruption
* Driver bug
* Unable to mount root filesystem

---

## 2. Kernel Oops

Example

```
Unable to handle kernel NULL pointer dereference
```

Kernel reports an error but sometimes continues.

Usually caused by

```
NULL pointer

Use-after-free

Buffer overflow

Invalid DMA

Race condition
```

---

## 3. System Hang

Symptoms

```
No keyboard

No SSH

No logs

Watchdog resets later
```

Could be

```
Deadlock

Spinlock stuck

Interrupt disabled forever

Infinite loop

CPU soft lockup
```

---

## 4. Random Reboot

Usually

```
Watchdog

Power issue

Kernel panic

Hardware reset

Brownout
```

---

# Step 2 : Ask Questions

Before opening the code, ask:

### Does it happen every boot?

```
Yes
```

Probably

```
Driver

Device Tree

Memory

Probe
```

---

### Random?

```
Maybe race condition

DMA corruption

Memory corruption

Stack overflow
```

---

### During boot?

```
Probe issue

Clock

GPIO

Power domain

Interrupt

I2C/SPI
```

---

### During runtime?

```
Interrupt

User application

DMA

Memory corruption
```

---

# Step 3 : Collect Logs

Never debug without logs.

Collect

```
dmesg

journalctl

serial console

netconsole

pstore

ramoops

crash dump

kdump
```

Example

```
dmesg -T

journalctl -k
```

---

# Step 4 : Read the Last Kernel Message

Usually the last message is important.

Example

```
BUG:

Unable to handle kernel NULL pointer dereference

PC is at my_driver_write+0x34

LR is at vfs_write+0x88
```

Immediately know

```
Crash inside

my_driver_write()
```

---

# Step 5 : Decode the Stack Trace

Example

```
Call Trace:

my_driver_irq()

my_driver_transfer()

spi_sync()

vfs_write()

sys_write()
```

Read **bottom to top**

Application

↓

System Call

↓

VFS

↓

Driver

↓

Crash

---

# Step 6 : Decode Program Counter (PC)

Suppose

```
PC = ffffffc010123450
```

Use

```
addr2line

gdb

scripts/decode_stacktrace.sh
```

Example

```
addr2line -e vmlinux ffffffc010123450
```

Output

```
driver.c:210
```

Now you know the exact line.

---

# Step 7 : Inspect Registers

Example

```
PC

LR

SP

X0-X30
```

On ARM64

Example

```
PC

ffff800010203450

LR

ffff800010102000

SP

ffff800013ffbc00
```

Important registers

```
PC

Current instruction

LR

Caller

SP

Stack

X0-X7

Arguments
```

---

# Step 8 : Check NULL Pointer

Example

```
X0 = 0x0
```

Then

```
ldr x1,[x0]
```

Boom.

NULL pointer.

---

# Step 9 : Check Stack Overflow

Example

```
recursive_function()

recursive_function()

recursive_function()
```

Stack becomes

```
8 KB

16 KB

overflow
```

Kernel panic.

Check

```
CONFIG_DEBUG_STACKOVERFLOW
```

---

# Step 10 : Check Memory Corruption

Very common.

Example

```
char buf[10];

strcpy(buf,"123456789012345");
```

Stack corrupted.

Later

```
panic
```

Enable

```
KASAN

KFENCE

SLUB_DEBUG
```

---

# Step 11 : Check Use-after-free

Example

```
kfree(dev);

dev->name
```

Crash.

Enable

```
KASAN
```

Output

```
Use-after-free
```

---

# Step 12 : Check DMA

DMA writing outside allocated memory

```
DMA Buffer

-------------

Valid

-------------

DMA writes here

-------------

Kernel stack

-------------
```

Boom.

Check

```
dma_alloc_coherent()

DMA length

Cache sync
```

---

# Step 13 : Check Interrupt Context

Suppose crash occurs inside

```
IRQ Handler
```

Verify

```
Sleeping API used?

Mutex?

copy_to_user()?

kmalloc(GFP_KERNEL)?
```

Illegal in interrupt context.

---

# Step 14 : Check Spinlocks

Deadlock example

```
CPU0

spin_lock()

...

spin_lock()

Forever
```

Enable

```
LOCKDEP
```

---

# Step 15 : Check Soft Lockup

Example

```
while(1)
{

}
```

CPU never schedules.

Kernel reports

```
Soft lockup detected
```

---

# Step 16 : Check Hard Lockup

Interrupts disabled forever.

Example

```
local_irq_disable();

while(1)
{

}
```

No timer interrupt.

Kernel

```
Hard LOCKUP
```

---

# Step 17 : Check Watchdog

Example

```
Watchdog timeout

Rebooting...
```

Ask

Why watchdog fired?

```
Deadlock?

Hang?

Interrupt disabled?

Infinite loop?
```

---

# Step 18 : Use Magic SysRq

If system hangs.

```
echo t > /proc/sysrq-trigger
```

Shows

```
All task stack traces
```

Useful

```
Deadlock

Hung task

Blocked mutex
```

Other useful SysRq commands:

```
echo w > /proc/sysrq-trigger   # Blocked tasks
echo l > /proc/sysrq-trigger   # Backtraces on all CPUs
echo m > /proc/sysrq-trigger   # Memory info
```

---

# Step 19 : Use Crash Dump

Enable

```
kdump
```

After panic

```
vmcore
```

Analyze

```
crash vmlinux vmcore
```

Useful commands

```
bt

ps

kmem

files

mount

log
```

---

# Step 20 : Enable Debug Options

Kernel config

```
CONFIG_KASAN=y

CONFIG_LOCKDEP=y

CONFIG_DEBUG_SPINLOCK=y

CONFIG_DEBUG_MUTEXES=y

CONFIG_DEBUG_LIST=y

CONFIG_SLUB_DEBUG=y

CONFIG_KFENCE=y

CONFIG_DEBUG_PAGEALLOC=y

CONFIG_PROVE_LOCKING=y
```

---

# ARM64 Exception Flow During a Crash

```
User Process
      |
      |
sys_write()
      |
      V
Driver
      |
NULL Pointer
      |
      V
Synchronous Exception
      |
      V
EL1 Exception Vector
      |
      V
do_mem_abort()
      |
      V
die()
      |
      V
Oops
      |
      |
panic()
(optional)
```

---

# Complete Debug Flow

```
Crash
   |
   V
Collect Logs
   |
   V
Kernel Panic?
   |
  Yes
   |
Read Call Trace
   |
Decode PC
   |
Check Registers
   |
Locate Source Line
   |
Check NULL
   |
Check Memory
   |
Check Lock
   |
Check IRQ Context
   |
Check DMA
   |
Enable KASAN
   |
Reproduce
   |
Fix
   |
Stress Test
```

---

# Real Interview Example

**Question:** *The board crashes while probing an I2C driver. How would you debug it?*

**Approach:**

1. Reproduce the crash consistently and collect the full serial console log (`earlycon`/UART if available).
2. Identify whether it's a kernel panic, oops, or hang.
3. Read the complete call trace to identify the failing function.
4. Decode the PC/LR addresses using `vmlinux` and `addr2line` or `scripts/decode_stacktrace.sh`.
5. Verify whether the crash occurs before or after the driver's `probe()` function.
6. Check Device Tree properties (`compatible`, `reg`, interrupts, clocks, regulators, GPIOs) and ensure they match the driver.
7. Verify all resources acquired with `devm_*` APIs and check error handling paths.
8. Confirm the I²C bus is functioning by enabling I²C debug logs or using an oscilloscope/logic analyzer to inspect SDA/SCL waveforms.
9. Check for NULL pointers, invalid `i2c_client` usage, incorrect register addresses, and failed memory allocations.
10. Enable `CONFIG_KASAN` and other relevant debug options if memory corruption is suspected.
11. If needed, capture a `vmcore` with `kdump` and analyze it using the `crash` utility.
12. After implementing a fix, perform repeated boot and stress testing to ensure the issue is resolved and doesn't regress.

---

## What Interviewers Expect

For companies such as AMD, NVIDIA, Qualcomm, Intel, and automotive Linux teams, they expect a **methodical debugging process** rather than immediately guessing the root cause:

1. **Classify the failure** (panic, oops, hang, reboot).
2. **Gather evidence** (serial logs, `dmesg`, `pstore`, `vmcore`).
3. **Analyze the stack trace** and decode the program counter.
4. **Inspect registers and memory state**.
5. **Correlate the failure with the relevant driver or subsystem**.
6. **Use kernel debugging tools** (`KASAN`, `LOCKDEP`, `ftrace`, `dynamic_debug`, `kgdb`, `crash`, `kdump`).
7. **Form a hypothesis, reproduce the issue, fix it, and validate the fix with stress testing.**

Following this structured workflow demonstrates that you can debug complex kernel crashes efficiently and systematically.


For **Linux Kernel Driver interviews** (AMD, NVIDIA, Qualcomm, Intel, Samsung, NXP, TI, etc.), when an interviewer asks:

> **"How do you debug a system crash?"**

they usually expect you to explain **how the CPU reaches the crash, how the kernel handles it, how you collect evidence, and how you find the root cause**—not just a list of commands.

---

# Complete Linux System Crash Debugging Guide

## Step 1: What do you mean by "System Crash"?

A system crash means **the CPU cannot continue executing normally** because of either:

* Software bug
* Hardware failure
* Memory corruption
* Invalid instruction
* Kernel bug
* Driver bug
* CPU exception

Depending on the severity, Linux may:

```
Continue execution
        ↓
Kernel Oops
        ↓
Kernel Panic
        ↓
Watchdog Reset
        ↓
Hardware Reset
```

---

# Step 2: Understand the Timeline

Imagine a user writes to a character driver.

```
User Space
--------------------------------
write(fd, buffer, 100);

        |
        V

System Call

        |

sys_write()

        |

vfs_write()

        |

driver->write()

        |

Driver Bug

        |

NULL Pointer

        |

CPU Exception

        |

Exception Vector

        |

Kernel Exception Handler

        |

Oops / Panic

        |

Logs Printed

        |

System Stops
```

Everything after the NULL pointer is automatic.

---

# Step 3: CPU Detects Something Wrong

Suppose your driver contains

```c
struct device *dev = NULL;

dev->id = 10;
```

CPU executes

```
STR W1,[X0]
```

where

```
X0 = 0x0
```

CPU immediately detects

```
Invalid virtual address
```

CPU cannot continue.

Instead, it raises

```
Synchronous Exception
```

---

# Step 4: ARM64 Exception Flow

Internally ARM64 executes

```
Current Instruction

↓

MMU Translation

↓

Address Invalid

↓

Data Abort

↓

Exception Vector

↓

EL1 Sync Handler

↓

do_mem_abort()

↓

die()

↓

Oops

↓

panic()
```

The CPU **never returns** to the faulty instruction.

---

# Step 5: Linux Prints Crash Information

Example

```
Unable to handle kernel NULL pointer dereference

Mem abort info:

ESR = 0x96000004

FSC = Translation fault

PC : my_driver_write+0x40

LR : vfs_write+0xa4

SP : ffff800013ffbc40
```

Every line is important.

---

# Let's Understand Every Field

---

## PC

```
PC = Program Counter
```

Current instruction executing.

Example

```
PC

my_driver_write+0x40
```

Meaning

```
Crash occurred here.
```

---

## LR

```
Link Register
```

Who called current function.

```
driver_write()

↑

vfs_write()
```

LR tells you

```
Caller
```

---

## SP

```
Stack Pointer
```

Current kernel stack.

Useful for

* Stack overflow
* Local variables
* Backtrace

---

## ESR

```
Exception Syndrome Register
```

ARM64 stores

```
Why exception occurred.
```

Example

```
ESR

0x96000004
```

means

```
Data Abort

Level 0 Translation Fault
```

---

## FAR

```
Fault Address Register
```

Stores

```
Address causing fault.
```

Example

```
FAR

0x00000000
```

Immediately indicates

```
NULL pointer.
```

---

# Step 6: Read the Call Trace

Example

```
Call Trace

panic()

die()

do_mem_abort()

el1_sync()

driver_write()

vfs_write()

ksys_write()

syscall()
```

Read **bottom to top** because the stack grows downward.

```
Application

↓

syscall

↓

VFS

↓

Driver

↓

Crash

↓

Exception

↓

Oops

↓

panic
```

---

# Step 7: Decode PC Address

Suppose

```
PC

ffff800010203450
```

Need exact source line.

Use

```bash
addr2line -e vmlinux ffff800010203450
```

Result

```
driver.c:258
```

Now inspect line 258.

---

# Step 8: Read Registers

Example

```
X0

0x0

X1

0x100

X2

0x200

PC

ffff...

LR

ffff...
```

On ARM64

```
X0-X7

Function arguments
```

Suppose

```
driver_write(struct dev *dev)

↓

X0

NULL
```

Crash becomes obvious.

---

# Step 9: Look at Source Code

Suppose

```c
static ssize_t write(...)
{
    dev->buffer[0]=1;
}
```

If

```
dev=NULL
```

Then

```
NULL->buffer
```

Crash.

---

# Step 10: Ask Important Questions

A kernel engineer never jumps directly into the code. They first ask:

### Did it happen every boot?

```
Yes
```

Likely

* Probe failure
* Device Tree
* Clock
* GPIO
* Regulator

---

### Did it happen after 30 minutes?

Likely

* Memory corruption
* DMA corruption
* Race condition
* Use-after-free

---

### Happens only under stress?

Think

```
Concurrency

Spinlock

Atomic

DMA

IRQ
```

---

# Step 11: Collect Every Possible Log

Never debug using only one log.

Collect

```
UART Console

↓

dmesg

↓

journalctl

↓

pstore

↓

ramoops

↓

kdump

↓

vmcore

↓

netconsole

↓

ftrace
```

Serial logs are the most valuable because they capture messages even if storage is unavailable.

---

# Step 12: Use Dynamic Debugging

Enable runtime debug prints without recompiling.

```bash
echo 'file drivers/i2c/* +p' > /sys/kernel/debug/dynamic_debug/control
```

Now every `pr_debug()` becomes visible.

---

# Step 13: Enable Function Tracing

```
echo function > current_tracer
```

Kernel records

```
Function Entry

↓

Function Exit

↓

Crash
```

You know the last executed function.

---

# Step 14: Check Memory Corruption

Most crashes are not where the bug happened.

Example

```c
char buf[8];

strcpy(buf,"abcdefghijklmnop");
```

Overflow corrupts nearby stack memory.

```
Time

10:00

↓

Overflow

↓

Nothing happens

↓

10:20

↓

Scheduler crashes
```

The scheduler isn't the real culprit—the earlier overflow is.

Use:

* `CONFIG_KASAN`
* `CONFIG_KFENCE`
* `CONFIG_DEBUG_PAGEALLOC`
* `CONFIG_SLUB_DEBUG`

---

# Step 15: Use KASAN

Example output

```
BUG: KASAN: use-after-free

Read of size 8

Allocated by

↓

my_driver_probe()

Freed by

↓

my_driver_remove()

Used by

↓

IRQ Handler
```

KASAN shows:

* Allocation stack
* Free stack
* Use stack

This is extremely powerful.

---

# Step 16: Debug Deadlocks

Suppose

CPU0

```
spin_lock(A)

↓

spin_lock(B)
```

CPU1

```
spin_lock(B)

↓

spin_lock(A)
```

Both CPUs wait forever.

System hangs.

Enable

```
LOCKDEP
```

Kernel reports

```
Possible circular locking dependency
```

---

# Step 17: Debug Interrupt Problems

IRQ handler

```c
irq_handler()
{
    mutex_lock();
}
```

Illegal.

Because mutex may sleep.

IRQ cannot sleep.

Kernel prints

```
Sleeping function called from invalid context
```

---

# Step 18: Debug DMA Issues

DMA engine writes beyond the allocated buffer.

```
DMA Buffer

----------------

Allocated

----------------

DMA writes here

----------------

Kernel Object

----------------

Stack

----------------
```

Symptoms appear much later:

* Random panic
* Scheduler crash
* List corruption
* Page fault

Check:

* DMA length
* Alignment
* Cache maintenance
* `dma_sync_*()` usage
* IOMMU mappings

---

# Step 19: Debug Stack Corruption

Kernel stack is small (often 16 KB on ARM64).

Recursive functions or large local arrays can overflow it.

Bad example:

```c
void func(void)
{
    char buf[12000];
}
```

Or:

```c
void recurse(void)
{
    recurse();
}
```

Enable:

```
CONFIG_DEBUG_STACKOVERFLOW
```

---

# Step 20: Analyze vmcore

If `kdump` is enabled, a crash dump (`vmcore`) is saved.

Use:

```bash
crash vmlinux vmcore
```

Useful commands:

```text
bt          # Backtrace of current task
bt -a       # Backtrace of all tasks
ps          # List processes
kmem        # Memory usage
log         # Kernel log buffer
files       # Open files
mount       # Mounted filesystems
mod         # Loaded modules
dis         # Disassemble instructions
struct      # Inspect kernel structures
rd          # Read memory
```

---

# Complete Crash Investigation Flow

```text
                System Crash
                     │
                     ▼
      Is it Panic, Oops, Hang, or Reboot?
                     │
                     ▼
           Collect Serial Console Logs
                     │
                     ▼
            Save dmesg / pstore / vmcore
                     │
                     ▼
           Read Panic Message Carefully
                     │
                     ▼
         Examine PC, LR, SP, ESR, FAR
                     │
                     ▼
            Read the Complete Call Trace
                     │
                     ▼
      Decode PC Using addr2line/vmlinux
                     │
                     ▼
      Inspect Source Code at Crash Point
                     │
                     ▼
      Check for NULL Pointer or Bad Access
                     │
                     ▼
  Check Memory Corruption, DMA, Race, Locks
                     │
                     ▼
 Enable KASAN / LOCKDEP / ftrace if Needed
                     │
                     ▼
         Reproduce the Crash Consistently
                     │
                     ▼
              Fix and Validate the Issue
                     │
                     ▼
     Stress Test Under Load and Regression Test
```

## Interview Tips

Senior Linux kernel interviewers often ask follow-up questions after "How do you debug a crash?" such as:

* How do you distinguish a **kernel panic** from an **oops**?
* How do you decode a **call trace** manually?
* What do **PC**, **LR**, **SP**, **ESR**, and **FAR** mean on ARM64?
* How do you debug a crash when there are **no logs**?
* How do you debug a **boot-time** crash before the console is initialized?
* How do you use **ftrace**, **dynamic debug**, **kgdb**, and **kdump**?
* How do you identify **memory corruption** that causes a crash much later?
* How do you debug **interrupt-context** crashes?
* How do you differentiate between a **software bug** and a **hardware fault**?

Mastering these topics demonstrates not only familiarity with Linux internals but also a systematic approach to root-cause analysis, which is what interviewers for kernel and embedded Linux roles are typically evaluating.
