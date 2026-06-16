# UXN and XN: Unprivileged Execute Never

**Category**: Access Permissions & Execute Never  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. UXN and XN Definitions

```
UXN (Unprivileged Execute Never) / XN (Execute Never):
  PTE bit[54] in Stage 1 descriptors for EL1/EL0

  The bit name differs by context:
    When used in EL1/EL0 Stage 1 tables: called UXN
    When used in EL2 or EL3 Stage 1 tables: called XN (no user concept at EL2/3)
    When used in Stage 2 tables: called XN

  UXN = 0: User space (EL0) CAN execute instructions from this page
  UXN = 1: User space (EL0) CANNOT execute — Instruction Abort if EL0 tries

  UXN only affects EL0 execution from EL1/EL0 Stage 1 translations.
  Does NOT affect kernel (EL1) execution (that's PXN).
  
Key distinction:
  PXN bit[55]: restricts EL1 (kernel) execution
  UXN bit[54]: restricts EL0 (user) execution
  
  Both can be set simultaneously on a page to prevent ANY execution (pure data)
```

---

## 2. Default Linux UXN Policy

```
Linux applies UXN=1 to ALL kernel pages, always.
  Rationale: User code must never execute from kernel virtual addresses.
  Even if user code somehow obtained a kernel VA, UXN=1 blocks execution.

User space page types and UXN:
  
  Executable code pages (VM_EXEC set in VMA flags):
    UXN = 0 → user can execute
    PXN = 1 → kernel cannot execute (W^X for kernel side)
    AP[2:1] = 0b01 (user can at least read)
    
  Stack pages (VM_READ|VM_WRITE, no VM_EXEC):
    UXN = 1 → user cannot execute stack as code
    This prevents stack-based shellcode execution
    
  Heap pages (VM_READ|VM_WRITE, no VM_EXEC):
    UXN = 1 → user cannot execute heap as code
    This prevents heap spray attacks
    
  File-backed data mappings:
    Default: UXN = 1 (data, not code)
    Exception: shared library mappings with VM_EXEC → UXN = 0
    
  JIT compilation (mmap with PROT_EXEC):
    UXN = 0 → user can execute JIT'd code
    W^X: once PROT_EXEC is set, PROT_WRITE should be cleared
    mprotect(buf, size, PROT_READ|PROT_EXEC) after JIT compilation
    Modern constraint: CONFIG_STRICT_EXECMEM prevents PROT_EXEC|PROT_WRITE together

Page transition:
  New anonymous page (PROT_READ|PROT_WRITE):
    UXN = 1, AP = 0b01 (user RW)
  
  After mprotect(PROT_READ|PROT_EXEC):
    UXN = 0, AP = 0b11 (user RO — write removed when exec added)
    → W^X: cannot be writable AND executable simultaneously
```

---

## 3. mprotect() and UXN Changes

```
mprotect() system call sequence for making page executable:

User call: mprotect(addr, len, PROT_READ | PROT_EXEC)

Kernel path:
  do_mprotect_pkey()
  → mprotect_fixup()
    → change_protection()
      → change_pte_range()
        → For each PTE in range:
          - Clear UXN bit (bit[54]) in PTE
          - Ensure PXN=1 (kernel cannot execute user page)
          - If PROT_WRITE not in new_flags: set AP[2]=1 (read-only)
          - ptep_modify_prot_start() / ptep_modify_prot_commit()
  → flush_tlb_range() → TLBI VAE1IS for each page
  
  After TLB flush: CPU will fetch fresh PTEs with new UXN=0 on next access

W^X enforcement in Linux (execmem_is_ro):
  Security policy: PROT_WRITE and PROT_EXEC cannot coexist
  
  If mprotect(PROT_WRITE|PROT_EXEC):
    With CONFIG_STRICT_W_XOR_X=y:
    Returns -EACCES
    
  Without strict mode:
    AP allows write (AP[2]=0), UXN=0 allows exec
    Security risk: code injection possible
    
  Kernel's own code must always maintain W^X
  mark_rodata_ro() ensures .text is not writable after initialization
```

---

## 4. UXN Fault and Signal Delivery

```
UXN Instruction Abort:
  CPU at EL0 fetches instruction from page with UXN=1
  
  Exception taken to EL1 (because fault is from EL0):
  ESR_EL1: EC = 0b100000 (Instruction Abort from lower EL)
  IFSC = 0b001111 (Permission Fault, L3)
  FAR_EL1 = address of faulting instruction

Linux fault handler:
  do_page_fault()
  → arm64_do_page_fault() [arch/arm64/mm/fault.c]
    → fault_signal_inject() for UXN fault from user space
    → si_signo = SIGSEGV
    → si_code = SEGV_ACCERR (access error — permissions violated)
    → si_addr = faulting address (from FAR_EL1)
    → User process receives SIGSEGV and typically terminates with core dump

Common causes of UXN fault in practice:
  1. Buffer overflow corrupts function pointer → jumps to stack/heap (UXN=1)
  2. ROP chain jumps to writable data region instead of code
  3. mmap() of read/write file without PROT_EXEC, then jump to it
  4. JIT code not marked with PROT_EXEC (remains UXN=1 as data page)
  5. Null pointer dereference at address 0 (zero page is UXN=1 if mapped at all)

Security value:
  UXN prevents classical stack/heap shellcode injection attacks
  Attacker must find existing executable code (ROP/JOP gadgets) instead
  NX bit (x86) is the equivalent protection
```

---

## 5. ARM64 XN in Stage 2 (Hypervisor)

```
Stage 2 descriptor bit[54] = XN (Execute Never for both EL0 and EL1 of guest)
Stage 2 descriptor bit[53] = S2AP (Stage 2 Access Permissions, 2 bits)

XN in Stage 2:
  XN = 1: Guest (both EL0 and EL1) cannot execute from this GPA
  Used by hypervisor to prevent guest from executing:
  - MMIO regions passed through to guest
  - Memory regions reserved for hypervisor use
  
  KVM implementation:
    Guest RAM: Stage 2 XN = 0 (guest can execute from RAM)
    MMIO passthrough: Stage 2 XN = 1 (device memory, not executable)
    
  S2AP[1:0]:
    0b00 = No access (fault on any access)
    0b01 = Read-only
    0b10 = Read/Write
    0b11 = Read/Write (same as 0b10 in most implementations)
```

---

## 6. Interview Questions & Answers

**Q1: Why does Linux set UXN=1 on stack and heap pages, and why does this matter for security?**

Linux sets `UXN=1` on stack and heap pages because they are writable data regions that should never contain executable code. The W^X (Write XOR Execute) policy requires that no memory region can be simultaneously writable and executable. Without `UXN=1` on the stack, a classic stack shellcode injection attack would work: an attacker overflows a buffer on the stack, writes machine code bytes into it, then redirects the return address to point to the stack. With `UXN=1`, when execution reaches the stack address, the CPU raises an Instruction Abort → the attack fails. This is exactly what NX (No Execute) / DEP (Data Execution Prevention) does on x86. It forces attackers to use more sophisticated techniques like ROP (Return-Oriented Programming) that reuse existing executable code. Linux enforces `UXN=1` via `mprotect()` enforcement: new anonymous pages start with no PROT_EXEC (UXN=1), and if PROT_EXEC is added, PROT_WRITE must be removed.

---

## 7. Quick Reference

| Scenario | UXN value | Result |
|---|---|---|
| User executable code (.text, JIT) | 0 | EL0 can execute |
| User data (stack, heap, rodata) | 1 | EL0 cannot execute |
| Kernel pages (any) | 1 | EL0 cannot access kernel VA |
| MMIO page (device) | 1 | No execution from device memory |

| Permission | AP[2:1] | PXN | UXN | Security property |
|---|---|---|---|---|
| User exec | 0b01 | 1 | 0 | User: RW+exec; Kernel: cannot exec |
| User data | 0b01 | 1 | 1 | User: RW; no exec from data |
| Kernel exec | 0b10 | 0 | 1 | Kernel: RO+exec; User: no access |
| Kernel data | 0b00 | 1 | 1 | Kernel: RW; no exec; User: no access |
