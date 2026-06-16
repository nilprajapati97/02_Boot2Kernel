# Page Fault: do_page_fault and Kernel Page Fault Handler Flow

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. ARM64 Page Fault Entry

```
ARM64 Exception Entry (arch/arm64/kernel/entry.S):

Page fault triggers when:
  CPU cannot translate a virtual address using TTBR0/TTBR1 page tables
  OR access violates permissions

Exception types:
  Data Abort:       load/store to unmapped or no-permission address
  Instruction Abort: execute from unmapped or PXN/UXN address
  
Exception vectors (arm64):
  el1_abort:   kernel-mode data abort (EL1 page fault)
  el1_ia:      kernel-mode instruction abort
  el0_da:      user-mode data abort
  el0_ia:      user-mode instruction abort

ARM64 registers on fault:
  ESR_EL1: Exception Syndrome Register — describes the fault type
    ESR_EL1.EC[31:26]: 0b100100 = data abort (EL0)
                       0b100101 = data abort (EL1)
                       0b100000 = instruction abort (EL0)
                       0b100001 = instruction abort (EL1)
    ESR_EL1.DFSC[5:0]: Data Fault Status Code
      0b000001: Translation fault level 1 (PGD entry invalid)
      0b000010: Translation fault level 2 (PUD/PMD entry invalid)
      0b000011: Translation fault level 3 (PTE entry invalid)
      0b001001: Access flag fault level 1
      0b001010: Access flag fault level 2
      0b001011: Access flag fault level 3
      0b001101: Permission fault level 1
      0b001110: Permission fault level 2
      0b001111: Permission fault level 3
    ESR_EL1.WnR: 1 = Write caused the fault, 0 = Read
    ESR_EL1.CM:  Cache maintenance instruction caused the fault
    
  FAR_EL1: Fault Address Register — the virtual address that faulted

Fault type decision:
  If ESR_EL1.EC == data_abort_el0 or instruction_abort_el0:
    → user-mode fault → do_mem_abort() with is_el0=true
  If ESR_EL1.EC == data_abort_el1 or instruction_abort_el1:
    → kernel-mode fault → do_mem_abort() with is_el0=false
```

---

## 2. ARM64 Page Fault Handler Call Chain

```
Entry point (arch/arm64/mm/fault.c):

do_mem_abort(unsigned long far, unsigned long esr, struct pt_regs *regs)
  ↓
  Parse ESR_EL1 into fault_info:
    esr_to_fault_info(esr) → look up in fault_info table
    → dispatches to: do_translation_fault, do_page_fault, do_alignment_fault, etc.
  ↓
  For translation fault or permission fault:
    → do_page_fault(unsigned long far, unsigned long esr, struct pt_regs *regs)
  ↓
  do_page_fault():
    1. fault_addr = far (from FAR_EL1 register, read in entry.S)
    2. Determine fault_flags:
       - FAULT_FLAG_WRITE:  if ESR_EL1.WnR set
       - FAULT_FLAG_EXEC:   if instruction abort
       - FAULT_FLAG_USER:   if from EL0
       - FAULT_FLAG_REMOTE: if remote TLB shootdown (not normal path)
       - FAULT_FLAG_ALLOW_RETRY: kernel can retry on lock
    
    3. mm = current->mm (or init_mm for kernel fault)
    4. mmap_read_lock(mm)   ← take mmap_lock (read mode)
    5. vma = find_vma(mm, fault_addr)
       → look up VMA in maple tree / rb tree
    
    6. if (!vma) goto bad_area   ← no VMA, invalid access
    7. if (vma->vm_start > fault_addr):
         // fault in a "hole" between VMAs
         // but if stack VMA and can expand: expand_stack(vma, fault_addr)
         // else: goto bad_area
    
    8. // VMA found and addr is in VMA
       Check permissions:
       if (fault_flags & FAULT_FLAG_WRITE):
           if (!(vma->vm_flags & VM_WRITE)): goto bad_area_nosemaphore
           // if PROT_NONE or read-only page: this will CoW later
       if (fault_flags & FAULT_FLAG_EXEC):
           if (vma->vm_flags & VM_EXEC == 0): goto bad_area_nosemaphore
    
    9. ret = handle_mm_fault(vma, fault_addr, fault_flags, regs)
    10. mmap_read_unlock(mm)
    11. Process return value
```

---

## 3. handle_mm_fault and Page Table Walk

```c
/* mm/memory.c */

vm_fault_t handle_mm_fault(struct vm_area_struct *vma,
                            unsigned long address,
                            unsigned int fault_flags,
                            struct pt_regs *regs)
{
    // Check for huge page THP possibility first:
    if (unlikely(is_vm_hugetlb_page(vma)))
        return hugetlb_fault(vma->vm_mm, vma, address, fault_flags);
    
    // Regular page fault:
    return __handle_mm_fault(vma, address, fault_flags);
}

vm_fault_t __handle_mm_fault(struct vm_area_struct *vma,
                               unsigned long address, unsigned int flags)
{
    struct vm_fault vmf = {
        .vma = vma,
        .address = address & PAGE_MASK,
        .flags = flags,
        .pgoff = linear_page_index(vma, address),
    };
    
    // Walk/create page tables:
    pgd_t *pgd = pgd_offset(vma->vm_mm, address);
    p4d_t *p4d = p4d_alloc(vma->vm_mm, pgd, address);
    if (!p4d) return VM_FAULT_OOM;
    
    vmf.pud = pud_alloc(vma->vm_mm, p4d, address);
    if (!vmf.pud) return VM_FAULT_OOM;
    
    // Check for PUD-level huge page (1GB):
    if (pud_none(*vmf.pud) && thp_vma_allowable_order(vma, ...))
        return create_huge_pud(&vmf);
    
    vmf.pmd = pmd_alloc(vma->vm_mm, vmf.pud, address);
    if (!vmf.pmd) return VM_FAULT_OOM;
    
    // Check for PMD-level huge page (2MB THP):
    if (pmd_none(*vmf.pmd) && thp_vma_allowable_order(vma, ...))
        return create_huge_pmd(&vmf);
    
    // 4KB PTE level:
    return handle_pte_fault(&vmf);
}

vm_fault_t handle_pte_fault(struct vm_fault *vmf)
{
    pte_t entry;
    
    vmf->pte = pte_offset_map(vmf->pmd, vmf->address);
    vmf->orig_pte = *vmf->pte;
    entry = vmf->orig_pte;
    
    if (!pte_present(entry)) {
        if (pte_none(entry)) {
            // No PTE at all:
            if (vma_is_anonymous(vmf->vma))
                return do_anonymous_page(vmf);   // anonymous demand paging
            else
                return do_fault(vmf);            // file-backed page fault
        }
        // PTE exists but not present: swapped out
        return do_swap_page(vmf);
    }
    
    // PTE is present:
    if (pte_protnone(entry) && vma_is_accessible(vmf->vma))
        return do_numa_page(vmf);  // NUMA page migration
    
    if (vmf->flags & (FAULT_FLAG_WRITE | FAULT_FLAG_UNSHARE)) {
        if (!pte_write(entry))
            return do_wp_page(vmf);  // copy-on-write!
    }
    
    // Nothing to do: spurious fault or access flag
    return VM_FAULT_NOPAGE;
}
```

---

## 4. Fault Return Values

```
vm_fault_t return value (bitfield):
  VM_FAULT_OOM    (0x0001): out of memory, kill process
  VM_FAULT_SIGBUS (0x0002): send SIGBUS (bus error — MMIO region, etc.)
  VM_FAULT_MAJOR  (0x0004): major fault (page fetched from disk)
  VM_FAULT_HWPOISON (0x0010): hardware memory error (ECC uncorrectable)
  VM_FAULT_SIGSEGV (0x0040): segmentation fault — send SIGSEGV
  VM_FAULT_NOPAGE (0x0100): fault handled silently (nothing to return)
  VM_FAULT_LOCKED (0x0200): returned locked page (caller must unlock)
  VM_FAULT_RETRY  (0x0400): need to retry (dropped mmap_lock)
  VM_FAULT_FALLBACK (0x0800): huge page fallback to small page
  VM_FAULT_DONE_COW (0x1000): COW completed

Back in do_page_fault(), processing return value:
  if (ret & VM_FAULT_OOM):
    pagefault_out_of_memory()   // invoke OOM killer
  if (ret & VM_FAULT_SIGBUS):
    force_sig_fault(SIGBUS, BUS_ADRERR, ...)
  if (ret & VM_FAULT_SIGSEGV):
    force_sig_fault(SIGSEGV, SEGV_MAPERR, ...)
  if (ret & VM_FAULT_MAJOR):
    current->maj_flt++          // update process major fault counter
  else:
    current->min_flt++          // update minor fault counter
```

---

## 5. Fault Types in Practice

```
1. Translation fault (most common — demand paging):
   ESR_EL1.DFSC = 0b000011 (translation fault L3)
   VMA found, pte_none() == true
   → do_anonymous_page() or do_fault()
   → Allocate physical page, install PTE
   
2. Permission fault (write to read-only = CoW trigger):
   ESR_EL1.DFSC = 0b001111 (permission fault L3)
   PTE present, AP[2]=1 (read-only), but VM_WRITE is set
   → do_wp_page() → copy-on-write
   
3. Access flag fault (hardware access flag management):
   ESR_EL1.DFSC = 0b001011 (access flag fault L3)
   ARM64 hardware: can fault when AF=0 (if HW access flag management enabled)
   → set_pte_bit(ptep, PTE_AF) → re-execute instruction
   
4. Spurious fault (TLB invalidation race):
   TLB entry was invalidated; PTE is still valid (race between CPUs)
   handle_pte_fault returns VM_FAULT_NOPAGE
   → cpu re-tries instruction with fresh TLB fill

5. Stack expansion fault:
   fault_addr < vma->vm_start, vma has VM_GROWSDOWN
   expand_stack(vma, fault_addr):
     → extend vma->vm_start downward
     → check rlimit RLIMIT_STACK
     → may fail if stack limit exceeded → SIGSEGV
```

---

## 6. Interview Questions & Answers

**Q1: Walk through exactly what happens (from hardware to user process) when a process reads from an unmapped virtual address in its VMA (demand paging scenario on ARM64).**

**Step 1 – Hardware fault**: CPU executes an `LDR` instruction. The MMU walks `TTBR0_EL1` page tables. At level 3 (PTE), it finds a zero entry (`pte_none` = 0). ARM64 raises a **synchronous exception** with `ESR_EL1.DFSC = 0b000011` (Translation fault, level 3). The faulting address is saved in `FAR_EL1`. CPU switches to EL1 and jumps to the exception vector (`el0_da`).

**Step 2 – Entry assembly**: `entry.S` saves all registers to the stack, reads `ESR_EL1` and `FAR_EL1`, calls `do_mem_abort(far, esr, regs)`.

**Step 3 – `do_page_fault()`**: Parses ESR, determines `fault_flags = FAULT_FLAG_USER` (EL0 fault). Takes `mmap_read_lock(current->mm)`. Calls `find_vma(mm, fault_addr)` → finds the VMA covering this address.

**Step 4 – `handle_mm_fault()` → `__handle_mm_fault()`**: Walks page tables. Calls `pmd_alloc()` to ensure PMD table exists (allocating if needed). Then calls `handle_pte_fault()`.

**Step 5 – `handle_pte_fault()`**: Finds PTE is zero (`pte_none`). VMA is anonymous → calls `do_anonymous_page(vmf)`.

**Step 6 – `do_anonymous_page()`**: 
- First access (read): maps the **shared zero page** (`ZERO_PAGE(0)`) read-only. No physical allocation yet!
- Install PTE: `AP[2]=1` (read-only), `AP[1]=1` (EL0 accessible), `AF=1`, `AttrIndx = Normal`, `UXN=1`.

**Step 7 – Return**: `handle_mm_fault` returns `VM_FAULT_MINOR`. `do_page_fault` increments `current->min_flt`. Releases `mmap_lock`. Returns to entry assembly.

**Step 8 – Resume**: Entry assembly restores registers, `ERET` to EL0. The `LDR` instruction re-executes. This time the TLB fills from the new PTE (zero page). The instruction returns zero. Process continues.

---

## 7. Quick Reference

| ESR_EL1.DFSC | Fault Type | Handler |
|---|---|---|
| 0b000001–0b000011 | Translation fault L1–L3 | do_anonymous_page / do_fault |
| 0b001001–0b001011 | Access flag fault L1–L3 | Set AF bit, continue |
| 0b001101–0b001111 | Permission fault L1–L3 | do_wp_page (CoW) |
| 0b100001 | Alignment fault | SIGBUS |

| fault_flags Bit | Set When |
|---|---|
| FAULT_FLAG_WRITE | ESR_EL1.WnR=1 (write fault) |
| FAULT_FLAG_EXEC | Instruction abort |
| FAULT_FLAG_USER | Fault from EL0 |
| FAULT_FLAG_ALLOW_RETRY | mmap_lock can be dropped for retry |
| FAULT_FLAG_TRIED | Already retried once |
