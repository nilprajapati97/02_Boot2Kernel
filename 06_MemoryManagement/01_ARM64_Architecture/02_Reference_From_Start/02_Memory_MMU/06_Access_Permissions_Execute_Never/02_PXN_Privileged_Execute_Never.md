# PXN: Privileged Execute Never — Deep Dive

**Category**: Access Permissions & Execute Never  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. What is PXN?

```
PXN (Privileged Execute Never):
  PTE bit[55] (Stage 1 descriptor)
  
  PXN = 0: EL1 (kernel) CAN execute instructions from this page
  PXN = 1: EL1 (kernel) CANNOT execute — Instruction Abort if attempted

PXN applies ONLY to EL1:
  Does not restrict EL0 execution (that's UXN/XN — bit[54])
  Does not apply to EL2 or EL3 translations

Why PXN exists:
  Security: Prevent kernel code execution of user-space pages
  (Prevents JOP — Jump-Oriented Programming — from kernel gadgets to user pages)
  
  Without PXN: if attacker places shellcode in user memory,
  a kernel vulnerability that redirects EL1 execution could
  cause the kernel to execute attacker code from user-space VA.
  
  With PXN=1 on user pages: EL1 trying to execute user page → Instruction Abort
  (KPTI partially addresses this; PXN provides defense-in-depth)
```

---

## 2. PXN Bit in PTE and Table Descriptors

```
PXN in LEAF descriptor (page/block):
  Bit[55] = PXN
  0 → kernel can execute this page
  1 → kernel cannot execute this page

PXNTable in TABLE descriptor (non-leaf, intermediate level):
  Table descriptor bit[59] = PXNTable
  
  PXNTable = 1: ALL pages reachable through this table have PXN=1
              regardless of their individual PXN bits
  PXNTable = 0: Individual PXN bits in leaf descriptors apply
  
  This is the Hierarchical Permission Override mechanism.
  HPD0/HPD1 in TCR_EL1 can disable this override (see Category 04).

Linux usage:
  All kernel data mapped via the linear map:
    PXN = 0 for .text (executable)
    PXN = 1 for .data, .bss, .rodata, stack, heap (non-executable)
  
  User pages in TTBR0:
    PXN = 1 always (kernel must never execute user pages)
    Linux sets this via PAGE_USER flag which includes PXN=1
    
  Kernel linear map table descriptor:
    pgd → p4d → pud → pmd for kernel linear map:
    PXNTable = 0 (let leaf PXN bits control)
    Leaf blocks with kernel data: PXN = 1
    Leaf blocks with kernel text: PXN = 0
```

---

## 3. Linux Kernel PXN Policy

```
Boot sequence PXN setup (arch/arm64/mm/mmu.c):

  create_mapping_late(): maps kernel sections with appropriate PXN:
    
  Kernel .text section (executable code):
    pgprot = PAGE_KERNEL_EXEC
    PXN = 0, UXN = 1  (kernel executes, user cannot)
    
  Kernel .rodata section:
    pgprot = PAGE_KERNEL_RO
    PXN = 1 (read-only data, kernel should not execute from here)
    UXN = 1
    
  Kernel .data, .bss:
    pgprot = PAGE_KERNEL
    PXN = 1, UXN = 1 (no one can execute data sections)
    AP = 0b00 (kernel RW, user no access)

  mark_rodata_ro() called from mark_readonly():
    Changes kernel text from PAGE_KERNEL_EXEC to PAGE_KERNEL_ROX:
    AP = 0b10 (read-only), PXN = 0 (still executable)
    This makes .text read-only AND executable (no writable+exec)
    Called during kernel init after all patching complete

After mark_rodata_ro():
  .text: AP=0b10 (RO), PXN=0 → kernel can read and execute, cannot write
  .rodata: AP=0b10 (RO), PXN=1 → kernel can read, cannot write or execute
  .data: AP=0b00 (RW kernel only), PXN=1 → kernel can read/write, cannot exec
  User pages: AP=0b01 (EL0 RW), PXN=1 → user can RW, kernel CANNOT exec
```

---

## 4. PXN Fault Details

```
PXN fault occurs when:
  CPU at EL1 attempts instruction fetch from page with PXN=1
  
Exception:
  Instruction Abort at EL1
  ESR_EL1: EC = 0b100001 (Instruction Abort at current EL)
  IFSC = 0b001111 (Permission Fault, L3)
  FAR_EL1 = faulting instruction VA

In Linux:
  do_page_fault() called
  access_error() checks: ESR_ELx_EXEC (instruction fetch fault)
  + page is PXN (kernel code tried to execute non-exec page)
  
  Normal case: this indicates a kernel bug
  → BUG() / oops() triggered
  → Stack dump, register dump, then kernel panic or continue (depends on config)
  
  NEVER expected in a correctly operating system:
  If you see a PXN fault in kernel log:
  "Unable to handle kernel paging request at virtual address ..."
  with ESR showing "instruction access" → likely a code pointer corruption:
    - Stack overflow into adjacent data page
    - Corrupted function pointer pointing to data section
    - Buffer overflow that redirected PC to writable memory
```

---

## 5. PXN and the SMEP Equivalent

```
x86 has SMEP (Supervisor Mode Execution Prevention):
  CR4.SMEP=1: CPU at ring 0 cannot execute user-space pages

ARM64 equivalent:
  SCTLR_EL1.WXN: Writable implies XN (all writable pages are non-executable)
  PXN=1 on user pages via page permission bits

SMEP vs ARM PXN:
  x86 SMEP: global hardware feature enabled by a single CR4 bit
  ARM64: per-page via PXN bit; software ensures user pages have PXN=1
  
  ARM64 advantage: fine-grained (some kernel data can be executable, e.g., trampolines)
  ARM64 disadvantage: if a page gets wrong permissions, exploit may work;
  x86 SMEP is harder to bypass since it's a global CPU feature

Modern ARM64 SoCs:
  FEAT_HAFT (ARMv8.9): Hardware enforces that user VA pages always have PXN
  Proposal to make PXN=1 for EL0 mappings a hardware guarantee
  Not yet universally available; Linux still sets PXN explicitly per page
```

---

## 6. Interview Questions & Answers

**Q1: What is the difference between PXN and UXN, and how does Linux use each?**

**PXN (Privileged Execute Never, bit[55])** prevents code execution by the kernel (EL1). When `PXN=1`, if the kernel's program counter reaches that page, an Instruction Abort exception occurs. Linux sets `PXN=1` on ALL user-space pages and ALL kernel data pages (`.data`, `.bss`, heap, stacks). This prevents kernel exploits from redirecting execution into user memory or kernel data.

**UXN (Unprivileged Execute Never, bit[54])** prevents code execution by user space (EL0). When `UXN=1`, user applications cannot execute instructions from that page. Linux sets `UXN=1` on ALL kernel pages (the kernel never wants users executing kernel memory) and also on user data pages (stack, heap, file-backed data). Only user `.text` (executable code) pages have `UXN=0`.

**Together**: A normal user code page has `PXN=1, UXN=0` (user can execute, kernel cannot). A kernel code page has `PXN=0, UXN=1` (kernel can execute, user cannot). All data pages (regardless of privilege level) should have both `PXN=1` and `UXN=1` — this is the W^X enforcement.

---

## 7. Quick Reference

| Bit | Name | When 1 | Applies to |
|---|---|---|---|
| [55] | PXN | EL1 cannot execute | Kernel privilege level |
| [54] | UXN/XN | EL0 cannot execute | User privilege level |
| [59] (table) | PXNTable | All pages below cannot EL1-exec | Table-level override |
| [58] (table) | XNTable | All pages below cannot EL0-exec | Table-level override |

| Page type | AP[2:1] | PXN | UXN | Meaning |
|---|---|---|---|---|
| Kernel text (exec) | 0b10 | 0 | 1 | Kernel: RO+exec; User: none |
| Kernel data | 0b00 | 1 | 1 | Kernel: RW; User: none |
| User text (exec) | 0b01 | 1 | 0 | User: RW+exec; Kernel: RW |
| User data | 0b01 | 1 | 1 | User: RW; Kernel: RW; no exec |
