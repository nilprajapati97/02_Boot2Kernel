# Linux Kernel Crash Analysis – Complete Interview Guide

## 1. What is a Kernel Crash?

A kernel crash occurs when the Linux kernel encounters a fatal error from which it cannot safely recover.

Common crash outputs:

```bash
Kernel panic - not syncing
Oops: Unable to handle kernel NULL pointer dereference
BUG: unable to handle kernel paging request
Watchdog detected hard LOCKUP
Soft lockup detected
RCU Stall detected
```

When a crash occurs:

```text
User Space
   ↓
System Call
   ↓
Kernel
   ↓
Exception
   ↓
Oops / Panic
   ↓
System Halt or Reboot
```

---

# 2. Difference Between Oops and Panic

| Oops                            | Panic                |
| ------------------------------- | -------------------- |
| Recoverable                     | Non-recoverable      |
| Current task killed             | Entire system halted |
| Kernel may continue             | Kernel stops         |
| Debugging information generated | System reboot/hang   |

Example:

```bash
BUG: unable to handle kernel NULL pointer dereference
```

This is Oops.

Example:

```bash
Kernel panic - not syncing: Fatal exception
```

This is Panic.

---

# 3. Most Common Reasons for Kernel Crash

## 1. NULL Pointer Dereference

Most common interview question.

### Example

```c
struct device *dev = NULL;

dev->driver = drv;
```

Crash:

```bash
Unable to handle kernel NULL pointer dereference
```

### How to Debug

```bash
bt
list
```

using crash utility.

---

## 2. Invalid Memory Access

### Example

```c
char *ptr = kmalloc(10, GFP_KERNEL);

ptr[100] = 1;
```

Result:

```bash
Kernel paging request
```

---

## 3. Use After Free

### Example

```c
ptr = kmalloc(100,GFP_KERNEL);

kfree(ptr);

ptr[0] = 1;
```

Crash due to accessing freed memory.

### Detection

```bash
CONFIG_KASAN
```

Output:

```bash
BUG: KASAN: use-after-free
```

---

## 4. Double Free

```c
kfree(ptr);
kfree(ptr);
```

Crash:

```bash
BUG: Bad page state
```

---

## 5. Stack Overflow

Kernel stack size:

### ARM64

```text
16KB
```

### Example

```c
void func(void)
{
    char buf[20000];
}
```

Crash:

```bash
Kernel stack overflow
```

---

## 6. Infinite Recursion

```c
void func(void)
{
    func();
}
```

Eventually:

```bash
Kernel stack overflow
```

---

## 7. Deadlock

### Example

Thread A

```c
mutex_lock(&lock1);
mutex_lock(&lock2);
```

Thread B

```c
mutex_lock(&lock2);
mutex_lock(&lock1);
```

Result:

```text
Deadlock
```

System hangs.

---

## 8. Soft Lockup

CPU stuck in kernel.

### Example

```c
while(1)
{
}
```

Output:

```bash
BUG: soft lockup
```

---

## 9. Hard Lockup

Interrupts disabled forever.

```c
local_irq_disable();

while(1)
{
}
```

Output:

```bash
Watchdog detected HARD LOCKUP
```

---

## 10. RCU Stall

Frequently asked by Qualcomm/NVIDIA.

### Example

```c
preempt_disable();

while(1)
{
}
```

Output:

```bash
INFO: rcu_sched detected stalls
```

---

## 11. Memory Corruption

### Example

```c
memcpy(dst, src, 1000);
```

while destination is:

```c
char dst[10];
```

Results:

```text
Random crashes
Kernel panic
Page faults
```

Hardest issue to debug.

---

## 12. DMA Corruption

Very common in Qualcomm and NVIDIA.

### Example

```c
DMA engine writes beyond allocated buffer
```

Results:

```text
Kernel panic
Random memory corruption
```

---

## 13. Driver Probe Failure

Most common BSP interview topic.

### Example

```c
static int my_probe(...)
{
    ptr = devm_kzalloc(...);

    ptr->member = 1;
}
```

If allocation fails:

```bash
NULL pointer dereference
```

---

## 14. Interrupt Handler Crash

### Example

```c
irq_handler()
{
    NULL->member = 1;
}
```

Crash inside IRQ context.

---

## 15. Workqueue Crash

```c
queue_work(...)
```

Worker accesses invalid pointer.

Crash later.

Difficult because failure occurs asynchronously.

---

# 4. Commands Used During Kernel Crash Analysis

## dmesg

```bash
dmesg
```

Shows kernel logs.

---

## journalctl

```bash
journalctl -k
```

Kernel logs.

---

## crash Utility

Most important for Qualcomm/NVIDIA.

Open vmcore:

```bash
crash vmlinux vmcore
```

---

### bt

Backtrace

```bash
crash> bt
```

Shows call stack.

---

### ps

Process list

```bash
crash> ps
```

---

### files

```bash
crash> files
```

---

### vm

Memory usage

```bash
crash> vm
```

---

### kmem

Memory analysis

```bash
crash> kmem
```

---

### log

Kernel log

```bash
crash> log
```

---

### dis

Disassembly

```bash
crash> dis function_name
```

---

# 5. Kernel Panic Debugging Flow

```text
Kernel Panic
      │
      ▼
Collect dmesg
      │
      ▼
Collect vmcore
      │
      ▼
Open in crash utility
      │
      ▼
Analyze Backtrace
      │
      ▼
Identify Faulting Function
      │
      ▼
Review Source Code
      │
      ▼
Find Root Cause
```

---

# 6. Qualcomm Specific Crash Reasons

## RPMh Driver Issues

```text
Power collapse failure
```

---

## SMMU Fault

Output:

```bash
Unhandled context fault
```

Cause:

```text
Invalid DMA mapping
```

---

## Remote Processor Crash

```bash
Subsystem Restart
SSR Triggered
```

Examples:

```text
ADSP
CDSP
MODEM
```

---

## TrustZone Communication Failure

```bash
SCM call timeout
```

---

## IPC Router Crash

```bash
GLINK failure
```

---

# 7. NVIDIA Specific Crash Reasons

## GPU Driver Crash

```bash
NVRM: Xid Error
```

Most common.

---

## IOMMU Fault

```bash
Unhandled IOMMU fault
```

---

## NvMap Memory Corruption

```text
GPU buffer corruption
```

---

## Camera Driver Crash

```text
CSI
VI
ISP
```

DMA buffer issues.

---

## Display Driver Crash

```text
DRM
Framebuffer
Display Pipeline
```

---

# 8. Crash Analysis Interview Questions

### Q1

What information do you collect after kernel panic?

Answer:

```text
1. dmesg
2. vmcore
3. vmlinux
4. Backtrace
5. System state
```

---

### Q2

How do you debug NULL pointer dereference?

Answer:

```text
1. Find fault address
2. Get backtrace
3. Identify NULL object
4. Verify allocation path
5. Verify probe/init sequence
```

---

### Q3

Difference between Soft Lockup and Hard Lockup?

Soft Lockup:

```text
Task stuck
CPU still servicing interrupts
```

Hard Lockup:

```text
CPU not servicing interrupts
```

---

### Q4

How do you analyze vmcore?

```bash
crash vmlinux vmcore
```

Commands:

```bash
bt
ps
log
kmem
vm
```

---

### Q5

Most common driver bugs causing kernel panic?

```text
NULL Pointer Dereference
Use After Free
Double Free
Buffer Overflow
DMA Corruption
Deadlock
Stack Overflow
Race Conditions
```

---

# 9. Senior Engineer (8+ Years) Expected Answer

If interviewer asks:

### "You got kernel panic on Qualcomm/NVIDIA platform. How do you proceed?"

Answer:

```text
1. Capture panic log.

2. Check panic type:
   - NULL pointer
   - Page fault
   - Watchdog
   - RCU stall
   - SMMU fault

3. Collect vmcore.

4. Open vmcore using crash utility.

5. Analyze:
   bt
   ps
   log
   kmem

6. Identify faulting driver/module.

7. Verify:
   Memory allocation
   DMA mapping
   Locking sequence
   Interrupt handling
   Probe path

8. Reproduce issue.

9. Use KASAN/KMEMLEAK/LOCKDEP/FTRACE.

10. Root-cause and fix.
```

---

# Commands Every Kernel Engineer Must Know

```bash
dmesg
journalctl -k

cat /proc/kallsyms
cat /proc/interrupts
cat /proc/meminfo

echo t > /proc/sysrq-trigger
echo w > /proc/sysrq-trigger
echo l > /proc/sysrq-trigger

crash vmlinux vmcore

bt
ps
kmem
vm
log

addr2line
objdump
gdb

ftrace
trace-cmd
perf
kgdb
kdb
```

For Qualcomm and NVIDIA interviews, focus heavily on:

1. NULL Pointer Dereference Analysis
2. Page Fault Analysis
3. SMMU/IOMMU Fault Debugging
4. DMA Corruption
5. RCU Stall
6. Soft/Hard Lockup
7. vmcore + crash utility debugging
8. KASAN, LOCKDEP, KMEMLEAK
9. Driver Probe Crash Analysis
10. Interrupt Context Crashes

These topics account for a large portion of senior BSP/Kernel Engineer interview discussions.
