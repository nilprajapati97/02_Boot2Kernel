# AP[2:1] Access Permission Encoding: Complete Reference

**Category**: Access Permissions & Execute Never  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Why Access Permissions Exist

```
ARM64 page tables encode access permissions in EVERY descriptor.
These permissions control:
  - Which Exception Level can read/write the page (EL0 = user, EL1 = kernel)
  - Whether writes are allowed at all (read-only)
  - Whether code execution is allowed (separate PXN/UXN bits)

Access permissions serve two goals:
  1. Privilege isolation: kernel pages invisible/inaccessible to user space
  2. Memory protection: read-only data cannot be overwritten
     (critical for W^X: writable pages cannot be executable)
```

---

## 2. AP[2:1] Bit Field

```
AP[2:1] = bits[7:6] in page/block descriptor
(Note: bit[6] in the PTE corresponds to AP[1], bit[7] to AP[2])

AP[2:0]:
  bit[4] = AP[0]: Access Flag (handled separately — see Category 03/04)
  bit[6] = AP[1]: Access Permission bit 1
  bit[7] = AP[2]: Access Permission bit 2
  
Encoding table (for Stage 1 EL1/EL0):

  AP[2:1] | Access from EL1 | Access from EL0 (user)
  --------|-----------------|------------------------
  0b00    | Read/Write      | None (no access)
  0b01    | Read/Write      | Read/Write
  0b10    | Read-Only       | None (no access)
  0b11    | Read-Only       | Read-Only

Linux constant names:
  PTE_AP_RDWR_EL1   = AP[2:1] = 0b00  (kernel RW, user no access)
  PTE_AP_RDWR       = AP[2:1] = 0b01  (kernel RW, user RW)
  PTE_AP_RDONLY_EL1 = AP[2:1] = 0b10  (kernel RO, user no access)
  PTE_AP_RDONLY     = AP[2:1] = 0b11  (kernel RO, user RO)
  
  PAGE_KERNEL       → AP=0b00 (EL1 RW, EL0 none)
  PAGE_KERNEL_RO    → AP=0b10 (EL1 RO, EL0 none)
  PAGE_KERNEL_EXEC  → AP=0b00 + PXN=0 (EL1 RW+Exec, EL0 none)
  
  user pages (normal):
  VM_READ|VM_WRITE  → AP=0b01 (EL0 RW) + UXN=0 (executable if also VM_EXEC)
  VM_READ           → AP=0b11 (EL0 RO)
```

---

## 3. Fault Behavior on Permission Violation

```
When a CPU access violates AP permissions:
  → Permission Fault is taken (synchronous exception to EL1 or EL2)
  
ESR_EL1 encoding for permission fault:
  EC[31:26] = 0b100101 (Data Abort at current EL) or
  EC[31:26] = 0b100100 (Data Abort from lower EL)
  DFSC[5:0] = 0b0011xx (Permission Fault at level xx)
    0b001100 = Permission Fault at L0
    0b001101 = Permission Fault at L1
    0b001110 = Permission Fault at L2
    0b001111 = Permission Fault at L3 (leaf PTE, most common)
  WnR[6] = 0 if read fault, 1 if write fault

  ISV (Instruction Syndrome Valid) bit in ESR:
    If set: provides instruction-level detail (Rt register, access size)

Specific scenarios:
  User writes to kernel page (AP=0b00):
    → Data Abort to EL1, ESR shows Permission Fault L3, WnR=1
    → Linux fault handler: SIGSEGV to user process

  User executes non-executable page (UXN=1):
    → Instruction Abort from EL0 to EL1
    → ESR EC = 0b100000 (Instruction Abort from lower EL)
    → IFSC = 0b001111 (Permission Fault L3)
    → Linux: SIGSEGV (SEGV_ACCERR)

  Kernel writes to read-only kernel page (AP=0b10):
    → Data Abort at EL1
    → Linux: BUG() if RODATA protection enabled, or page fault handler
```

---

## 4. ARM64 PTE Bit Layout Summary

```
Page Table Entry (64-bit), Stage 1, EL1/EL0:

63   62 61   59 58 55 54  53  52 51        12 11  10  9  8  7  6  5  4  2  1  0
┌───┬────────┬────┬──┬───┬──────────────────┬───┬───┬────┬───┬───┬───┬───┬──┬──┐
│NG │PBHA[3:0]│SW │XN│PXN│  Output Addr[51:12]│nT │AF │SH[1:0]│AP[2]│AP[1]│NS│AttrIdx[2:0]│V │V │
└───┴────────┴────┴──┴───┴──────────────────┴───┴───┴────┴───┴───┴───┴───┴──┴──┘

Bit[0:1] = Valid bits: 0b11=Page/Block, 0b01=Block, 0b00=Fault
Bits[4:2] = AttrIndx[2:0] → MAIR_EL1 slot (memory type)
Bit[5] = NS (Non-Secure)
Bit[6] = AP[1] (access permission bit 1)
Bit[7] = AP[2] (access permission bit 2)
Bits[9:8] = SH[1:0] (shareability)
Bit[10] = AF (Access Flag)
Bit[11] = nG (not Global — ASID tagged if 1)
Bits[51:12] = Output Address (PA, IPA, or next-level table PA)
Bit[52] = GP (Guarded Page, BTI — ARMv8.5)
Bit[53] = DBM (Dirty Bit Management — ARMv8.1)
Bit[54] = UXN/XN (Unprivileged Execute Never / Execute Never)
Bit[55] = PXN (Privileged Execute Never)
Bits[58:55] = SW (software use bits: PTE_SPECIAL, PTE_DEVMAP, etc.)
Bits[62:59] = PBHA (Page Based Hardware Attributes, if TCR HWU set)
Bit[63] = nG already shown above (actually bit 11)
```

---

## 5. Linux AP Bit Usage

```
Linux defines page protection macros in:
  arch/arm64/include/asm/pgtable-prot.h
  arch/arm64/include/asm/page.h

Key constants:
  #define PTE_WRITE     (PTE_DBM)         /* writable: use DBM for HW dirty */
  #define PTE_RDONLY    (_AT(pteval_t, 1) << 7)   /* AP[2]=1 → read-only */
  #define PTE_USER      (_AT(pteval_t, 1) << 6)   /* AP[1]=1 → user accessible */

  PAGE_KERNEL:
    AP[2:1] = 0b00 (kernel RW, user no access)
    PXN=0 (kernel can execute)
    UXN=1 (user cannot execute kernel pages)

  PAGE_KERNEL_RO:
    AP[2:1] = 0b10 (kernel RO, user no access)
    PXN=0 (kernel can execute, but AP[2]=1 means read-only)
    UXN=1

  PAGE_KERNEL_EXEC:
    AP[2:1] = 0b00 (kernel RW→ actually this is kernel-exec without RO)
    PXN=0 (kernel executable)
    UXN=1 (user cannot execute)
    Note: Linux marks kernel text as both readable and executable

  User page (VM_READ|VM_WRITE|VM_EXEC):
    AP[2:1] = 0b01 (EL1 RW, EL0 RW)
    PXN=1 (kernel cannot execute user pages)
    UXN=0 (user CAN execute → this is the executable flag)

  User read-only (VM_READ):
    AP[2:1] = 0b11 (both read-only)
    UXN=0 (if also executable) or UXN=1 (data page)
```

---

## 6. Interview Questions & Answers

**Q1: What happens at the hardware level when user space tries to write to a read-only page?**

The CPU's memory management unit checks the PTE for the faulting virtual address. It finds `AP[2:1] = 0b11` (read-only) or `AP[2:1] = 0b10` (kernel read-only, no user access). Since the access is a write from EL0 (user), it violates the AP encoding. The hardware raises a **Data Abort** exception, transitioning from EL0 to EL1. The CPU stores the fault details in `ESR_EL1`: EC field indicates Data Abort from lower EL, DFSC encodes `0b001111` (Permission Fault, level 3), and `WnR=1` indicates a write fault. `FAR_EL1` contains the faulting virtual address. Linux's fault handler calls `do_page_fault()` → `handle_mm_fault()` → checks the VMA's permissions (`vma->vm_flags & VM_WRITE`). If the write is not allowed (page is genuinely read-only), Linux sends `SIGSEGV` with `si_code = SEGV_ACCERR`.

**Q2: What is the difference between AP[2:1]=0b00 and AP[2:1]=0b10 for kernel pages?**

Both `0b00` and `0b10` deny EL0 (user) access to the page — user gets a permission fault either way. The difference is for EL1 (kernel): `0b00` allows kernel read AND write, while `0b10` makes the page **kernel read-only**. Linux uses `0b10` for kernel read-only data: `__ro_after_init` sections, kernel text pages (after init), and `PAGE_KERNEL_RO` mappings. After `mark_rodata_ro()` is called during kernel boot, the kernel's `.text` and `.rodata` sections are changed to `AP[2:1]=0b10` — even the kernel itself cannot overwrite these pages. This prevents kernel exploits that write shellcode to kernel text.

---

## 7. Quick Reference

| AP[2:1] | EL1 (kernel) | EL0 (user) | Linux constant |
|---|---|---|---|
| 0b00 | Read/Write | No access | `PAGE_KERNEL` (default) |
| 0b01 | Read/Write | Read/Write | User writable page |
| 0b10 | Read-Only | No access | `PAGE_KERNEL_RO` |
| 0b11 | Read-Only | Read-Only | User read-only page |
