# ARM32 Linux Kernel Memory Management Internals
## Document 3: Linux MM Subsystem — ARMv7-A Deep Dive

**Author:** Senior Kernel Engineer  
**Target:** ARMv7-A (ARM32), Linux Kernel v5.x/v6.x  
**Scope:** Linux MM internals, page tables, page faults, context switch, highmem  
**Revision:** 1.0  
**Date:** 2026  
**Prerequisite:** Document 01 (Architecture Overview), Document 02 (Baremetal Init)

---

## Table of Contents
1. [Address Space Layout](#1-address-space-layout)
2. [Linux Page Table Structure](#2-linux-page-table-structure)
3. [mm_struct and vm_area_struct](#3-mm_struct-and-vm_area_struct)
4. [Page Fault Handling](#4-page-fault-handling)
5. [Context Switch — TTBR0/ASID Update](#5-context-switch--ttbr0asid-update)
6. [Memory Zones and the Page Allocator](#6-memory-zones-and-the-page-allocator)
7. [Highmem: PKMAP and FIXMAP](#7-highmem-pkmap-and-fixmap)
8. [vmalloc and ioremap](#8-vmalloc-and-ioremap)
9. [Copy-on-Write and Demand Paging](#9-copy-on-write-and-demand-paging)
10. [Kernel Direct Mapping](#10-kernel-direct-mapping)
11. [ARM-Specific MM Files in Linux Source](#11-arm-specific-mm-files-in-linux-source)
12. [Common Bugs and Debug Techniques](#12-common-bugs-and-debug-techniques)

---

## 1. Address Space Layout

### 1.1 Standard 3G/1G Split (CONFIG_VMSPLIT_3G)

```
Virtual Address Space (ARM32, 3G/1G split)

0xFFFFFFFF ┌──────────────────────────────────┐
           │ Exception Vectors (high vectors) │ 4KB  (0xFFFF0000)
0xFFFF0000 ├──────────────────────────────────┤
           │     CPU Vectors Page             │ Mapped via SCTLR.V=1
0xFFFE0000 ├──────────────────────────────────┤
           │     FIXADDR_TOP → FIXADDR_START  │ ~3MB (compile-time fixed VAs)
           │     (FIXMAP region)              │
0xFF800000 ├──────────────────────────────────┤
           │     Permanent Kernel Map         │ 2MB (PKMAP, highmem)
           │     PKMAP_BASE (0xFF000000)      │
0xFEE00000 ├──────────────────────────────────┤
           │         vmalloc area             │ ~240MB (vmalloc/ioremap)
           │     VMALLOC_START→VMALLOC_END    │
0xC0000000 ├══════════════════════════════════╡ ← PAGE_OFFSET / TTBR1 boundary
           │     Direct-mapped Kernel Memory  │ Physical RAM → Kernel Virtual
           │     PAGE_OFFSET + phys_offset    │ (lowmem)
           │     (kernel .text, .data, stack) │
           │     (page struct arrays)         │
0x80000000 ├──────────────────────────────────┤ (typical RAM top, 2GB phys)
           │                                  │
           │      U S E R   S P A C E         │ 0x00000000 – 0xBFFFFFFF
           │                                  │ (TTBR0 managed)
           │  [text][data][heap→][←stack]     │
           │  mmap region (libs, anon)        │
0x00000000 └──────────────────────────────────┘
```

### 1.2 Other Split Options

| Config               | User Space | Kernel Space | PAGE_OFFSET |
|----------------------|-----------|--------------|-------------|
| `CONFIG_VMSPLIT_3G`  | 0–3GB     | 3–4GB (1GB)  | 0xC0000000  |
| `CONFIG_VMSPLIT_2G`  | 0–2GB     | 2–4GB (2GB)  | 0x80000000  |
| `CONFIG_VMSPLIT_1G`  | 0–1GB     | 1–4GB (3GB)  | 0x40000000  |

> **Interview Note:** Why 3G/1G? Trades user addressable space for more kernel direct map. On systems with >1GB RAM, 3G/1G causes highmem pressure. Many embedded SoCs (Qualcomm, NVIDIA Tegra) use 2G/2G or custom splits.

### 1.3 TTBR Boundary and N-bit

```c
/* arch/arm/include/asm/pgtable.h */
#define PAGE_OFFSET     UL(CONFIG_PAGE_OFFSET)   /* e.g. 0xC0000000 */
#define TASK_SIZE       PAGE_OFFSET              /* Max user VA */

/*
 * TTBCR.N controls the split:
 * - N=1: TTBR0 → 0x00000000–0x7FFFFFFF, TTBR1 → 0x80000000–0xFFFFFFFF (2G/2G)
 * - N=2: TTBR0 → 0x00000000–0x3FFFFFFF, TTBR1 → 0xC0000000–0xFFFFFFFF (1G/3G)
 *
 * For 3G/1G: TTBCR.N=2 → TTBR1 covers top 1GB (kernel)
 */
#define TTBCR_N_VALUE   2   /* for 3G/1G split */
```

---

## 2. Linux Page Table Structure

### 2.1 Four-Level vs Two-Level Reality on ARM32

Linux uses a 4-level page table model (pgd → p4d → pud → pmd → pte) but ARM32 hardware only has 2 levels. Linux folds the unused levels:

```
Linux logical:   pgd → [p4d] → [pud] → pmd → pte
ARM32 reality:   pgd →                  pmd → pte
                 (p4d and pud are compile-time folded)
```

### 2.2 ARM32 PGD (Page Global Directory)

```
ARM32 PGD:
- 4096 entries × 4 bytes = 16KB total
- Each entry covers 1MB of VA
- PGD base is stored in TTBR0 (user) or TTBR1 (kernel)
- MUST be 16KB aligned (hardware requirement)

struct mm_struct {
    pgd_t *pgd;         /* points to 16KB PGD */
    ...
};

typedef struct { unsigned long pgd[2]; } pgd_t;
/*
 * ARM32: Each Linux pgd_t covers 2MB (two 1MB hardware L1 entries)
 * So Linux PGD has 2048 entries × 8 bytes = 16KB
 * Matches hardware: 4096 L1 entries × 4 bytes = 16KB
 */
```

**Why pgd[2]?**  
Linux ARM32 uses a two-word PGD entry to allow the second word to store a pointer to a Linux-format PTE table (separate from the hardware PTE table). This is the "software/hardware PTE split."

### 2.3 Hardware vs Software PTE Split

This is one of the most ARM32-specific design decisions — a critical interview topic.

```
ARM32 Linux PTE Reality:

For each 2MB VA region (one Linux PGD entry):
┌──────────────────────────────────────────────────────────┐
│  One 4KB page allocated for BOTH hw and sw PTE tables    │
├──────────────────────┬───────────────────────────────────┤
│  Software PTEs       │  Hardware PTEs                    │
│  (Linux format)      │  (ARM HW format)                  │
│  512 entries × 4B    │  512 entries × 4B                 │
│  addr: pte_page      │  addr: pte_page + 0x800 (2KB up)  │
└──────────────────────┴───────────────────────────────────┘

Linux PTE flags (software):
  PTE_PRESENT, PTE_YOUNG, PTE_DIRTY, PTE_WRITE, PTE_EXEC

ARM Hardware PTE flags:
  AP bits, TEX/C/B, XN, S, nG (hardware-enforced)

Linux populates BOTH on page mapping. Hardware only walks HW PTEs.
```

```c
/* arch/arm/include/asm/pgtable-2level.h */
/*
 * The "pgd_t" structure holds 2 hardware L1 descriptors (2 × 1MB = 2MB).
 * For page tables (not sections), it points to a L2 table.
 *
 * Layout of an ARM32 L2 (PTE) page (4KB total):
 *
 * Offset 0x000–0x7FF: Linux (software) PTEs  [512 × 4B]
 * Offset 0x800–0xFFF: Hardware PTEs           [512 × 4B]
 *
 * Hardware PTE count: 512 per 2MB = 4KB pages (correct: 2MB/4KB = 512)
 */
#define PTRS_PER_PTE    512
#define PTRS_PER_PMD    1
#define PTRS_PER_PGD    2048

#define PTE_HWTABLE_PTRS    (PTRS_PER_PTE)
#define PTE_HWTABLE_OFF     (PTE_HWTABLE_PTRS * sizeof(pte_t))
#define PTE_HWTABLE_SIZE    (PTRS_PER_PTE * sizeof(u32))
```

### 2.4 PGD Walk — Step by Step

```c
/*
 * Walking from mm->pgd to physical address for VA=0x1234ABCD
 *
 * VA breakdown (3G/1G, TTBCR.N=2):
 *   [31:21] → PGD index = 0x1234ABCD >> 21 = 0x91 (145)
 *   [20:12] → PTE index = (0x1234ABCD >> 12) & 0x1FF = 0x34 (52)
 *   [11:0]  → Page offset = 0xBCD
 */

pgd_t *pgd = pgd_offset(mm, va);   /* mm->pgd + (va >> PGDIR_SHIFT) */
pmd_t *pmd = pmd_offset(pgd, va);  /* same as pgd on ARM32 (pmd folded) */
pte_t *pte = pte_offset_map(pmd, va);

/* pte_offset_map returns the SOFTWARE pte (first 2KB half) */
/* Hardware PTE = pte + PTRS_PER_PTE */

physical = (*pte & PAGE_MASK) | (va & ~PAGE_MASK);
```

### 2.5 Section Mappings in Linux

Linux kernel uses 1MB section entries for its direct mapping (lowmem):

```c
/* arch/arm/mm/mmu.c */
static void __init create_mapping(struct map_desc *md)
{
    unsigned long addr = md->virtual;
    unsigned long end  = addr + md->length;
    pgd_t *pgd;

    pgd = pgd_offset_k(addr);   /* kernel PGD from init_mm.pgd */

    do {
        unsigned long next = pgd_addr_end(addr, end);

        if (cpu_architecture() >= CPU_ARCH_ARMv6 &&
            (addr | next | md->pfn) & ~SECTION_MASK) {
            /* Use 4KB pages */
            alloc_init_pte(pgd, addr, next, md->pfn, md->type);
        } else {
            /* Use 1MB sections — faster, fewer TLB entries */
            alloc_init_section(pgd, addr, next, md->pfn, md->type);
        }

        pgd++;
        addr = next;
    } while (addr != end);
}
```

---

## 3. mm_struct and vm_area_struct

### 3.1 mm_struct — Process Memory Descriptor

```c
struct mm_struct {
    struct vm_area_struct   *mmap;          /* VMA linked list (sorted by VA) */
    struct rb_root          mm_rb;          /* VMA red-black tree (fast lookup) */
    unsigned long           mmap_base;      /* mmap start address */
    unsigned long           task_size;      /* max user VA (PAGE_OFFSET) */
    pgd_t                   *pgd;           /* page table base (TTBR0 value) */
    atomic_t                mm_users;       /* threads using this mm */
    atomic_t                mm_count;       /* references to mm_struct */
    unsigned long           start_code;     /* .text start */
    unsigned long           end_code;       /* .text end */
    unsigned long           start_data;     /* .data start */
    unsigned long           end_data;       /* .data end */
    unsigned long           start_brk;      /* heap start */
    unsigned long           brk;            /* current heap top */
    unsigned long           start_stack;    /* stack start */
    unsigned long           arg_start;      /* argv start */
    unsigned long           env_start;      /* env start */
    mm_context_t            context;        /* ARM: ASID + generation counter */
    /* ... */
};
```

### 3.2 vm_area_struct — Virtual Memory Area

```c
struct vm_area_struct {
    struct mm_struct    *vm_mm;         /* owning mm */
    unsigned long        vm_start;      /* VMA start VA (inclusive) */
    unsigned long        vm_end;        /* VMA end VA (exclusive) */
    struct vm_area_struct *vm_next;     /* sorted linked list */
    struct vm_area_struct *vm_prev;
    struct rb_node       vm_rb;         /* red-black tree node */
    pgprot_t             vm_page_prot;  /* page protection (from flags) */
    unsigned long        vm_flags;      /* VM_READ, VM_WRITE, VM_EXEC, etc. */
    struct file         *vm_file;       /* mapped file (NULL for anonymous) */
    unsigned long        vm_pgoff;      /* file offset (in pages) */
    const struct vm_operations_struct *vm_ops;
};

/* Key vm_flags */
#define VM_READ    0x00000001   /* readable */
#define VM_WRITE   0x00000002   /* writable */
#define VM_EXEC    0x00000004   /* executable */
#define VM_SHARED  0x00000008   /* shared mapping */
#define VM_MAYWRITE 0x00000020  /* writable with mprotect */
#define VM_GROWSDOWN 0x00000100 /* stack VMA, grows down */
#define VM_IO      0x00004000   /* MMIO mapping */
#define VM_PFNMAP  0x00000400   /* no struct page (pfn map) */
#define VM_DONTCOPY 0x00020000  /* don't copy on fork */
```

### 3.3 mmap() System Call Flow

```
User: mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)

Kernel path:
  syscall mmap2
    → ksys_mmap_pgoff()
      → vm_mmap_pgoff()
        → do_mmap()
          → get_unmapped_area()    ← find free VA range
          → mmap_region()
            → vma_merge()          ← try to extend existing VMA
            → vm_area_alloc()      ← allocate new vm_area_struct
            → vma_set_page_prot()  ← compute vm_page_prot from flags
            → vma_link()           ← insert into mm->mmap list + rb tree
            → (no pages mapped yet — demand paging)
```

> **Key insight:** `mmap()` does **not** allocate physical pages. It only creates a `vm_area_struct`. Physical pages are allocated on first access via page fault.

---

## 4. Page Fault Handling

### 4.1 Hardware Fault Entry

```
ARM32 Page Fault triggers Data Abort or Prefetch Abort:

Data Abort (load/store fault):
  CPU → enters abort mode
  → saves LR_abt = faulting instruction + 8
  → jumps to vector_dabt (0xFFFF0010 with high vectors)

  Saved registers: DFSR (fault status), DFAR (fault address)

Prefetch Abort (instruction fetch fault):
  → jumps to vector_pabt (0xFFFF000C)
  → Saved: IFSR, IFAR
```

### 4.2 Linux Fault Handler Chain

```
vector_dabt (assembly)
  → __dabt_usr / __dabt_svc  (save regs, get DFSR/DFAR)
    → do_DataAbort()
      → fsr_info[] table lookup (indexed by FSR fault type bits)
        → do_page_fault()       ← main handler
            ↓
        find_vma(mm, addr)     ← is addr in a known VMA?
            ↓
        [NO VMA] → do_sigbus() or SIGSEGV
        [VMA found] → handle_mm_fault()
            ↓
        handle_pte_fault()
            ├─ [PTE not present] → do_fault()
            │     ├─ [anon]  → do_anonymous_page()
            │     ├─ [file]  → do_read_fault() / do_cow_fault()
            │     └─ [swap]  → do_swap_page()
            ├─ [write fault, COW] → do_wp_page()
            └─ [spurious]   → update access flags, return
```

### 4.3 Anonymous Page Fault (Heap / Stack)

```c
static vm_fault_t do_anonymous_page(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct page *page;
    pte_t entry;

    /* Check permissions */
    if (!(vma->vm_flags & VM_WRITE) && vmf->flags & FAULT_FLAG_WRITE)
        return VM_FAULT_SIGBUS;

    /* For read faults: map zero page (shared, read-only) */
    if (!(vmf->flags & FAULT_FLAG_WRITE) && !mm_forbids_zeropage(vma->vm_mm)) {
        entry = pte_mkspecial(pfn_pte(my_zero_pfn(vmf->address),
                                       vma->vm_page_prot));
        vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd,
                                        vmf->address, &vmf->ptl);
        set_pte_at(vma->vm_mm, vmf->address, vmf->pte, entry);
        goto unlock;
    }

    /* For write faults: allocate a real page */
    page = alloc_zeroed_user_highpage_movable(vma, vmf->address);
    if (!page)
        return VM_FAULT_OOM;

    entry = mk_pte(page, vma->vm_page_prot);
    entry = pte_sw_mkyoung(entry);
    if (vma->vm_flags & VM_WRITE)
        entry = pte_mkwrite(pte_mkdirty(entry));

    /* Install PTE — populates both software and hardware PTEs */
    set_pte_at(vma->vm_mm, vmf->address, vmf->pte, entry);

unlock:
    pte_unmap_unlock(vmf->pte, vmf->ptl);
    return 0;
}
```

### 4.4 set_pte_at() — ARM32 Dual PTE Write

```c
/* arch/arm/include/asm/pgtable.h */
static inline void set_pte_at(struct mm_struct *mm, unsigned long addr,
                                pte_t *ptep, pte_t pteval)
{
    /*
     * ARM32: ptep points to SOFTWARE PTE (first 2KB half of page)
     * Hardware PTE is at ptep + PTRS_PER_PTE (2KB up)
     */
    pte_t *hwpte = ptep + PTRS_PER_PTE;

    /* Write Linux software PTE (stores dirty/young/write bits) */
    *ptep = pteval;

    /* Translate Linux PTE flags to ARM hardware PTE flags */
    *hwpte = linux_pte_to_hardware(mm, addr, pteval);

    /* Ensure hardware PTE is visible before TLB fill */
    flush_pte_entry(addr, hwpte);
}
```

### 4.5 Copy-on-Write (COW) — Write Fault on Read-Only PTE

```
fork() creates child with same PTEs but marked read-only:

Parent writes to page → Page Fault (permission fault)
  → do_wp_page()
    → is_cow_mapping() → YES
    → PageAnon(page) → YES
    → page_count(page) == 1? → NO (shared with parent)
    → Copy page: alloc_page(), copy_user_highpage()
    → Unmap old entry, map new writable entry
    → Return → CPU retries write → succeeds

Parent writes to page → Page Fault (permission fault)
  → do_wp_page()
    → page_count(page) == 1? → YES (child already forked away)
    → Just make PTE writable (reuse same page)
    → wp_page_reuse()
```

---

## 5. Context Switch — TTBR0/ASID Update

### 5.1 switch_mm() — The Critical Path

```c
/* arch/arm/include/asm/mmu_context.h */
static inline void
switch_mm(struct mm_struct *prev, struct mm_struct *next,
          struct task_struct *tsk)
{
    unsigned int cpu = smp_processor_id();

    if (!cpumask_test_and_set_cpu(cpu, mm_cpumask(next)) || prev != next) {
        check_and_switch_context(next, tsk);

#ifdef CONFIG_MMU
        if (cache_is_vivt()) {
            /* VIVT cache: must flush D-cache on switch to prevent aliasing */
            vivt_switch_mm(prev, next, tsk);
        }
#endif
    }
}
```

### 5.2 check_and_switch_context() — ASID Management

```c
/* arch/arm/mm/context.c */
void check_and_switch_context(struct mm_struct *mm, struct task_struct *tsk)
{
    unsigned long flags;
    unsigned int cpu;
    u64 asid;

    /* Fast path: ASID still valid for current generation */
    asid = atomic64_read(&mm->context.id);
    if (!((asid ^ atomic64_read(&asid_generation)) >> ASID_BITS)
        && atomic64_xchg(&per_cpu(active_asids, cpu), asid))
        goto switch_mm_fastpath;

    /* Slow path: allocate new ASID */
    raw_spin_lock_irqsave(&cpu_asid_lock, flags);
    asid = new_context(mm, cpu);   /* may trigger global TLB flush if generation wraps */
    raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

switch_mm_fastpath:
    cpu_switch_mm(mm->pgd, mm);   /* Write TTBR0 + CONTEXTIDR */
}
```

### 5.3 cpu_switch_mm() — TTBR0 Write Sequence

```assembly
/* arch/arm/mm/proc-v7.S */
ENTRY(cpu_v7_switch_mm)
    /* r0 = pgd phys addr, r1 = mm pointer */
    mmid    r1, r1              @ r1 = mm->context.id (ASID in [7:0])
    ALT_SMP(orr r0, r0, #TTB_FLAGS_SMP)
    ALT_UP(orr  r0, r0, #TTB_FLAGS_UP)

    /* Drain write buffer before updating TTBR0 */
    dsb     ish

    /* Write new CONTEXTIDR (ASID) first */
    mcr     p15, 0, r1, c13, c0, 1  @ CONTEXTIDR = ASID | PROCID

    /* Instruction barrier after ASID update */
    isb

    /* Write new TTBR0 (page table base) */
    mcr     p15, 0, r0, c2, c0, 0   @ TTBR0 = pgd_phys

    /* Instruction barrier — ensure new mappings take effect */
    isb

    mov     pc, lr
ENDPROC(cpu_v7_switch_mm)
```

> **Interview Critical:** Why write CONTEXTIDR before TTBR0? Because the ASID must be valid before the new page table is active. A window where TTBR0 is updated but ASID is stale can cause the CPU to use old TLB entries tagged with wrong ASID for the new process. The `isb` between them ensures the ASID write completes before TTBR0 takes effect.

### 5.4 ASID Generation and Rollover

```
ARM32: 8-bit ASID → only 256 unique ASIDs

Strategy:
┌─────────────────────────────────────────────────────┐
│ asid_generation: 64-bit counter (upper bits = gen)  │
│ mm->context.id: generation[63:8] | ASID[7:0]        │
│                                                       │
│ Valid if: (mm->context.id >> 8) == (asid_generation >> 8) │
└─────────────────────────────────────────────────────┘

Rollover (every 256 context switches when all ASIDs used):
1. Increment asid_generation (upper bits)
2. Flush entire TLB on all CPUs (IPI broadcast)
3. Reassign ASIDs to currently running tasks
4. Mark all other mm->context.id stale

Cost: Full TLB flush → all TLB misses on next round
```

---

## 6. Memory Zones and the Page Allocator

### 6.1 ARM32 Memory Zones

```
Physical memory zones (typical ARM32 with 1GB RAM, PAGE_OFFSET=0xC0000000):

ZONE_DMA       0x00000000 – 0x00FFFFFF  (first 16MB, legacy DMA)
ZONE_NORMAL    0x01000000 – highmem_start (direct-mapped low memory)
ZONE_HIGHMEM   highmem_start – RAM_end   (cannot be directly mapped)

ARM32 with 3G/1G split: Kernel direct map window = 1GB
→ If RAM > 1GB: ZONE_HIGHMEM exists for pages above 0xC0000000 + 1GB

Typical Qualcomm SoC (2GB RAM, PAGE_OFFSET=0x80000000):
  ZONE_DMA     = first 16MB
  ZONE_NORMAL  = 16MB – 2GB (all directly mapped, no highmem)
```

### 6.2 Buddy Allocator

```
Order-0 to Order-10 free lists (4KB to 4MB blocks):

alloc_pages(GFP_KERNEL, order)
  → __alloc_pages_nodemask()
    → get_page_from_freelist()
      → rmqueue()                  ← remove from free list
        → __rmqueue_smallest()     ← find smallest satisfying block
          → expand()               ← split larger block if needed
    → [if fail] __alloc_pages_slowpath()
      → reclaim, OOM killer
```

### 6.3 Slab / SLUB Allocator

```c
/* Per-CPU cache of frequently allocated kernel objects */

/* Create a cache */
struct kmem_cache *cache = kmem_cache_create("my_obj", sizeof(struct my_obj),
                                              0, SLAB_HWCACHE_ALIGN, NULL);

/* Alloc/free */
struct my_obj *obj = kmem_cache_alloc(cache, GFP_KERNEL);
kmem_cache_free(cache, obj);

/* kmalloc uses pre-created size caches (8B, 16B, 32B, ... 4MB) */
void *p = kmalloc(64, GFP_KERNEL);   /* uses kmalloc-64 cache */
```

---

## 7. Highmem: PKMAP and FIXMAP

> **Highmem** is memory above the kernel's direct-map window. On ARM32 with 3G/1G, any RAM above ~896MB (traditional x86 limit) or up to 1GB needs temporary kernel virtual mappings.

### 7.1 PKMAP (Permanent Kernel Map)

```c
/* include/linux/highmem.h */
/*
 * PKMAP region: PKMAP_BASE = 0xFF000000 (just below fixmap)
 * LAST_PKMAP = 512 entries (2MB window for highmem pages)
 *
 * kmap() — map a highmem page permanently (until kunmap)
 *         Sleepable (may block waiting for PKMAP slot)
 */
void *kmap(struct page *page)
{
    if (!PageHighMem(page))
        return page_address(page);  /* lowmem: direct map */

    return kmap_high(page);         /* highmem: find PKMAP slot */
}

/* kmap_high() internals: */
/*   1. Check if page already mapped in pkmap_page_table */
/*   2. If not: find free PKMAP slot (blocks if full) */
/*   3. Set PKMAP PTE, flush TLB for that VA */
/*   4. Return VA */
```

### 7.2 FIXMAP (Fixed Virtual Addresses)

```c
/* arch/arm/include/asm/fixmap.h */
/*
 * Fixed virtual addresses — compile-time constants near top of kernel space.
 * Used for: early console (UART), FDT mapping, kmap_atomic slots
 */
enum fixed_addresses {
    FIX_EARLYCON_MEM_BASE,
    FIX_TEXT_POKE0,
    FIX_TEXT_POKE1,
    FIX_KMAP_BEGIN,        /* per-CPU kmap_atomic slots */
    FIX_KMAP_END = FIX_KMAP_BEGIN + (KM_TYPE_NR * NR_CPUS) - 1,
    __end_of_fixed_addresses
};

#define fix_to_virt(x)  (FIXADDR_TOP - ((x) + 1) * PAGE_SIZE)
```

### 7.3 kmap_atomic() — Interrupt-Safe Highmem Mapping

```c
/*
 * kmap_atomic(): Temporary highmem mapping, non-sleepable, disables preemption
 * Uses per-CPU FIXMAP slots → no TLB shootdown needed
 */
void *kmap_atomic(struct page *page)
{
    unsigned int idx;
    unsigned long vaddr;
    void *kmap;

    preempt_disable();
    pagefault_disable();

    if (!PageHighMem(page))
        return page_address(page);

    idx = KM_TYPE_NR * smp_processor_id() + kmap_atomic_idx_push();
    vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);

    /* Map the page */
    set_pte_ext(TOP_PTE(vaddr),
                pfn_pte(page_to_pfn(page), kmap_prot),
                0);
    local_flush_tlb_kernel_page(vaddr);

    return (void *)vaddr;
}
```

---

## 8. vmalloc and ioremap

### 8.1 vmalloc — Virtually Contiguous, Physically Scattered

```
vmalloc region: VMALLOC_START (0xC0800000) → VMALLOC_END (0xFF000000)
  ~240MB available for vmalloc/ioremap

Use cases:
  - Large kernel allocations (>PAGE_SIZE) where physical contiguity not needed
  - Module text/data sections
  - Stack for kernel threads (THREAD_SIZE = 8KB, vmalloc for large stacks)

NOT suitable for:
  - DMA (physically scattered)
  - Performance-critical paths (extra TLB pressure vs kmalloc)
```

```c
void *vmalloc(unsigned long size)
{
    /* 1. Allocate virtual address range from vmalloc_area */
    struct vm_struct *area = get_vm_area(size, VM_ALLOC);

    /* 2. Allocate physical pages (order-0, scattered) */
    /* 3. Map each page into the vmalloc PGD entries */
    /* 4. Kernel PGD (init_mm.pgd) updated */
    /* 5. Other CPUs see updates via vmalloc_fault or pgd_alloc */

    return area->addr;
}
```

**vmalloc_fault:** When a CPU accesses a vmalloc address not yet in its local PGD, a page fault fires. The fault handler propagates the mapping from init_mm.pgd to the current mm.pgd — this is valid in kernel space because all processes share the kernel PGD upper half.

### 8.2 ioremap — Map Device Registers

```c
/*
 * ioremap variants for ARM32:
 */
void __iomem *ioremap(phys_addr_t phys, size_t size)
    /* Device memory, Strongly-Ordered, no cache */
    /* Uses ioremap_prot() with MT_DEVICE */

void __iomem *ioremap_wc(phys_addr_t phys, size_t size)
    /* Write-Combining (Normal, Non-cacheable) */
    /* Better for framebuffers */

void __iomem *ioremap_cache(phys_addr_t phys, size_t size)
    /* Cacheable (for memory-like peripherals) */
    /* Dangerous: needs explicit cache maintenance */

/* Under the hood: */
static void __iomem *__ioremap_pfn_caller(unsigned long pfn,
    unsigned long offset, size_t size, pgprot_t prot)
{
    area = get_vm_area_caller(size, VM_IOREMAP, caller);
    addr = area->addr;
    ioremap_page_range(addr, addr + size, phys, prot);
    /* Sets ARM MT_DEVICE: TEX=0, C=0, B=1 in PTEs */
    return (void __iomem *)(offset + (char *)addr);
}
```

---

## 9. Copy-on-Write and Demand Paging

### 9.1 fork() — Page Table Duplication

```c
/* kernel/fork.c: copy_process() → copy_mm() → dup_mm() */

/*
 * ARM32 fork() page table copy:
 * 1. Allocate new 16KB PGD for child
 * 2. Copy kernel PGD entries (upper half: same for all processes)
 * 3. For user entries: copy PGD entry AND underlying PTE table
 * 4. Mark all user writable PTEs as read-only in BOTH parent and child
 * 5. Increment page reference counts
 *
 * Result: Parent and child share all physical pages, all read-only
 *         First write by either → page fault → COW copy
 */

static int copy_page_range(struct mm_struct *dst_mm, struct mm_struct *src_mm,
                            struct vm_area_struct *vma)
{
    /* Walk src PGD → PMD → PTE */
    /* For each present PTE: */
    /*   - If writable: clear write bit in src PTE */
    /*   - Copy PTE value (now read-only) to dst */
    /*   - Increment page->_refcount */
    /* Flush TLB for write-protected parent PTEs */
}
```

### 9.2 Swap and Page Reclaim

```
swap PTE encoding on ARM32:
┌────────────────────────────────────────────────────────────┐
│ swap_type[4:0] | swap_offset[31:5] | ... | Present=0       │
└────────────────────────────────────────────────────────────┘

Page reclaim (kswapd):
  1. select_victim_page() — LRU eviction
  2. try_to_unmap() — walk reverse maps, clear PTEs
  3. add_to_swap() — write page to swap device
  4. Set swap PTE (type + offset, Present=0)
  5. TLB invalidate for the unmapped VA

Page-in (swap fault):
  do_swap_page()
    → lookup_swap_cache()    — in swap cache?
    → swapin_readahead()     — read from swap device
    → set_pte_at() with new page
    → free_swap_and_cache()  — may release swap slot
```

---

## 10. Kernel Direct Mapping

### 10.1 Physical to Kernel Virtual

```c
/* Lowmem pages are directly mapped: PA → KVA is a fixed offset */

#define __va(x)  ((void *)((unsigned long)(x) + PAGE_OFFSET - PHYS_OFFSET))
#define __pa(x)  ((unsigned long)(x) - PAGE_OFFSET + PHYS_OFFSET)

/* Example (PAGE_OFFSET=0xC0000000, PHYS_OFFSET=0x80000000): */
/* PA=0x80001000 → KVA = 0x80001000 - 0x80000000 + 0xC0000000 = 0xC0001000 */

/* page_address(): return KVA for struct page */
static inline void *page_address(const struct page *page)
{
    if (!PageHighMem(page))
        return __va(page_to_pfn(page) << PAGE_SHIFT);
    return page_address_in_highmem(page);  /* PKMAP lookup */
}
```

### 10.2 ARM32 Kernel Mapping at Boot

```c
/* arch/arm/mm/mmu.c: map_lowmem() */
static void __init map_lowmem(void)
{
    struct memblock_region *reg;

    for_each_memblock(memory, reg) {
        phys_addr_t start = reg->base;
        phys_addr_t end   = start + reg->size;

        /* Map each RAM region into kernel direct map */
        create_mapping({
            .virtual = __phys_to_virt(start),
            .pfn     = __phys_to_pfn(start),
            .length  = end - start,
            .type    = MT_MEMORY_RWX,   /* Normal, cacheable, RWX */
        });
    }
}
```

---

## 11. ARM-Specific MM Files in Linux Source

| File | Purpose |
|------|---------|
| `arch/arm/mm/mmu.c` | Boot-time page table setup, map_lowmem, create_mapping |
| `arch/arm/mm/pgalloc.h` | PGD/PTE allocation, pte_alloc_one |
| `arch/arm/mm/context.c` | ASID management, check_and_switch_context |
| `arch/arm/mm/fault.c` | do_page_fault, do_DataAbort, FSR decode table |
| `arch/arm/mm/proc-v7.S` | cpu_v7_switch_mm, TLB ops, cache ops |
| `arch/arm/mm/cache-v7.S` | L1 cache flush (by set/way, by MVA) |
| `arch/arm/mm/highmem.c` | kmap_atomic for ARM |
| `arch/arm/mm/ioremap.c` | ioremap, ioremap_wc implementation |
| `arch/arm/include/asm/pgtable-2level.h` | PTE/PMD/PGD type definitions |
| `arch/arm/include/asm/mmu_context.h` | switch_mm, deactivate_mm |
| `arch/arm/kernel/head.S` | Pre-MMU boot, identity map, MMU enable |

---

## 12. Common Bugs and Debug Techniques

### 12.1 Accessing Freed Pages (Use-After-Free)

```
Symptoms: Random kernel panics, DFAR points to valid-looking address
Debug:
  - Enable CONFIG_DEBUG_PAGEALLOC (poisons freed pages)
  - Enable CONFIG_KASAN (ARM32 KASAN support limited but available)
  - Check DFSR for permission fault vs translation fault
```

### 12.2 vmalloc_fault Loop

```
Symptom: Repeated page faults in vmalloc region, system hangs
Cause: vmalloc area PGD entry not propagated to current PGD
Fix: Ensure vmalloc_fault() properly copies from init_mm.pgd
     Check for concurrent free+remap of vmalloc area (use vmalloc_lock)
```

### 12.3 TLB Coherency Bug

```c
/* BUG: Missing TLB flush after PTE change */
void buggy_remap(unsigned long va, phys_addr_t pa) {
    pte_t *pte = get_pte(va);
    set_pte(pte, make_pte(pa, PAGE_KERNEL));
    /* Missing: flush_tlb_page(vma, va) */
    /* CPU still uses old TLB entry → stale mapping */
}

/* FIX: */
void correct_remap(unsigned long va, phys_addr_t pa) {
    pte_t *pte = get_pte(va);
    set_pte_at(mm, va, pte, make_pte(pa, PAGE_KERNEL));
    flush_tlb_page(vma, va);   /* Invalidate stale TLB entry */
}
```

### 12.4 Cache Aliasing with VIVT Caches (Cortex-A8)

```
Cortex-A8 has VIVT L1 D-cache → aliasing if two VAs map same PA
Symptom: Data corruption between processes sharing COW pages
Kernel mitigation: flush_cache_page() before COW break
                   flush_cache_mm() on context switch
ARM Cortex-A9+: PIPT → no aliasing issue
```

### 12.5 Reading Fault Registers from User Space (Debug)

```bash
# From /proc/<pid>/maps — VMA layout
cat /proc/self/maps

# From /proc/<pid>/smaps — per-VMA detailed stats
cat /proc/self/smaps

# Page table dump (requires CONFIG_PROC_PAGE_MONITOR)
cat /proc/<pid>/pagemap

# Kernel fault log from oops:
# pgd = <pgd_addr>
# [<va>] *pgd=<pgd_val>, *pte=<pte_val>
# FSR: <fault_status_bits>
```

---

## Summary

| Topic | Key File | Key Function |
|-------|----------|--------------|
| Boot page tables | `arch/arm/mm/mmu.c` | `create_mapping()` |
| Context switch | `arch/arm/mm/context.c` | `check_and_switch_context()` |
| Page fault | `arch/arm/mm/fault.c` | `do_page_fault()` |
| PTE install | `arch/arm/include/asm/pgtable.h` | `set_pte_at()` |
| TTBR0 write | `arch/arm/mm/proc-v7.S` | `cpu_v7_switch_mm` |
| Highmem | `arch/arm/mm/highmem.c` | `kmap_atomic()` |
| ioremap | `arch/arm/mm/ioremap.c` | `ioremap()` |

---

**Cross-References:**
- Doc 01: MMU registers (TTBR0/1, TTBCR, CONTEXTIDR)
- Doc 02: Boot-time identity map and MMU enable
- Doc 04: ASID rollover and TLB shootdown details
- Doc 05: Cache maintenance in context switch and COW
- Doc 07: SMMU/IOMMU for DMA-mapped memory

---
**End of Document 3**
