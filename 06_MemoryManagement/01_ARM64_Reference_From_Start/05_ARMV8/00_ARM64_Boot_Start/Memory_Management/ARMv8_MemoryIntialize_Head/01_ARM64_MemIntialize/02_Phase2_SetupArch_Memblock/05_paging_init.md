# `paging_init()` — Final Page Table Creation

**Source:** `arch/arm64/mm/mmu.c` lines 1446–1457
**Phase:** Memblock Era
**Memory Allocator:** Memblock (used to allocate page table pages)
**Called by:** `setup_arch()`
**Calls:** `map_mem()`, `memblock_allow_resize()`, `create_idmap()`, `declare_kernel_vmas()`

---

## What This Function Does

Creates the **final kernel page tables** in `swapper_pg_dir` that map **all physical RAM** into the kernel's linear map. After this function:

- Every byte of physical RAM is accessible via the linear map
- TTBR1_EL1 points to `swapper_pg_dir` (replacing `init_pg_dir`)
- The kernel can convert between physical and virtual addresses using simple arithmetic

---

## How It Works With Memory

### Memory It Allocates (from memblock)

| What | Approximate Size | Purpose |
|------|-----------------|---------|
| PGD page | 4 KB | Top-level `swapper_pg_dir` |
| PUD pages | 4 KB each | Level 1 tables |
| PMD pages | 4 KB each | Level 2 tables |
| PTE pages | 4 KB each (if needed) | Level 3 tables |

For a 4GB system with 4KB pages:
- With 1GB PUD block mappings: ~3 PUD entries + PGD = ~8 KB
- With 2MB PMD block mappings: ~2048 PMD entries + PUD + PGD = ~20 KB
- With 4KB PTE mappings: much more (for sections requiring fine-grained permissions)

### Memory It Maps

All `memblock.memory` regions that don't have `MEMBLOCK_NOMAP` flag.

---

## Step-by-Step Execution

### Step 1: `map_mem(swapper_pg_dir)` — Map All RAM

```c
void __init paging_init(void)
{
    map_mem(swapper_pg_dir);
```

**See:** [06_map_mem.md](06_map_mem.md) for full details.

**Summary:** Iterates all memblock memory regions and creates page table entries mapping physical addresses to virtual addresses in the linear map range.

---

### Step 2: `memblock_allow_resize()` — Enable Dynamic Array Growth

```c
    memblock_allow_resize();
```

**Before this call:** If `memblock.memory` or `memblock.reserved` arrays need more than 128 entries, the kernel panics.

**After this call:** The arrays can be dynamically doubled by allocating new arrays from memblock itself.

**Why wait until now?**
- `memblock_alloc()` needs page tables to convert the returned physical address to a virtual address (for `memset()`, etc.)
- Before `paging_init()`, only the kernel image is mapped
- After `map_mem()`, all RAM is mapped, so any memblock allocation result has a valid virtual address via the linear map

---

### Step 3: `create_idmap()` — Recreate Identity Map

```c
    create_idmap();
```

Creates a fresh identity mapping in `idmap_pg_dir` (separate from the init_idmap used during boot). This identity map is needed for:

- **CPU hotplug** — bringing up secondary CPUs (they start with MMU off)
- **Suspend/resume** — CPU re-enters at physical addresses after resume
- **Hibernate** — switching page tables during hibernation restore

The identity map covers `__idmap_text_start` to `__idmap_text_end` — a small section of code that runs during CPU bringup and mode switches.

---

### Step 4: `declare_kernel_vmas()` — Register Kernel VM Areas

```c
    declare_kernel_vmas();
}
```

Registers the kernel's virtual address regions as `vm_struct` entries in the vmalloc/vmap area list. This tells the VM subsystem about kernel text, data, and other pre-mapped regions.

```c
static void __init declare_kernel_vmas(void)
{
    // Kernel text: RO+X
    declare_vma(&kernel_text, _text, __inittext_end, VM_NO_GUARD);

    // Kernel rodata: RO+NX
    declare_vma(&kernel_rodata, __start_rodata, __initdata_begin, VM_NO_GUARD);

    // Kernel init text+data: RW (freed after init)
    declare_vma(&kernel_inittext, __inittext_begin, __initdata_begin, VM_NO_GUARD);
    declare_vma(&kernel_initdata, __initdata_begin, __init_end, VM_NO_GUARD);

    // Kernel data+BSS: RW+NX
    declare_vma(&kernel_data, _data, _end, VM_NO_GUARD);
}
```

These `vm_struct` entries are imported into the vmalloc subsystem during `vmalloc_init()` (Phase 4), so vmalloc knows these VA ranges are already in use.

---

## Page Table Switch: init_pg_dir → swapper_pg_dir

After `map_mem()` populates `swapper_pg_dir`, the kernel switches TTBR1:

```c
// Inside map_mem() or immediately after:
cpu_replace_ttbr1(swapper_pg_dir, idmap_pg_dir);
```

**`cpu_replace_ttbr1()` algorithm:**

```
1. Switch to identity map (TTBR0 = idmap_pg_dir)
2. Switch TTBR1 from init_pg_dir to swapper_pg_dir
3. TLB invalidate (tlbi vmalle1)
4. Switch back from identity map
5. init_pg_dir is now unused
```

**Why use identity map during switch?**
- When TTBR1 is being replaced, kernel VA translations are briefly invalid
- The identity map (TTBR0) provides a stable mapping for the currently executing code
- After TTBR1 is updated and TLB flushed, kernel VAs work with the new page tables

---

## Page Table Hierarchy After paging_init()

```
swapper_pg_dir (TTBR1_EL1):
├── PGD[0]: → PUD table
│   ├── PUD[0]: BLOCK → PA 0x0000_0000 (1GB, if RAM exists)
│   ├── PUD[1]: BLOCK → PA 0x4000_0000 (1GB block mapping)
│   ├── PUD[2]: BLOCK → PA 0x8000_0000 (1GB block mapping)
│   └── PUD[3]: BLOCK → PA 0xC000_0000 (1GB block mapping)
│                                        ↑ 4GB of RAM mapped with 4 PUD entries!
├── PGD[1]: → PUD table
│   ├── PUD[0]: BLOCK → PA 0x1_0000_0000
│   └── ...
│
├── PGD[N]: → PUD table (for kernel image, vmalloc, vmemmap, etc.)
│   └── ...
│
└── PGD[511]: → PUD table (fixmap region)
    └── ...
```

With 1GB PUD block mappings and 4KB pages, each PGD entry covers 512GB, and each PUD entry covers 1GB. A 4GB system needs only a few PUD entries.

---

## Virtual Address Space After paging_init()

```
0xFFFF_FFFF_FFFF_FFFF ┐
                       │ Fixmap (FDT, early console, etc.)
0xFFFF_FFFE_0000_0000 ┤
                       │ PCI I/O space
0xFFFF_FFFD_0000_0000 ┤
                       │ vmemmap (not populated yet)
0xFFFF_FFFC_0000_0000 ┤
                       │ vmalloc / ioremap (not populated yet)
0xFFFF_FF80_0800_0000 ┤
                       │ Kernel image (text, data, BSS)
0xFFFF_FF80_0000_0000 ┤
                       │ ┌────────────────────────────────────┐
                       │ │ LINEAR MAP — ALL RAM MAPPED HERE   │ ← NEW!
                       │ │ VA = PA - memstart_addr + PAGE_OFF │
                       │ │                                    │
                       │ │ Every physical page accessible     │
                       │ │ via simple address arithmetic      │
                       │ └────────────────────────────────────┘
0xFFFF_0000_0000_0000 ┘ PAGE_OFFSET
```

---

## Key Takeaways

1. **`swapper_pg_dir` is the permanent kernel page table** — used for the rest of the system's lifetime
2. **Block mappings for efficiency** — 1GB PUD blocks or 2MB PMD blocks minimize page table memory
3. **Identity map is kept** — needed for CPU hotplug, suspend/resume, hibernate
4. **Kernel VMAs registered** — prepares for vmalloc to know which VA ranges are already used
5. **memblock resize enabled** — now that all RAM is mapped, memblock can safely allocate from anywhere
6. **Page table pages from memblock** — this is one of the first significant uses of `memblock_alloc()`
