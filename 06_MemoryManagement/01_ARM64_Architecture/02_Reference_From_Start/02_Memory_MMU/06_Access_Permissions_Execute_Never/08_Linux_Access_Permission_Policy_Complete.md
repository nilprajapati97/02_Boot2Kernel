# Linux Access Permission Policy: End-to-End Page Protection

**Category**: Access Permissions & Execute Never  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Complete Linux Permission Model

```
Linux uses the full ARM64 permission machinery to implement:
  1. Kernel/user isolation (AP[2:1])
  2. Execute-never for data pages (PXN/UXN)
  3. W^X enforcement (no page writable AND executable)
  4. Accidental kernel→user access prevention (PAN)
  5. DMA/device page restrictions
  6. KPTI (separate page tables for kernel/user mode)

This document synthesizes all permission concepts into a unified view:
  How is each type of page mapped in a running Linux system?
```

---

## 2. Page Protection Constant Summary

```
Linux page protection constants (arch/arm64/include/asm/pgtable-prot.h):

PAGE_KERNEL:
  PTE = AP[2:1]=0b00 | PXN=1 | UXN=1 | AttrIndx=MT_NORMAL | SH=ISH
  EL1: Read/Write   EL0: No access   Exec: Neither
  Used for: kernel .data, .bss, per-CPU areas, struct page arrays

PAGE_KERNEL_RO:
  PTE = AP[2:1]=0b10 | PXN=1 | UXN=1 | AttrIndx=MT_NORMAL | SH=ISH
  EL1: Read-only    EL0: No access   Exec: Neither
  Used for: kernel .rodata, __ro_after_init data

PAGE_KERNEL_EXEC:
  PTE = AP[2:1]=0b10 | PXN=0 | UXN=1 | AttrIndx=MT_NORMAL | SH=ISH
  EL1: Read-only+Execute   EL0: No access
  Used for: kernel .text (after mark_rodata_ro())

PAGE_KERNEL_ROX = PAGE_KERNEL_EXEC (same bits)

PAGE_KERNEL_EXEC (during boot, before mark_rodata_ro):
  PTE = AP[2:1]=0b00 | PXN=0 | UXN=1 | AttrIndx=MT_NORMAL | SH=ISH
  EL1: Read/Write+Execute  (temporarily, before lockdown)
  Only valid during kernel initialization phase

PAGE_SHARED (user anonymous/file-backed, default):
  PTE = AP[2:1]=0b01 | PXN=1 | UXN=1 | AttrIndx=MT_NORMAL | SH=ISH
  EL1: Read/Write   EL0: Read/Write   Exec: Neither (UXN=PXN=1)
  Used for: user heap, stack, writable data mappings (mmap PROT_READ|PROT_WRITE)

PAGE_SHARED_EXEC (user text):
  PTE = AP[2:1]=0b01 | PXN=1 | UXN=0 | AttrIndx=MT_NORMAL | SH=ISH
  EL1: Read/Write   EL0: Read/Write+Execute
  Wait — Linux uses read-only for executable pages:
  
  PAGE_COPY_EXEC (user text, most common):
  PTE = AP[2:1]=0b11 | PXN=1 | UXN=0 | AttrIndx=MT_NORMAL | SH=ISH
  EL1: Read-only    EL0: Read-only+Execute
  Used for: user .text sections, shared library code

PAGE_READONLY (user read-only data):
  PTE = AP[2:1]=0b11 | PXN=1 | UXN=1 | AttrIndx=MT_NORMAL | SH=ISH
  EL1: Read-only    EL0: Read-only   Exec: Neither
  Used for: mmap(PROT_READ), shared read-only file mappings
```

---

## 3. Permission Lifecycle for a User Process

```
Process creation (fork/exec):

1. exec() sets up new address space:

   .text from ELF file (LOAD segment with X):
     → mmap(PROT_READ|PROT_EXEC): VM_READ|VM_EXEC flags
     → PTE: AP=0b11 (RO), PXN=1, UXN=0
     → Read-only executable

   .data from ELF file (LOAD segment with WR):
     → mmap(PROT_READ|PROT_WRITE): VM_READ|VM_WRITE flags
     → PTE: AP=0b01 (user RW), PXN=1, UXN=1
     → Writable, not executable

   .bss (anonymous zeroed pages):
     → mmap(PROT_READ|PROT_WRITE, MAP_ANON): VM_READ|VM_WRITE
     → Initially: PTE not present (demand paging)
     → On first access page fault → allocate, zero-fill
     → PTE: AP=0b01, PXN=1, UXN=1

   Stack (VM_GROWSDOWN):
     → mmap(PROT_READ|PROT_WRITE, MAP_ANON|MAP_STACK)
     → PTE: AP=0b01, PXN=1, UXN=1
     → NX on stack (UXN=1): prevents stack shellcode

2. During execution:
   CoW page fault on shared read-only page:
     → __do_page_fault → handle_pte_fault → do_cow_fault
     → Allocate new page, copy content
     → New PTE: AP=0b01 (user RW), same NX bits
     
   mprotect(PROT_READ|PROT_EXEC) for JIT:
     → change_protection → change_pte_range
     → AP[2]=1 (RO), UXN=0 (executable)
     → TLB flush: TLBI VAE1IS
   
   madvise(MADV_DONTNEED):
     → zap_page_range → PTEs cleared
     → On re-access: fresh zero page allocated

3. Context switch:
   TTBR0_EL1 switched to new process's pgd
   ASID changed atomically (write new ASID+BADDR in one 64-bit store)
   TLBI ASIDE1IS if ASID reused from different mm (ASID rollover)
```

---

## 4. Permission Verification Across Access Types

```
Complete access verification matrix for ARM64:

  Scenario: CPU at EL0 reads from VA in TTBR0 region
  PTE lookup → check:
    1. Valid bit (bits[1:0] = 0b11)?
    2. AP[2:1]: EL0 read allowed?
       - AP=0b00: NO (EL0 no access) → permission fault
       - AP=0b01/11: YES (EL0 can read)
    3. Access Flag (bit[10]): if 0, AF fault (set AF, continue or trap)
    4. Dirty bit: for reads, don't care

  Scenario: CPU at EL0 writes to VA
  PTE lookup → check:
    1. Valid?
    2. AP[2:1]: EL0 write allowed?
       - AP=0b00: NO → permission fault
       - AP=0b01: YES (user RW)
       - AP=0b11: NO (read-only) → permission fault
       - AP=0b10: NO (EL0 no access) → permission fault
    3. DBM: if AP[2]=1 but DBM=1, hardware may auto-convert to writable

  Scenario: CPU at EL0 executes instruction from VA
  I-cache fetch → PTE check:
    1. Valid?
    2. UXN bit: UXN=1 → instruction abort (permission fault)
    3. AP[2:1]: user must have at least READ access (to read instruction)
    4. No specific "execute" AP bit — controlled by UXN only

  Scenario: CPU at EL1 writes to VA in TTBR1 region
  PTE → check:
    1. Valid?
    2. AP[2:1]: EL1 write allowed?
       - AP=0b00: YES (EL1 RW)
       - AP=0b10: NO (EL1 RO) → permission fault
    3. PAN=1: this is TTBR1 range (kernel VA) → PAN doesn't apply to EL1's own range
       PAN only blocks EL1 accesses to TTBR0 range

  Scenario: CPU at EL1 executes from TTBR0 range (user VA):
    PAN=1: BLOCKS DATA access (fault)
    PXN: if PXN=1 on user pages → blocks INSTRUCTION fetch at EL1
    Both PAN and PXN protect against kernel executing user code
```

---

## 5. Device and Special Pages

```
Device MMIO pages:
  AttrIndx = MT_DEVICE_nGnRnE (index 0)
  AP[2:1] = 0b00 (kernel RW, user no access)
  PXN = 1, UXN = 1 (no execution from device registers)
  SH = any (SH ignored for Device memory architecturally)
  
  ioremap() default: AP=0b00, PXN=PXN=1, MT_DEVICE_nGnRnE

MMIO mapped for user space (UIO, vfio):
  AP[2:1] = 0b01 (user RW) — explicitly user-mapped device
  PXN = 1, UXN = 1 (no execution from MMIO)
  MT_DEVICE_nGnRnE

DMA coherent buffers (non-coherent SoC):
  AP[2:1] = 0b01 (user may need access) or 0b00 (kernel-only)
  PXN = 1, UXN = 1
  AttrIndx = MT_NORMAL_NC (non-cacheable)
  
KPTI trampoline page:
  AP[2:1] = 0b10 (read-only, accessible from both EL1 and EL0)
  PXN = 0 (kernel can execute trampoline)
  UXN = 1 (user cannot execute)
  nG = 0 (global — same in both TTBR0 and TTBR1 page tables)
  Used for: exception return path from EL1 to EL0
```

---

## 6. Interview Questions & Answers

**Q1: Walk through all the permission checks that occur when a user space program calls read() on a file, from the syscall entry to the data copy.**

1. **Syscall entry**: User code at EL0 executes `SVC #0`. CPU transitions to EL1. `PSTATE.PAN` is set to 1 automatically (SPAN=0 in SCTLR). The kernel's page table (`TTBR1_EL1`) is active throughout the kernel.

2. **System call handler**: `read()` dispatches to `ksys_read()`. The user buffer pointer and size are passed from registers (X0=fd, X1=buf, X2=count).

3. **File system/VFS reads data to kernel buffer**: File system reads file data into a kernel page cache page. That page has `AP[2:1]=0b00` (kernel RW, user no access), `PXN=1, UXN=1`.

4. **copy_to_user(user_buf, kernel_buf, count)**: Must copy from kernel page to user page. `uaccess_enable()` sets `PSTATE.UAO=1` (or clears PAN=0). The LDP/STP copy instructions now use EL0 permissions. Destination PTE for user buffer: `AP[2:1]=0b01` (user RW) → EL0 write allowed → copy succeeds. After copy: `uaccess_disable()` restores PAN=1, UAO=0.

5. **Return**: `ERET` back to EL0. PSTATE restored from SPSR_EL1, PAN cleared (EL0 state). User code continues with data in its buffer.

---

## 7. Quick Reference

| Page type | AP[2:1] | PXN | UXN | AttrIndx |
|---|---|---|---|---|
| Kernel data | 0b00 | 1 | 1 | MT_NORMAL |
| Kernel RO data | 0b10 | 1 | 1 | MT_NORMAL |
| Kernel text | 0b10 | 0 | 1 | MT_NORMAL |
| User text | 0b11 | 1 | 0 | MT_NORMAL |
| User data | 0b01 | 1 | 1 | MT_NORMAL |
| User RO data | 0b11 | 1 | 1 | MT_NORMAL |
| Device MMIO | 0b00 | 1 | 1 | MT_DEVICE_nGnRnE |
| User MMIO (UIO) | 0b01 | 1 | 1 | MT_DEVICE_nGnRnE |
| DMA buffer (NC) | 0b00 | 1 | 1 | MT_NORMAL_NC |
