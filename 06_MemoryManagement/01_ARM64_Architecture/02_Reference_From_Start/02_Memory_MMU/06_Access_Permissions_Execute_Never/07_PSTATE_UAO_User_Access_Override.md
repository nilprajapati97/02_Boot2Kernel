# PSTATE.UAO: User Access Override (ARMv8.2)

**Category**: Access Permissions & Execute Never  
**Targeted**: ARM, Qualcomm

---

## 1. UAO Background and Motivation

```
UAO (User Access Override):
  ARMv8.2 feature (FEAT_UAO)
  PSTATE.UAO bit: single bit in Processor State

Problem it addresses:
  With PAN enabled (ARMv8.1), kernel cannot directly access user memory.
  To access user memory, kernel must either:
    (a) Temporarily clear PAN (risky race window) — uaccess_enable/disable
    (b) Use LDTR/STTR (unprivileged load/store) instructions

  But LDTR/STTR have limitations:
    - Only available for regular loads/stores
    - Cannot use LDTR/STTR for atomic operations
    - Copy routines that use SIMD/NEON instructions cannot use LDTR/STTR variants
    - String copy (LDP/STP for performance) has no LDTR/LDTP equivalent

UAO solution:
  PSTATE.UAO = 1: ALL load/store instructions (LDR, STR, LDP, STP,
                  NEON VLDM, VSTM, etc.) behave as unprivileged accesses
                  → They use EL0 permission checks (not EL1)
                  → PAN=1 does NOT block them (PAN only blocks EL1-privileged)
                  
  PSTATE.UAO = 0: Normal EL1 behavior
```

---

## 2. UAO Technical Details

```
When PSTATE.UAO = 1:
  ALL memory access instructions at EL1 that would normally use EL1 permissions
  instead use EL0 permissions (as if executing at EL0).
  
  Specifically:
    LDR, LDRB, LDRH, LDR (64-bit), LDP, LDXR, etc.
      → Use EL0 permissions (TTBR0 region with user permissions)
    STR, STRB, STRH, STP, STXR, etc.
      → Use EL0 permissions
    SIMD/NEON loads/stores (LDR Q, etc.)
      → Use EL0 permissions
    Atomic instructions (CAS, LDADD, etc.)
      → Use EL0 permissions

Effect:
  Kernel at EL1 with UAO=1 accessing user VA:
    AP[2:1] = 0b01 (user RW) → access allowed (EL0 check: pass)
    AP[2:1] = 0b00 (kernel RW, user no access) → FAULT (EL0 check: fail!)
  
  This means: kernel can access user pages (as EL0 would)
              but CANNOT access kernel-only pages (AP=0b00)
              using UAO — perfect for uaccess primitives!

PAN interaction:
  PAN=1: blocks EL1 privileged (non-UAO) accesses to user VA
  UAO=1: makes ALL accesses use EL0 permissions
  
  Together: PAN=1 + UAO=1:
    EL1 can ONLY access memory with EL0 permissions (user pages)
    Cannot access kernel-only pages (AP=0b00) — blocked by EL0 permission check
    This is equivalent to "acting as EL0 for memory access purposes"

Hardware requirement:
  ID_AA64MMFR2_EL1.UAO[7:4] = 0b0001: UAO implemented
```

---

## 3. Linux UAO Implementation

```
Linux uses UAO to implement efficient uaccess:

When kernel needs to access user memory (copy_from_user path):
  1. Set PSTATE.UAO = 1 (now all accesses use EL0 permissions)
  2. Execute LDP/STP/NEON copy sequence (no LDTR/STTR needed)
  3. Clear PSTATE.UAO = 0 (return to normal EL1 behavior)

vs. old approach without UAO:
  1. Clear PSTATE.PAN = 0 (allow user access)
  2. Execute LDP/STP sequence
  3. Set PSTATE.PAN = 1 (re-enable protection)
  
  Difference: UAO keeps PAN=1 always, just changes permission model
  for the duration → slightly safer (narrower window)

Linux implementation (arch/arm64/lib/copy_from_user.S):
  uaccess_enable_privileged():
    If UAO available: MSR UAO, #1
    If UAO not available: MSR PAN, #0 (fallback: clear PAN)
  
  The actual copy uses LDP x4, x5, [src], #16 etc.
    With UAO=1: these LDP instructions use EL0 permissions → user access OK
    With PAN=0 (fallback): permissions already open → user access OK
  
  uaccess_disable_privileged():
    If UAO: MSR UAO, #0
    If no UAO: MSR PAN, #1

Syscall exception entry (arch/arm64/kernel/entry.S):
  kernel_entry macro:
    mrs x22, spsr_el1
    // PSTATE.UAO is restored from SPSR_EL1 on exception return
    // On exception entry from EL0: UAO=0 in saved SPSR (EL0 doesn't have UAO)
    // So after exception to EL1: UAO=0 automatically (no manual clear needed)
```

---

## 4. UAO vs LDTR/STTR: When to Use Which

```
LDTR/STTR (Unprivileged Load/Store):
  Specific instruction variants: LDTR, LDTRB, LDTRH, LDTRSB, LDTRSH, LDTRSW
                                  STTR, STTRB, STTRH
  - Always use EL0 permissions regardless of PAN/UAO settings
  - Limited to 32/64-bit integer register accesses
  - No LDTR for NEON/SIMD registers
  - No LDTR for pair (LDP) operations → slower for bulk copy
  - No atomic LDTR variants (LDAXR, STLXR)

LDTR advantage: very targeted — only ONE instruction behaves as unprivileged
LDTR disadvantage: cannot use advanced copy optimizations

UAO (User Access Override):
  Changes the permission model for ALL instructions at EL1
  → LDP/STP, NEON VLD/VST, CAS, LDADD — all become unprivileged
  → Can use the same optimized memcpy/memmove routines as user space!
  
UAO advantage: full instruction set access with user-space permissions
UAO disadvantage: affects ALL instructions during the window → broader scope
                 Must be carefully cleared before returning to kernel mode

Linux choice:
  If UAO available (ARMv8.2+): use UAO for copy_from/to_user
  If only PAN available (ARMv8.1): use LDTR/STTR where possible
  If neither: rely on AP bits (user pages accessible from EL1 directly)
              + KASAN/software checks for safety
```

---

## 5. Security Analysis

```
UAO window duration:
  uaccess_enable(): sets UAO=1 or clears PAN
  [copy operation]: 10–1000 instructions depending on copy size
  uaccess_disable(): clears UAO=0 or sets PAN=1
  
  During this window: kernel can access user memory
  An interrupt or exception during this window:
    → Exception handler also runs with UAO=1!
    → Security concern: exception handler might accidentally access user memory
    
  Mitigation:
    PSTATE is saved on exception entry to SPSR_EL1
    Exception handler at EL1 has its OWN copy of PSTATE
    Actually UAO is NOT saved/restored like PAN on exception entry
    UAO state carries into the exception handler → potential issue
    
    Linux handles: exception entry macro clears UAO=0 unconditionally:
      kernel_entry saves PSTATE but forces UAO=0 for the handler
      (via SPSR manipulation or explicit MSR UAO, #0)

Nested exception security:
  scenario: user -> syscall (EL0 -> EL1), UAO enabled for copy_from_user
  IRQ fires during copy → nested exception, EL1 handler runs
  Linux: forces UAO=0 in nested handler entry → safe
  On ERET from nested: restores original UAO=1 → copy continues safely
```

---

## 6. Interview Questions & Answers

**Q1: What problem does UAO solve that PAN alone cannot, and why does it matter for performance?**

PAN (ARMv8.1) prevents kernel code from directly accessing user memory, but when kernel NEEDS to access user memory (e.g., `copy_from_user`), it must temporarily disable PAN or use `LDTR`/`STTR` instructions. The limitation: `LDTR`/`STTR` are single-register, 64-bit-max instructions with no SIMD/NEON variants. This means high-performance bulk copies using `LDP`/`STP` or NEON `VLD`/`VST` instructions (which are typically 2–4× faster for bulk copies) CANNOT be used safely with PAN=1. The kernel had to choose between security (PAN=1) and performance (NEON copy).

**UAO** (ARMv8.2) solves this: setting `PSTATE.UAO=1` makes ALL memory instructions at EL1 behave as if executing at EL0 (use EL0 permissions). With `PAN=1` and `UAO=1`, the kernel can use `LDP`/`STP`/NEON instructions for fast bulk copies from user space, while maintaining PAN protection between uaccess windows. Performance matters because `copy_from_user` is called on every `read()`, `write()`, `recv()`, `send()` syscall — it's one of the highest-frequency kernel paths, often called millions of times per second on a busy server.

---

## 7. Quick Reference

| State | PAN | UAO | Kernel can access user VA? | Via which instructions? |
|---|---|---|---|---|
| Default | 1 | 0 | NO | — |
| uaccess (LDTR) | 1 | 0 | YES (LDTR/STTR only) | LDTR, STTR |
| uaccess (UAO) | 1 | 1 | YES (all instructions) | LDR, STR, LDP, NEON... |
| Pre-PAN | 0 | 0 | YES (all) | All (unsafe) |

| Feature | ARMv | What it provides |
|---|---|---|
| PAN | 8.1 | EL1 cannot accidentally access user VA |
| UAO | 8.2 | EL1 uses EL0 perms for all accesses (intentional user access) |
| EPAN | 8.7 | PAN also blocks speculative instruction fetches to user VA |
