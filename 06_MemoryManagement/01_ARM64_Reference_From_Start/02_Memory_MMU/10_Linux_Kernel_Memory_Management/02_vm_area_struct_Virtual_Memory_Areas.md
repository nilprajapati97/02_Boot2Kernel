# vm_area_struct: Virtual Memory Areas

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
vm_area_struct (VMA): describes ONE contiguous virtual address region
  with uniform permissions, flags, and backing store.

A process's virtual address space is a COLLECTION of VMAs:
  - One VMA per ELF segment (text, data, bss)
  - One VMA per mmap() call
  - One VMA for the stack, heap, vdso, vsyscall
  
  VMAs are NON-OVERLAPPING, contiguous regions
  Between VMAs: "holes" — unmapped virtual address regions
  Accessing a hole: SIGSEGV (page fault not handled → signal)

VMA represents:
  A range [vm_start, vm_end) of virtual addresses
  All pages in this range have the SAME permissions (rwx)
  All pages in this range back to the SAME type of physical memory
  (anonymous, file-backed, device, shared)

Inspecting VMAs:
  /proc/<pid>/maps: text listing of all VMAs
  /proc/<pid>/smaps: detailed stats per VMA
  
  Example /proc/1234/maps:
    7f1234000000-7f1234100000 r-xp 00000000 08:01 12345678 /lib/libc.so.6
    7f1234100000-7f1234101000 r--p 00100000 08:01 12345678 /lib/libc.so.6
    7f1234101000-7f1234102000 rw-p 00101000 08:01 12345678 /lib/libc.so.6
    7fff00000000-7fff00001000 rw-p 00000000 00:00 0         [stack]
    
    Format: start-end permissions offset dev inode filename
    permissions: r/w/x + p(rivate)/s(hared)
```

---

## 2. vm_area_struct Structure

```c
/* include/linux/mm_types.h */
struct vm_area_struct {
    /* VMA address range: [vm_start, vm_end) */
    unsigned long   vm_start;   // inclusive start VA
    unsigned long   vm_end;     // exclusive end VA (byte after last)
    
    /* Linked list and maple tree / rb_tree links */
    struct mm_struct    *vm_mm; // back-pointer to mm_struct
    
    /* Permissions and flags */
    pgprot_t        vm_page_prot;  // page protection bits (for new PTEs)
    unsigned long   vm_flags;      // VM_READ | VM_WRITE | VM_EXEC | ...
    
    /* File backing (NULL if anonymous) */
    struct file     *vm_file;       // file this VMA maps (NULL = anonymous)
    unsigned long   vm_pgoff;       // offset in file, in PAGE_SIZE units
    
    /* VMA operations (function pointers) */
    const struct vm_operations_struct *vm_ops;
    
    /* Anonymous page management */
    union {
        struct anon_vma     *anon_vma;     // reverse mapping (rmap)
        struct anon_vma_chain *anon_vma_chain; // chain for COW
    };
    
    /* Maple tree / linked list for mmap traversal */
    // In Linux 6.1+: embedded in maple_tree via vm_mt
    
    /* Huge pages */
    unsigned char   vm_userfaultfd_ctx; // userfaultfd
};
```

---

## 3. vm_flags: VMA Permission Bits

```
Important vm_flags values:

VM_READ  (0x00000001): pages can be read
VM_WRITE (0x00000002): pages can be written
VM_EXEC  (0x00000004): pages can be executed
VM_SHARED(0x00000008): shared mapping (not COW)
         If NOT set: private/COW mapping

VM_MAYREAD  (0x00000010): mmap can set VM_READ
VM_MAYWRITE (0x00000020): mmap can set VM_WRITE
VM_MAYEXEC  (0x00000040): mmap can set VM_EXEC
VM_MAYSHARE (0x00000080): mmap can set VM_SHARED

VM_GROWSDOWN (0x00000100): stack VMA (grows downward)
VM_PFNMAP   (0x00000400): PFN-based mapping (device memory, no struct page)
VM_DONTEXPAND(0x00000800): cannot expand via mremap
VM_ACCOUNT  (0x00100000): memory accounting
VM_HUGETLB  (0x00400000): HugeTLB page mapping
VM_HUGEPAGE (0x00800000): request transparent huge pages
VM_NOHUGEPAGE(0x01000000): prohibit THP for this VMA
VM_DONTDUMP (0x04000000): don't dump this VMA in core dump

ARM64-specific considerations:
  VM_EXEC: corresponds to UXN=0 in the PTE (user execute allowed)
  VM_WRITE: corresponds to AP[2]=0 (read-write)
  VM_READ: no direct PTE bit; managed via AP[1]=1 (EL0 access)
  
  vm_page_prot: derived from vm_flags by protection_map[] table
  ARM64 protection_map[]:
    { r--: PAGE_READONLY_EXEC,
      rw-: PAGE_COPY,          // COW
      r-x: PAGE_READONLY_EXEC,
      rwx: PAGE_COPY_EXEC,
      ...}
  PAGE_KERNEL_EXEC: UXN=0, PXN=0 (kernel execute, rare)
  PAGE_SHARED: AP=EL0_RW, UXN=1 (no execute)
```

---

## 4. VMA Data Structures

```
Linux 6.1+: Maple Tree (replaces red-black tree + doubly linked list)
  Maple Tree: B-tree variant optimized for range queries
  mm->mm_mt: the maple tree of VMAs keyed by [start, end)
  
  Operations:
    find VMA by address: mt_find(&mm->mm_mt, &index, max_addr)
    insert VMA: mas_store(&mas, vma)
    delete VMA: mas_erase(&mas)
    range query: mas_for_each(&mas, vma, last_addr)

Pre-6.1: Red-Black Tree + Linked List:
  mm->mm_rb: root of RB tree (fast O(log n) lookup by address)
  vma->vm_rb: node in RB tree
  mm->mmap: pointer to FIRST VMA (ordered by start address)
  vma->vm_next, vma->vm_prev: linked list for sequential traversal
  
  Why both? RB tree: O(log n) for random access. Linked list: O(1) sequential.

VMA lookup:
  find_vma(mm, addr):
    In Linux 6.1+: mt_find (maple tree range lookup)
    Returns: VMA such that vma->vm_end > addr (first VMA ending after addr)
    If addr is in the VMA: vma->vm_start <= addr < vma->vm_end
    If addr is in a hole: vma->vm_start > addr
    
  find_vma_intersection(mm, start, end):
    Returns VMA that overlaps [start, end)

Merging VMAs:
  Adjacent VMAs with same flags/file/offset: can be merged
  vma_merge() in mm/mmap.c: attempts to merge new VMA with adjacent ones
  Why merge? Fewer VMAs = faster lookup, less memory for VMA structs
  But: limits on number of VMAs per process (vm.max_map_count = 65530 default)
```

---

## 5. VMA Operations (vm_ops)

```
vm_operations_struct: function pointers for VMA operations

struct vm_operations_struct {
    void (*open)(struct vm_area_struct *area);
    void (*close)(struct vm_area_struct *area);
    int  (*fault)(struct vm_fault *vmf);          // page fault handler!
    int  (*huge_fault)(struct vm_fault *vmf, ...);
    void (*map_pages)(struct vm_fault *vmf, ...); // prefault readahead
    int  (*page_mkwrite)(struct vm_fault *vmf);   // first write to file page
    int  (*pfn_mkwrite)(struct vm_fault *vmf);
    int  (*access)(struct vm_area_struct *vma, unsigned long addr, ...);
    const char *(*name)(struct vm_area_struct *vma);  // VMA name for /proc/maps
};

Key: vma->vm_ops->fault() is called by the page fault handler!
  When CPU accesses an unmapped page → hardware page fault → do_page_fault()
  → handle_mm_fault() → __handle_mm_fault() → handle_pte_fault()
  → vma->vm_ops->fault() → allocate/map the page

Anonymous VMA: vm_ops = NULL (kernel handles directly in handle_pte_fault())
File-backed VMA: vm_ops = file->f_op->mmap sets vma->vm_ops
  e.g., ext4: ext4_file_vm_ops → .fault = filemap_fault
  e.g., shmem: shmem_file_operations → .fault = shmem_fault

Special VMAs:
  /dev/zero: vm_ops = zero_vma_ops (allocates zero-filled pages)
  vdso: vdso_vm_ops (maps pre-built VDSO binary)
  hugetlbfs: hugetlb_vm_ops (maps huge pages from hugetlb pool)
```

---

## 6. Interview Questions & Answers

**Q1: Explain the difference between private and shared VMAs and how they affect fork/CoW behavior.**

A **private (anonymous or file-backed)** VMA has `VM_SHARED` NOT set. When the process modifies a page in a private VMA, a **copy-on-write (CoW)** occurs: the kernel allocates a new private physical page, copies the original content, and maps the new page writable. The original (shared) page is unmodified. This is how `fork()` works efficiently: parent and child share physical pages (mapped read-only in both), and only when one writes does a private copy get made.

A **shared VMA** (`VM_SHARED` set) means all mappings point to the **same physical pages**. A write in one process is visible to all other processes mapping the same region. This is used for `MAP_SHARED` file mappings (both processes see each other's writes, which go back to the file) and `SHM_OPEN`/`mmap` based shared memory IPC.

For **ARM64 page tables**: private VMA pages are mapped with `AP[2]=1` (read-only, for COW) in both parent and child after `fork()`. A write triggers a permission fault → `do_wp_page()` → allocate new page, remap as `AP[2]=0` (read-write). Shared VMA pages are mapped read-write from the start and remain shared.

---

## 7. Quick Reference

| vm_flags Combination | VMA Type | PTE Permissions (ARM64) |
|---|---|---|
| VM_READ | Read-only file | AP=EL0_RO, UXN=1 |
| VM_READ\|VM_EXEC | .text segment | AP=EL0_RO, UXN=0 |
| VM_READ\|VM_WRITE | .data / heap | AP=EL0_RW, UXN=1 |
| VM_READ\|VM_WRITE\|VM_EXEC | (unusual, writable+exec) | AP=EL0_RW, UXN=0 |
| VM_GROWSDOWN | Stack | AP=EL0_RW, UXN=1 |
| VM_HUGETLB | Huge pages | Same + 2MB/1GB block descriptors |

| VMA Lifecycle Function | What It Does |
|---|---|
| mmap() | Create new VMA, insert in mm_mt/rb |
| munmap() | Remove VMA, free page tables, TLB flush |
| mprotect() | Change vm_flags + vm_page_prot, update PTEs |
| brk() | Extend/shrink heap VMA |
| fork() | Copy VMAs + mark pages COW |
| execve() | Replace all VMAs with new ELF VMAs |
