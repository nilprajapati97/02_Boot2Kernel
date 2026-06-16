For a **Linux Kernel Engineer interview (Qualcomm, NVIDIA, AMD, Intel, Automotive, Android BSP)**, one of the most common questions is:

> **"A system crashed. Which debugging tool will you use and when?"**

Many engineers know the tools but cannot explain **Why? When? Where? How?** and the **complete debugging flow**.

---

# Linux Kernel Crash Debugging Tools - Complete Interview Guide

## 1. Overall Debugging Flow

```text
Kernel Crash
     |
     +-------------------+
     |                   |
     V                   V
 Immediate Crash      Post-Mortem Analysis
     |                   |
 dmesg/oops          Ramdump
 printk             vmcore
 ftrace             crash
 kgdb               trace32
```

---

# 1. printk()

## Why?

Basic debugging mechanism.

Print variables, states, execution flow.

## When?

* Driver development
* Probe debugging
* Initialization failures
* Runtime issues

## Example

```c
printk(KERN_INFO "Probe Called\n");
printk(KERN_ERR "DMA allocation failed\n");
```

## Internally

```text
printk()
   |
   V
Ring Buffer
   |
   V
Console Driver
   |
   V
UART / Serial Console
```

Kernel stores messages in:

```text
kernel log buffer
```

View:

```bash
dmesg
```

---

# 2. dmesg

## Why?

View kernel messages.

## When?

After:

* Driver load failure
* Kernel warning
* Device initialization failure

## Example

```bash
dmesg | grep camera
```

Output:

```text
camera_probe failed
```

---

# 3. Dynamic Debug

## Why?

Enable debug logs without recompiling.

## When?

Production systems.

## Example

Enable:

```bash
echo 'file camera.c +p' \
> /sys/kernel/debug/dynamic_debug/control
```

---

# 4. WARN_ON()

## Why?

Catch unexpected conditions.

## Example

```c
WARN_ON(ptr == NULL);
```

Produces stack trace.

System continues.

---

# 5. BUG_ON()

## Why?

Fatal check.

## Example

```c
BUG_ON(ptr == NULL);
```

Result:

```text
Kernel BUG
Oops
Panic
```

---

# 6. Stack Dump

## Why?

Understand call flow.

## Example

```c
dump_stack();
```

Output:

```text
Call trace:
 probe
 platform_drv_probe
 really_probe
 driver_probe_device
```

---

# 7. ftrace

Most important interview topic.

## Why?

Kernel function tracing.

## When?

* Driver latency
* Scheduler debugging
* Interrupt debugging
* Performance issues

---

## Internally

Compiler inserts:

```text
mcount()
```

into every function.

```text
Function Entry
      |
      V
ftrace hook
      |
      V
Trace Buffer
```

---

## Enable

```bash
mount -t debugfs none /sys/kernel/debug

cd /sys/kernel/debug/tracing
```

Enable:

```bash
echo function > current_tracer
```

View:

```bash
cat trace
```

---

## Example Output

```text
camera_probe()
 regulator_enable()
 clk_prepare_enable()
```

---

# 8. trace-cmd

User-space interface for ftrace.

## Record

```bash
trace-cmd record
```

## View

```bash
trace-cmd report
```

---

# 9. perf

## Why?

Performance profiling.

## When?

* CPU utilization issues
* Scheduling delays
* Hot functions

---

## Example

```bash
perf top
```

Shows:

```text
20% memcpy
15% spin_lock
```

---

# 10. kgdb

Kernel debugger.

Equivalent of gdb for kernel.

---

## Why?

Live kernel debugging.

## When?

Need breakpoint support.

---

## Example

```bash
kgdbwait
```

Connect:

```bash
gdb vmlinux
```

Breakpoint:

```gdb
b camera_probe
```

---

# 11. KDB

Built-in kernel debugger.

No external gdb needed.

Enter:

```text
SysRq-g
```

Commands:

```text
bt
ps
lsmod
```

---

# 12. KASAN

Kernel Address Sanitizer.

Most important memory debugging tool.

---

## Why?

Detect:

* Use-after-free
* Buffer overflow
* Stack overflow

---

## Example

```c
char *buf=kmalloc(10,GFP_KERNEL);

buf[100]=1;
```

Output:

```text
KASAN: slab-out-of-bounds
```

---

## Internally

Adds redzones around memory.

```text
Object
+---------+
| Redzone |
+---------+
```

Illegal access detected immediately.

---

# 13. KFENCE

Low-overhead memory corruption detector.

## When?

Production builds.

Less overhead than KASAN.

---

# 14. LOCKDEP

Lock dependency checker.

## Why?

Detect deadlocks.

Example:

```text
A -> B
B -> A
```

Output:

```text
possible deadlock detected
```

---

# 15. RCU Stall Detector

## Why?

Detect stuck CPUs.

Output:

```text
RCU Stall detected
```

Common in:

* Infinite loops
* Deadlocks
* IRQ disabled too long

---

# 16. Hung Task Detector

Output:

```text
task blocked for more than 120 seconds
```

Useful for mutex deadlocks.

---

# 17. kmemleak

Memory leak detector.

Enable:

```text
CONFIG_DEBUG_KMEMLEAK
```

Scan:

```bash
echo scan > /sys/kernel/debug/kmemleak
```

Show leaks:

```bash
cat /sys/kernel/debug/kmemleak
```

---

# 18. Crash Utility

Most important post-mortem tool.

Interview favorite.

---

## Why?

Analyze vmcore after crash.

---

## Inputs

```text
vmcore
vmlinux
```

---

## Start

```bash
crash vmlinux vmcore
```

---

## Commands

### Backtrace

```bash
bt
```

### Process List

```bash
ps
```

### Memory

```bash
kmem
```

### Logs

```bash
log
```

### CPU Status

```bash
mach
```

---

# 19. Ramdump Analysis

Most common in Qualcomm.

---

## Why?

System rebooted.

Need root cause.

---

## Contains

```text
CPU Registers
Kernel Memory
Task Structures
Page Tables
```

---

## Analyze Using

* crash
* Trace32

---

# 20. Trace32

Industry standard.

Widely used in:

* Qualcomm
* NVIDIA
* Automotive

---

## Why?

Hardware-aware debugging.

Can inspect:

```text
CPU Registers
MMU
Page Tables
Tasks
Memory
```

---

## Example Commands

### Task List

```text
TASK.CONFIG
```

### Backtrace

```text
TASK.STACK
```

### Registers

```text
Register.View
```

### MMU

```text
MMU.List
```

---

# 21. JTAG

Hardware debugger.

---

## When?

System hangs before Linux boots.

Kernel never reaches console.

---

## Access

```text
CPU Registers
Memory
Bootloader
Kernel
```

---

# 22. kdump

Kernel crash dump infrastructure.

---

## Why?

Generate vmcore automatically.

---

## Flow

```text
Kernel Panic
      |
      V
kexec
      |
      V
Capture Kernel
      |
      V
vmcore
```

---

# 23. eBPF / bpftrace

Modern tracing.

Interview hot topic.

---

## Why?

Dynamic tracing without rebuilding kernel.

---

## Example

```bash
bpftrace -e 'kprobe:vfs_read { printf("read\n"); }'
```

---

# Tool Selection Matrix

| Problem               | Tool               |
| --------------------- | ------------------ |
| Driver probe failure  | printk, dmesg      |
| Boot issue            | Early printk, JTAG |
| Kernel panic          | Ramdump, Crash     |
| Memory leak           | kmemleak           |
| Use-after-free        | KASAN              |
| Buffer overflow       | KASAN              |
| Deadlock              | LOCKDEP            |
| RCU Stall             | ftrace, Trace32    |
| Scheduler issue       | ftrace, perf       |
| CPU performance       | perf               |
| Live debugging        | KGDB               |
| Production tracing    | eBPF               |
| Post-mortem analysis  | Crash Utility      |
| Qualcomm reboot issue | Ramdump + Trace32  |

---

# Interview Answer (8+ Years Experience)

If an interviewer asks:

> "Your Android/Linux system rebooted randomly. How do you debug it?"

A strong answer is:

```text
1. Check dmesg and last_kmsg.
2. Verify if a ramdump was generated.
3. Analyze vmcore/ramdump using crash utility.
4. Check panic logs, ESR, FAR, PC, LR.
5. Analyze call trace and task state.
6. Convert addresses using vmlinux and addr2line.
7. If memory corruption is suspected, enable KASAN.
8. If timing/scheduler related, use ftrace or perf.
9. For deadlocks, use LOCKDEP and hung-task detector.
10. For hardware-related failures, use Trace32/JTAG.
11. Reproduce and verify the fix.
```

This is the end-to-end debugging methodology expected from a senior Linux Kernel/BSP engineer working on ARM64 platforms such as Qualcomm Snapdragon, NVIDIA DRIVE, Jetson, Android BSP, or Automotive Linux systems.
