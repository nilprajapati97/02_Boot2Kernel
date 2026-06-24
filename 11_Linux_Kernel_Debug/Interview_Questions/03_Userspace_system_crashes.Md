This is one of the most common **Senior Linux Embedded/Kernel interview** questions.

> **Scenario:**
> "The system boots successfully. When a userspace application starts (or when you execute it manually), the application hangs, crashes, or causes the whole system to become unstable. How do you debug it?"

The interviewer is looking for your **systematic debugging process**, not just a list of commands.

---

# Step 1: Understand the Problem

The first thing I do is classify the failure.

```text
               Run Application
                     │
     ┌───────────────┼────────────────┐
     │               │                │
  App Crash      App Hangs      System Crash
     │               │                │
 SIGSEGV        Deadlock        Kernel Panic
 SIGABRT        Infinite Loop   Watchdog Reset
 SIGBUS         Waiting Lock    OOM Killer
```

The debugging approach is different for each.

---

# Step 2: Gather Basic Information

Before touching the code, ask:

* Does it happen every time?
* Which software version?
* Which kernel version?
* Which hardware revision?
* Does it happen only on one board?
* Did it start after a recent patch?
* Can it be reproduced consistently?

If it is reproducible, debugging becomes much easier.

---

# Step 3: Observe the Process

Find the PID.

```bash
ps -ef | grep my_app

# or

pidof my_app
```

Check whether it is actually running.

---

# Step 4: Monitor System Resources

Before assuming there is a software bug, verify the system health.

```bash
top
```

or

```bash
htop
```

Look for:

```
CPU = 100%

Memory full

Swap usage

Zombie processes

Load Average
```

Example

```
PID   CPU

2345  100%
```

Immediately suspect

```
Infinite Loop

Busy Wait

Spin Loop
```

---

# Step 5: Check Memory

```bash
free -h
```

Suppose

```
RAM

99% Used
```

Maybe

```
Memory Leak

OOM

Huge Allocation
```

Check

```bash
dmesg | tail
```

Example

```
Out of memory

Killed process my_app
```

Now you know

Kernel killed it.

---

# Step 6: Check Kernel Logs

Always

```bash
dmesg -w
```

Run application

Watch logs live.

Example

```
Unable to handle NULL pointer

my_driver_ioctl()

```

Now

Application isn't faulty.

Driver crashed.

---

# Step 7: Check Journal

```bash
journalctl -f
```

Watch

```
Segmentation fault

Abort

Service Restart
```

---

# Step 8: If Application Crashes

Example

```
Segmentation Fault
```

Enable core dump

```bash
ulimit -c unlimited
```

Run again.

Now

```
core.1234
```

generated.

---

# Step 9: Analyze Core Dump

Use

```bash
gdb ./my_app core.1234
```

Inside

```gdb
bt
```

Example

```
main()

↓

camera_init()

↓

open_device()

↓

NULL Pointer
```

Backtrace immediately tells

where crash happened.

Useful commands

```gdb
bt
info locals
info args
frame
list
print variable
thread apply all bt
```

---

# Step 10: Trace System Calls

Very powerful.

```bash
strace ./my_app
```

Suppose

```
open()

read()

ioctl()

read()

read()

read()

```

Stops at

```
ioctl()
```

Driver likely blocked.

Or

```
poll()

poll()

poll()

```

Application waiting forever.

---

# Step 11: Trace Library Calls

Use

```bash
ltrace ./my_app
```

Useful for

```
malloc()

printf()

dlopen()

pthread

fopen()
```

Find

Library failure.

---

# Step 12: Attach GDB to Running Process

Suppose application hangs.

```bash
gdb -p PID
```

Then

```
bt
```

Example

```
pthread_mutex_lock()

↓

waiting
```

Deadlock.

---

# Step 13: Check All Threads

Inside GDB

```gdb
info threads
```

Example

```
Thread 1

Waiting Mutex

Thread 2

Holding Mutex
```

Deadlock found.

Then

```gdb
thread apply all bt
```

Shows every thread's backtrace.

---

# Step 14: Use pstack

Without full GDB

```bash
pstack PID
```

Example

```
Thread1

↓

read()

Thread2

↓

poll()

Thread3

↓

pthread_mutex_lock()
```

---

# Step 15: Use perf

If CPU usage

100%

```bash
perf top
```

or

```bash
perf record ./my_app

perf report
```

Output

```
90%

calculate_crc()

```

Now optimize

calculate_crc().

---

# Step 16: Check Memory Leak

```bash
valgrind --leak-check=full ./my_app
```

Output

```
Definitely Lost

128 KB
```

Shows

Allocation line

Never freed.

---

# Step 17: Detect Invalid Memory Access

Valgrind also detects

```
Invalid Read

Invalid Write

Use After Free

Double Free
```

Example

```
Invalid write

camera.c:120
```

---

# Step 18: AddressSanitizer (ASan)

Compile

```bash
-g -fsanitize=address
```

Run

```
Heap Buffer Overflow

camera.c

Line 210
```

ASan is faster than Valgrind and excellent for development.

---

# Step 19: Undefined Behavior Sanitizer (UBSan)

Compile

```bash
-fsanitize=undefined
```

Detects

```
Integer Overflow

Shift Overflow

NULL

Misaligned Access
```

---

# Step 20: Thread Sanitizer (TSan)

Compile

```bash
-fsanitize=thread
```

Detects

```
Race Condition

Mutex Issue

Thread Synchronization Bug
```

---

# Step 21: If Application Talks to Driver

Suppose

```
Application

↓

ioctl()

↓

Kernel Driver

↓

Crash
```

Need

```
strace

+

dmesg

+

ftrace

+

dynamic_debug
```

Check

```
Which ioctl()

Which command

Arguments

Driver response
```

---

# Step 22: If Application Hangs in read()

Example

```
read(fd,...)
```

Never returns.

Check

Driver

```
wake_up()

completion

wait_event()

interrupt
```

Maybe

Wakeup never happens.

---

# Step 23: If Poll Never Returns

Application

```
poll()
```

Driver must

```
poll_wait()

wake_up_interruptible()
```

Missing wakeup

Application hangs forever.

---

# Step 24: Observe Scheduling

Use

```bash
perf sched
```

or

```bash
trace-cmd record
```

Check

```
Thread sleeping

Scheduler

Context Switch
```

---

# Step 25: ftrace

Enable

```
function_graph
```

Now

```
Application

↓

ioctl

↓

Driver

↓

Interrupt

↓

Completion
```

Whole flow visible.

---

# Step 26: System Call Flow

Suppose

Application

```c
read(fd,buf,100);
```

Internally

```
User

↓

glibc

↓

syscall

↓

Kernel

↓

VFS

↓

Driver

↓

Hardware

↓

Interrupt

↓

Driver

↓

Copy_to_user()

↓

User
```

Need

Know

Where

It stopped.

---

# Complete Debug Flow

```text
               Application Problem
                      │
        ┌─────────────┼─────────────┐
        │             │             │
      Crash         Hang       High CPU
        │             │             │
      Core Dump      GDB         perf
        │             │             │
       gdb bt      thread bt    perf top
        │             │             │
    Segfault?     Deadlock?   Busy Loop?
        │             │             │
     strace       pstack      Flame Graph
        │             │             │
        └─────────────┼─────────────┘
                      │
                 Driver Interaction?
                      │
                Yes           No
                 │             │
     dmesg + ftrace      ASan/Valgrind
                 │
          Root Cause Analysis
                 │
          Fix → Test → Stress Test
```

# Interview Answer (Senior Level)

If asked in an interview, I would answer like this:

> "I first classify whether the problem is an application crash, an application hang, or a system crash. If it's a crash, I enable core dumps and analyze them with GDB using `bt`, `info threads`, and `thread apply all bt`. If it's a hang, I attach GDB to the running process or use `pstack` to identify where the threads are blocked. I monitor the system with `top`/`htop` and `free` to check CPU and memory usage, and I watch `dmesg -w` and `journalctl -f` for kernel-side errors. I use `strace` to identify the last system call before the failure and `ltrace` if I suspect library issues. For performance problems, I use `perf` to identify CPU hotspots. For memory issues, I use AddressSanitizer or Valgrind to detect invalid accesses and leaks. If the application communicates with a kernel driver, I correlate the `strace` output with kernel logs, enable `dynamic_debug` or `ftrace`, and inspect the driver's code path to determine whether the issue is in userspace or the kernel."

---

This is the methodology expected from experienced Embedded Linux and Kernel engineers because it **starts with symptom classification, gathers evidence at both userspace and kernel levels, and then uses the appropriate tool for each type of failure rather than jumping directly to the code.**
