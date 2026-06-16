# PSTATE.PAN: Privileged Access Never (ARMv8.1)

**Category**: Access Permissions & Execute Never  
**Targeted**: ARM, Qualcomm

---

## 1. The Problem PAN Solves

```
Before PAN (ARM prior to ARMv8.1):
  When kernel code runs at EL1, it CAN directly read/write user space memory
  via the user's virtual addresses (TTBR0 is active at EL1).
  
  Vulnerability scenario:
    Kernel code has a pointer taken from user space:
      struct user_data *p = (struct user_data *)user_provided_ptr;
      p->field = sensitive_kernel_value;  // Kernel writes to arbitrary user VA
      
    If attacker crafts user_provided_ptr to point to a kernel target:
      → Not directly exploitable since kernel VA and user VA are in different ranges
      
    BUT: kernel code might accidentally dereference a user pointer without
    going through copy_from_user()/copy_to_user():
      → NULL pointer: user maps addr 0 → kernel accesses it directly
      → "NULL ptr deref to user" attack (mmap zero page then wait for kernel bug)

PAN (Privileged Access Never):
  PSTATE.PAN = 1: EL1 (kernel) CANNOT access user-space addresses (TTBR0 range)
                  Any EL1 access to TTBR0 VA space → Permission Fault
  PSTATE.PAN = 0: EL1 can access user-space (normal pre-PAN behavior)
  
  With PAN enabled: kernel must explicitly lower PAN protection
  before accessing user memory (via copy_from_user/copy_to_user).
  Accidental direct dereference of user pointer in kernel → fault → protection!
```

---

## 2. PAN Implementation in ARM64 Hardware

```
PSTATE.PAN:
  A single bit in the Processor STATE register
  Set/Clear with:
    MSR PAN, #1    → Sets PSTATE.PAN = 1 (user access forbidden)
    MSR PAN, #0    → Clears PSTATE.PAN = 0 (user access allowed)
  
  Alternatively:
    PSTATE.PAN is automatically SET by hardware on any exception to EL1
    (EL0→EL1 transitions: syscall, IRQ, page fault, etc.)
    On exception entry: PAN is set, kernel cannot access user space accidentally
    
    PSTATE.PAN is restored on ERET (return to EL0):
      The saved PSTATE (in SPSR_EL1) is restored, typically PAN=0 for EL0

  Hardware requirement:
    ID_AA64MMFR1_EL1.PAN[23:20]: 
      0b0000 = PAN not implemented
      0b0001 = PAN implemented (FEAT_PAN)
      0b0010 = PAN + ATS1E1 returns PAN fault (FEAT_PAN2)
      0b0011 = PAN + EPAN (see below, FEAT_PAN3)

  SCTLR_EL1.SPAN (Set PAN on exception):
    SPAN=0: PSTATE.PAN automatically SET when taking exception to EL1
    SPAN=1: PSTATE.PAN NOT automatically set (preserved from previous value)
    Linux sets SPAN=0 for maximum security

  EPAN (Enhanced PAN, ARMv8.7, FEAT_PAN3):
    PAN also blocks execution of user pages at EL1
    (Previously PAN only blocked data accesses; now also instruction fetch)
```

---

## 3. Linux Kernel PAN Integration

```
Detection (arch/arm64/kernel/cpufeature.c):
  arm64_has_pan() checks ID_AA64MMFR1_EL1.PAN field
  If PAN supported: enable it globally

Enabling PAN:
  head.S boot sequence:
    __cpu_setup() → sets SCTLR_EL1.SPAN = 0
    Means: every EL0→EL1 exception automatically sets PAN=1

Disabling PAN for user memory access:
  uaccess_enable() → MSR PAN, #0 (or: sets PSTATE.PAN=0)
  uaccess_disable() → MSR PAN, #1 (or: sets PSTATE.PAN=1)
  
  These are called around copy_from_user/copy_to_user:
  
  copy_from_user(to, from, n):
    uaccess_enable_privileged()    // PAN=0: allow user access
    ret = raw_copy_from_user(to, from, n);
    uaccess_disable_privileged()   // PAN=1: forbid user access again
    return ret;

  Alternative implementation using LDTR/STTR (unprivileged load/store):
    LDTR = Load UnPrivileged: behaves as if at EL0 for permission check
    STTR = Store UnPrivileged: same for stores
    If kernel uses LDTR/STTR, it can keep PAN=1 and still access user memory
    (PAN does NOT block LDTR/STTR by design!)
    This is the preferred modern implementation:
      uaccess_enable() is a NOP when LDTR/STTR used
      Maximum security: PAN never cleared, no race window

Linux CONFIG_ARM64_PAN:
  If set: enables PAN support if hardware provides it
  Kernel detects PAN capability and enables it automatically
  Fallback: without PAN hardware, software emulation via TTBR0 manipulation
```

---

## 4. PAN Violation and Fault Handling

```
When PAN=1 and kernel accesses user VA:
  Data Abort at EL1
  ESR_EL1: EC = 0b100101 (Data Abort at EL1)
  DFSC = 0b001101 (Permission Fault level 1)
  FAR_EL1 = faulting user virtual address
  
  Note: ESR does NOT have a specific "PAN fault" code
  The fault looks like a regular permission fault
  But the kernel knows PAN is enabled and this address is in user VA space
  → Stack trace, BUG() triggered
  → "Unable to handle kernel access to user memory without uaccess primitives"

Common cause of PAN fault in kernel:
  1. Driver dereferences pointer from user ioctl without copy_from_user
  2. kernel code accesses __user pointer directly (forgot annotation check)
  3. Interrupt handler uses user-space data structure pointer saved on stack
  4. struct containing __user pointer is accessed directly

Kernel sanitizer check:
  sparse(__user annotation) catches many of these at compile time
  KASAN can detect use-after-free but not PAN violations
  PAN provides RUNTIME detection of direct user pointer access

Example PAN fault scenario:
  ioctl handler receives userspace pointer:
    int my_ioctl(struct inode *inode, struct file *file, 
                 unsigned int cmd, unsigned long arg) {
      struct my_data *data = (struct my_data *)arg;  // BUG! arg is user ptr
      data->field = 0;   // FAULT: PAN=1, kernel accesses user VA
    }
    
  Fix:
    struct my_data kdata;
    if (copy_from_user(&kdata, (void __user *)arg, sizeof(kdata)))
        return -EFAULT;
    kdata.field = 0;  // Safe: kernel accesses kernel memory
```

---

## 5. PAN and Speculative Execution

```
PAN blocks speculative accesses too:
  Modern CPUs speculatively execute loads ahead of PAN check
  
  With FEAT_PAN3 (EPAN):
    PAN blocks speculative instruction fetches as well
    Previously (PAN without EPAN): speculative data loads blocked,
    but speculative instruction fetches to user space not blocked
    
  Speculation attacks:
    Spectre-v1 style: attacker trains branch predictor, kernel speculatively
    loads from user pointer before PAN check
    → PAN does NOT protect against Spectre (speculative access happens,
    just not architecturally committed)
    Defense: LFENCE equivalent (DSB+ISB) around user pointer access,
    or array_index_nospec() bounds masking

  Meltdown mitigation:
    KPTI (Kernel Page Table Isolation) prevents user processes from reading
    kernel memory via Meltdown — separate from PAN's purpose
    PAN prevents kernel from accessing user memory accidentally
    KPTI prevents user from accessing kernel memory via speculation
```

---

## 6. Interview Questions & Answers

**Q1: What is PAN and why is it important even on a system with ASLR and other mitigations?**

**PAN (Privileged Access Never)** is a CPU feature (ARMv8.1) that prevents kernel code (EL1) from directly accessing user-space virtual addresses. When `PSTATE.PAN=1`, any direct kernel access to a TTBR0 VA space address causes a Permission Fault, even though the kernel technically has higher privilege. ASLR (Address Space Layout Randomization) randomizes addresses but does not prevent access — if a kernel bug has a user pointer, ASLR just means the attacker needs to leak the address first. PAN provides a different layer: it enforces that user memory can only be accessed through explicit uaccess primitives (`copy_from_user`, `LDTR`/`STTR`). This catches an entire class of bugs where kernel code accidentally treats a user-supplied value as a kernel pointer and dereferences it. These bugs (sometimes called "NULL deref to user" or "unintended kernel data access") can be exploited to leak kernel data or cause privilege escalation even with ASLR. PAN makes them cause a predictable fault instead of silent exploitation.

---

## 7. Quick Reference

| PSTATE.PAN | Kernel can access user VA | Used when |
|---|---|---|
| 0 | YES | Before PAN (no protection) |
| 1 | NO (fault) | Default on EL1 exception entry |

| Access method | Bypasses PAN? | Safe? |
|---|---|---|
| Direct load/store to user VA | NO (fault if PAN=1) | UNSAFE |
| `copy_from_user()` with PAN=0 | N/A (PAN lowered) | SAFE (validated) |
| `LDTR`/`STTR` instructions | YES (always EL0 perms) | SAFE (by design) |

| Feature | ARMv version | What it adds |
|---|---|---|
| PAN | 8.1 | Blocks EL1 data access to user VA |
| EPAN (PAN3) | 8.7 | Also blocks EL1 speculative instruction fetch to user VA |
