This is actually one of the **most common interview questions** for Linux Kernel/Embedded engineers at **AMD, NVIDIA, Qualcomm, Intel, Samsung, Bosch, Continental**, etc.

> **Interviewer:** *"The system reboots continuously within a fraction of a second. You don't get any kernel panic logs. How will you debug it?"*

They are testing whether you know **early boot debugging** and **crash debugging without logs**.

---

# My Debugging Approach

I divide the problem into phases.

```text
Continuous Reboot
       │
       ▼
Can I capture logs?
       │
 ┌─────┴─────────┐
 │               │
YES             NO
 │               │
Analyze      Enable Early Debug
Logs             │
                 ▼
           Narrow Down Stage
```

---

# Step 1 : Find When It Reboots

First question I ask myself:

> **Where exactly is the reboot happening?**

```text
Power ON

↓

BootROM

↓

SPL

↓

U-Boot

↓

Kernel Image

↓

Decompress Kernel

↓

start_kernel()

↓

Driver Probe

↓

Userspace

↓

Login

↓

Runtime
```

The reboot could happen anywhere.

For example:

| Stage                | Possible Cause           |
| -------------------- | ------------------------ |
| BootROM              | Hardware                 |
| U-Boot               | Bootloader bug           |
| Kernel Decompression | Corrupt Image            |
| start_kernel()       | MMU/Page table issue     |
| Driver Probe         | Driver crash             |
| init process         | RootFS issue             |
| Runtime              | Driver/Memory corruption |

First determine **which stage is failing**.

---

# Step 2 : Use UART (Serial Console)

This is the **first thing every kernel engineer does**.

If HDMI is blank, UART still prints.

Example:

```text
CPU0 Booting Linux

Starting kernel...

```

If reboot happens after

```text
Starting kernel...
```

Then

Kernel has started.

If nothing appears

Problem is before kernel.

---

# Step 3 : Enable Early Console

Sometimes normal console initializes too late.

Enable

```text
earlycon

ignore_loglevel

loglevel=8

initcall_debug

keep_bootcon
```

Bootargs

```text
console=ttyS0,115200

earlycon

ignore_loglevel

loglevel=8
```

Now kernel prints immediately after entering `start_kernel()`.

Example

```text
Boot CPU: AArch64 Processor

Kernel command line

Initializing cgroup

```

Now you know where reboot happens.

---

# Step 4 : Add Early printk()

Suppose kernel crashes before console initialization.

Add

```c
printk("Reached here\n");
```

or

```c
pr_emerg("Reached init_memory()\n");
```

at different locations.

Example

```text
start_kernel()

↓

printk A

↓

setup_arch()

↓

printk B

↓

mm_init()

↓

printk C

↓

trap_init()

↓

printk D

```

Output

```text
A

B

C

(reboot)
```

Crash is between

```text
C

↓

D
```

This is called **binary search debugging**.

---

# Step 5 : Use GPIO Toggling

Suppose UART doesn't work.

Use GPIO.

```c
gpio_set_value(DEBUG_GPIO,1);
```

Connect LED or Oscilloscope.

```text
LED ON

↓

Reached setup_arch()

LED OFF

↓

Reached mm_init()

LED ON

↓

Reached Driver Probe()

Reboot
```

Very common in embedded systems.

---

# Step 6 : JTAG Debugger

If reboot occurs before UART.

Use

* Lauterbach
* TRACE32
* J-Link
* OpenOCD

Stop CPU before reset.

Read

```text
PC

LR

SP

Registers
```

Example

```text
PC

0xffffffc010102344
```

Now decode

```bash
addr2line -e vmlinux PC
```

You immediately know source line.

---

# Step 7 : Disable Watchdog

Sometimes

System didn't crash.

Watchdog resets CPU.

Check

```text
Watchdog Timeout

↓

CPU Reset
```

Disable

```text
CONFIG_WATCHDOG=n
```

or

Disable watchdog in bootloader.

Now crash stays.

---

# Step 8 : Enable ramoops / pstore

Even after reboot

Kernel stores logs inside reserved RAM.

After reboot

```bash
cat /sys/fs/pstore/*
```

Example

```text
Kernel panic

Call Trace

Driver Probe
```

Very useful.

---

# Step 9 : Use kdump

If panic occurs

Enable

```text
kdump
```

Kernel saves

```text
vmcore
```

Analyze later

```bash
crash vmlinux vmcore
```

---

# Step 10 : Check Reset Reason Register

Many SoCs have reset status registers.

Example

```text
Power Reset

Watchdog Reset

Software Reset

Brownout

Thermal Reset
```

Example

```text
Reset Cause

0x08

↓

Watchdog
```

Immediately tells

Not software panic.

---

# Step 11 : Disable Drivers

Suppose crash occurs during probe.

Disable driver

```text
CONFIG_MY_DRIVER=n
```

Boot again.

If reboot disappears

Problem inside driver.

Then enable drivers one by one.

---

# Step 12 : Use initcall_debug

Boot parameter

```text
initcall_debug
```

Kernel prints

```text
calling

i2c_init()

returned

calling

spi_init()

returned

calling

camera_probe()

```

Output

```text
calling camera_probe()

Reboot
```

Driver found.

---

# Step 13 : Use ftrace Boot Tracing

Kernel parameter

```text
ftrace=function
```

Kernel stores

```text
Last Function

↓

Crash
```

Useful for early crashes.

---

# Step 14 : Memory Corruption

Suppose

```c
char buf[8];

strcpy(buf,"abcdefghijklmnopqrstuvwxyz");
```

Nothing happens immediately.

Later

```text
Scheduler crashes

↓

Panic

↓

Reset
```

Real bug

Occurred much earlier.

Enable

```text
KASAN

KFENCE

SLUB_DEBUG
```

---

# Step 15 : Hardware Investigation

Sometimes not software.

Check

Power rails

Oscillator

DDR

PMIC

Temperature

Brownout

Thermal reset

Voltage drop

Oscilloscope helps.

---

# Step 16 : Use Git Bisect

Suppose reboot started after new patch.

Use

```bash
git bisect
```

Automatically finds

First bad commit.

---

# Step 17 : If Absolutely No Logs

My debugging order becomes

```text
Power

↓

Clock

↓

DDR

↓

BootROM

↓

Bootloader

↓

UART

↓

Early Console

↓

printk()

↓

GPIO Toggle

↓

JTAG

↓

Watchdog

↓

pstore

↓

Crash Dump

↓

Binary Search
```

---

# Real Interview Example

### Interviewer

**System reboots within 300 ms. No panic logs. What will you do?**

### Good Answer

> "First, I would determine the stage at which the reboot occurs—whether it's in BootROM, U-Boot, kernel initialization, driver probe, or userspace. I would connect a UART console and enable `earlycon`, `ignore_loglevel`, `loglevel=8`, `keep_bootcon`, and `initcall_debug` to capture the earliest possible kernel messages. If no output appears, I would suspect a pre-kernel issue or an early CPU exception and use a JTAG debugger to halt the CPU and inspect the PC, LR, SP, and exception registers. If UART is unavailable, I would instrument the code with `printk()` or GPIO toggling to identify the last executed point. I would also verify whether the watchdog is causing the reset by checking the SoC reset-cause register and, if possible, disable the watchdog temporarily. To preserve logs across reboots, I would enable `ramoops`/`pstore` or `kdump`. If the reboot happens during driver initialization, I would use `initcall_debug` and selectively disable drivers to isolate the faulty module. Finally, if the issue started after recent code changes, I would use `git bisect` to identify the offending commit."

---

# This is the Debugging Flow Senior Kernel Engineers Follow

```text
                     Continuous Reboot
                            │
                Is UART output available?
                     ┌──────┴──────┐
                     │             │
                    Yes            No
                     │             │
       Enable earlycon/loglevel    Check Power/Clock/DDR
                     │             │
             Use initcall_debug    Use JTAG
                     │             │
         Find Last Printed Message │
                     │             │
             Driver or Kernel?     │
             ┌────────┴────────┐   │
             │                 │   │
          Driver            Core Kernel
             │                 │
      Disable Modules      Add Early printk()
             │                 │
       Reproduce Issue     Binary Search
             └────────┬────────┘
                      │
          Check Watchdog & Reset Cause
                      │
            Enable pstore/ramoops/kdump
                      │
            Analyze Crash and Fix Root Cause
```

## Interview Tip (AMD/NVIDIA/Qualcomm)

A senior interviewer is usually **not** looking for "I'll check `dmesg`." If the board reboots in a fraction of a second, there may be **no `dmesg` to read**.

The answer they expect is that you know **how to create observability** when none exists:

* Use **UART** with `earlycon`, `keep_bootcon`, `ignore_loglevel`, and `initcall_debug`.
* If that fails, use **JTAG** to stop the CPU and inspect registers.
* If JTAG isn't available, instrument the code with **early `printk()`** or **GPIO toggling** to identify the last executed code path.
* Determine whether the reset is due to a **watchdog**, **kernel panic**, or a **hardware reset** by checking the SoC reset-cause registers.
* Preserve information across reboots using **ramoops/pstore** or **kdump**.
* Narrow the fault systematically using **binary search** and by selectively disabling drivers or recent changes.

This systematic methodology is what distinguishes an experienced Linux kernel engineer from someone who only relies on crash logs.


Absolutely. This is a **Senior Linux Kernel Engineer (5–15 years)** level debugging topic. In companies like **AMD, NVIDIA, Qualcomm, Intel, Google, Meta, Bosch, Continental**, interviewers may spend **30–45 minutes** on this single scenario.

Let's go much deeper.

---

# Scenario

**Problem Statement**

```
Power ON

↓

Linux starts

↓

Within 100~500ms

↓

Board Reboots

↓

Again Boot

↓

Again Reboot

↓

Infinite Loop
```

No kernel panic.

No oops.

No serial log.

No vmcore.

No pstore.

Nothing.

Question:

> **How will you debug?**

---

# First Principle

Never assume it's a Linux kernel issue.

The reboot can happen because of:

```
Hardware

↓

BootROM

↓

SPL

↓

ATF (BL31)

↓

TEE

↓

U-Boot

↓

Kernel Image

↓

Kernel Decompression

↓

start_kernel()

↓

Scheduler

↓

Driver Probe

↓

Init Process

↓

User Space

↓

Watchdog

↓

PMIC

↓

Thermal

↓

Brownout
```

Your first job is **not debugging**.

Your first job is

> **Find the reboot stage.**

This is called

**Failure Localization**

---

# Step 1 — Is CPU Actually Resetting?

Many engineers immediately assume panic.

Wrong.

Sometimes Linux never crashed.

CPU simply reset.

Possible reset reasons

```
Watchdog

Software Reset

PMIC Reset

Thermal Reset

Brownout

External Reset Pin

Debug Reset

Power Glitch

Clock Failure
```

So first read

Reset Cause Register.

Example

```
PMIC_RESET_STATUS

0x04
```

Meaning

```
Watchdog Reset
```

Immediately

You know

Linux never panicked.

---

# Step 2 — Determine Boot Progress

Every boot stage must leave a footprint.

```
Power ON

↓

BootROM

↓

SPL

↓

ATF

↓

U-Boot

↓

Kernel Entry

↓

MMU Enable

↓

Scheduler

↓

Probe

↓

Init

↓

Shell
```

Ask

Which footprint is last visible?

Suppose UART prints

```
U-Boot 2025

Loading Kernel

Starting kernel...
```

Nothing after

Then

Linux never initialized console.

Crash is

```
Kernel Entry

↓

Console Init
```

Very important.

---

# Step 3 — Understand Kernel Boot Timeline

Most engineers don't know this.

Kernel boot sequence

```
head.S

↓

Primary CPU

↓

MMU

↓

Page Tables

↓

Caches

↓

start_kernel()

↓

setup_arch()

↓

mm_init()

↓

trap_init()

↓

sched_init()

↓

time_init()

↓

console_init()

↓

rest_init()

↓

kernel_init()

↓

driver_init()

↓

initcalls()

↓

userspace
```

Suppose reboot occurs

Before

```
console_init()
```

Normal printk

Will NEVER work.

That's why

No logs.

---

# Step 4 — Enable Early Console

Console initializes late.

Need

```
earlycon

keep_bootcon

ignore_loglevel

loglevel=8
```

Now

Kernel prints

Immediately after

```
head.S
```

instead of

after

```
console_init()
```

This alone solves many boot crashes.

---

# Step 5 — If Even Early Console Doesn't Print

Now think

Crash before

```
start_kernel()
```

Possible reasons

```
Bad Page Tables

MMU

Cache

Wrong DTB

Bad Kernel Image

CPU Exception

Wrong EL

ATF Issue
```

Now UART cannot help.

Need

JTAG.

---

# Step 6 — JTAG Investigation

Freeze CPU.

Read

```
PC

LR

SP

Current EL

SPSR

ESR

FAR

X0-X30
```

Example

```
PC

0xffffff8008081230
```

Decode

```
addr2line

↓

arch/arm64/mm/mmu.c

↓

Line 281
```

Now

You know exact line.

---

# Step 7 — Binary Search Using printk()

Suppose

No JTAG.

Kernel source available.

Add

```
printk("1");
```

```
printk("2");
```

```
printk("3");
```

```
printk("4");
```

Example

```
start_kernel()

↓

1

↓

setup_arch()

↓

2

↓

mm_init()

↓

3

↓

sched_init()

↓

4

↓

Reboot
```

Output

```
1

2

3
```

No

4

Crash

Between

```
mm_init()

↓

sched_init()
```

Now continue

```
3.1

3.2

3.3

3.4
```

Binary search.

This is exactly how kernel engineers debug.

---

# Step 8 — GPIO Debugging

Suppose

UART dead.

JTAG unavailable.

Still possible.

Connect Oscilloscope.

Toggle GPIO.

```
GPIO1 High

↓

setup_arch()

GPIO Low

↓

mm_init()

GPIO High

↓

scheduler()

Reboot
```

Oscilloscope shows

```
____----____----
```

Every edge

Represents

One function reached.

Very common

In Automotive Linux.

---

# Step 9 — Is Watchdog Resetting?

One of the biggest interview questions.

Imagine

```
CPU

↓

Deadlock

↓

Watchdog not kicked

↓

10 seconds

↓

Reset
```

Kernel never panics.

Just resets.

Need

Disable watchdog.

Or

Increase timeout.

Now

System hangs

Instead of rebooting.

Now

You can debug.

---

# Step 10 — Driver Isolation

Suppose reboot happens

During

Driver Probe.

Kernel boots

Like

```
USB

OK

↓

SPI

OK

↓

I2C

OK

↓

Camera

↓

Reboot
```

Disable

Camera driver.

Boot again.

Works.

Now

Camera isolated.

Continue

Inside probe.

---

# Step 11 — initcall_debug

One of the most powerful boot options.

Kernel prints

```
calling foo_init()

returned

calling spi_init()

returned

calling camera_probe()

```

Then

```
Reboot
```

Last initcall

Found.

---

# Step 12 — What If There Is No Log at All?

Then

Think

Hardware.

```
DDR unstable

↓

Random reboot

↓

No log
```

Check

```
DDR Training

PLL

Voltage

Clock

PMIC

Reset Pin

Oscillator
```

Kernel cannot fix

Hardware.

---

# Step 13 — Hardware Reset vs Software Reset

Many engineers confuse these.

Software reset

```
panic()

↓

emergency_restart()

↓

reboot
```

Hardware reset

```
Voltage Drop

↓

PMIC

↓

Reset Pin

↓

CPU Restart
```

Difference

Software leaves logs.

Hardware often doesn't.

---

# Step 14 — Cache Corruption

Imagine

DMA

Overwrites

Kernel Page Tables.

Crash

Occurs

Later.

Symptoms

```
Random reboot

No trace

Different location

Every boot
```

Need

```
KASAN

IOMMU

DMA Debug

Cache Validation
```

---

# Step 15 — Memory Corruption Before Console

Worst case.

```
Page Table

↓

Corrupted

↓

Exception

↓

Exception Vector

↓

Corrupted

↓

Double Fault

↓

CPU Reset
```

Kernel

Cannot even panic.

CPU resets immediately.

Only

JTAG

Can see it.

---

# Step 16 — Exception Before Vector Table

Suppose

Exception occurs

Before

Vector table initialized.

CPU

Cannot jump

To exception handler.

Result

```
Synchronous Abort

↓

No Handler

↓

Reset
```

Again

No log.

---

# Step 17 — Crash Before MMU

Suppose

```
head.S

↓

Enable MMU

↓

Wrong TTBR

↓

Translation Fault

↓

Reset
```

Need

JTAG.

Cannot use printk.

---

# Step 18 — If It Started After Your Patch

Use

```
git bisect
```

Example

```
500 commits

↓

250

↓

125

↓

62

↓

31

↓

15

↓

7

↓

3

↓

1
```

Find bad commit

Within minutes.

---

# What Senior Engineers Think

A junior engineer thinks:

```
No log

↓

Cannot debug
```

A senior kernel engineer thinks:

```
No log

↓

Why no log?

↓

Console?

↓

Watchdog?

↓

Before printk?

↓

Before MMU?

↓

Hardware reset?

↓

Exception before vectors?

↓

PMIC?

↓

JTAG?
```

The absence of logs is itself valuable information.

---

# Complete Mental Debugging Flow

```text
                           Continuous Reboot
                                  │
                    ┌─────────────┴─────────────┐
                    │                           │
           Is CPU really resetting?       Is it only hanging?
                    │
          Read Reset-Cause Register
                    │
        ┌───────────┴───────────┐
        │                       │
   Watchdog                 Power/PMIC
        │                       │
 Disable or Extend         Check Voltages
        │
        ▼
Is UART output available?
        │
   ┌────┴────┐
   │         │
  Yes        No
   │         │
Enable       JTAG / GPIO / Scope
earlycon     to locate last execution point
   │
Use initcall_debug
   │
Find last successful initcall
   │
Binary search with printk()
   │
Disable suspect drivers
   │
Enable KASAN / LOCKDEP / DMA Debug
   │
Reproduce consistently
   │
Identify root cause
   │
Fix → Stress test → Regression test
```

---

## What AMD/NVIDIA/Qualcomm Interviewers Expect

If you're interviewing for a **Senior Linux Kernel Engineer**, a strong answer is not "I'll check `dmesg`."

Instead, they expect you to reason like this:

1. **Determine the reset source** (watchdog, PMIC, software, brownout, thermal, external reset).
2. **Localize the failure stage** (BootROM, ATF, U-Boot, `head.S`, `start_kernel()`, initcalls, userspace).
3. **Create observability** using `earlycon`, `keep_bootcon`, `initcall_debug`, GPIO instrumentation, or JTAG.
4. **Narrow the failure** using binary search with early `printk()` or selective driver disabling.
5. **Verify whether it's software or hardware**, because hardware resets often leave no software evidence.
6. **Use advanced kernel tools** (`KASAN`, `LOCKDEP`, `ftrace`, `ramoops`, `kdump`) once the system stays alive long enough.

---

This topic can go even deeper into **ARM64 exception handling**, including what happens **instruction-by-instruction** from the moment a data abort occurs, how the CPU enters the exception vector, how `ESR_EL1`, `ELR_EL1`, `SPSR_EL1`, and `FAR_EL1` are populated, and why some faults produce a kernel panic while others lead to an immediate reset with no logs. That is the level of detail typically expected in senior kernel interviews.
