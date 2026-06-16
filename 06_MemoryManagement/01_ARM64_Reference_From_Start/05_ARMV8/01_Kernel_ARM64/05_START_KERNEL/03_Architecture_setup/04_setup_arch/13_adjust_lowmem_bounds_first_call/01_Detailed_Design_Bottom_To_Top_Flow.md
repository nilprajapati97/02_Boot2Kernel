# adjust_lowmem_bounds() — First Call: Detailed Design

## 1. Position in setup_arch() Boot Sequence

```
setup_arch()
  ├── arm_efi_init()               ← EFI memory regions established
  └── adjust_lowmem_bounds()       ← *** FIRST CALL *** (line 1158)
        │ Comment: "Make sure the calculation for lowmem/highmem is set
        │  appropriately before reserving/allocating any memory"
        ├── compute vmalloc_limit from vmalloc_size
        ├── scan memblock for valid regions
        ├── set arm_lowmem_limit
        ├── set high_memory
        └── set memblock current limit
```

This is the **first** of two calls to `adjust_lowmem_bounds()`. Its role is to establish an initial lowmem/highmem boundary based on the vmalloc window size constraint, **before** any memory reservations are made. The second call (after `arm_memblock_init()`) recalculates because reservations may have changed available memory.

---

## 2. The Core Problem: 32-bit Virtual Address Crunch

ARM32 has a **32-bit virtual address space** (4GB). This must accommodate:
- User space: typically 0x00000000 – 0xBF000000 (3GB)
- Kernel code + data: above PAGE_OFFSET (0xC0000000)
- Direct-mapped physical RAM (lowmem): must fit in kernel VA above PAGE_OFFSET
- **vmalloc window**: needed for ioremap, vmalloc, module loading
- highmem: physical RAM beyond the lowmem limit (not permanently mapped)

If RAM is large (e.g., 1GB), naively mapping it all as lowmem would leave no room for vmalloc. `adjust_lowmem_bounds()` computes the highest physical address that can be mapped as lowmem without colliding with the vmalloc window.

---

## 3. Source Code Deep Dive

**File:** `arch/arm/mm/mmu.c`

```c
phys_addr_t arm_lowmem_limit __initdata = 0;

void __init adjust_lowmem_bounds(void)
{
    phys_addr_t block_start, block_end, memblock_limit = 0;
    u64 vmalloc_limit, i;
    phys_addr_t lowmem_limit = 0;
```

### Step 1: Compute vmalloc_limit

```c
    vmalloc_limit = (u64)VMALLOC_END - vmalloc_size - VMALLOC_OFFSET -
                    PAGE_OFFSET + PHYS_OFFSET;
```

This computes the **physical address** above which we must not directly map memory (because that virtual range is needed for vmalloc).

**Variables:**
- `VMALLOC_END`: The end of the vmalloc region in virtual space (typically 0xFF800000)
- `vmalloc_size`: Size of vmalloc window (default 240MB, changed by `vmalloc=` param)
- `VMALLOC_OFFSET`: 8MB guard gap before vmalloc starts (prevents overflows into vmalloc)
- `PAGE_OFFSET`: Start of kernel VA (0xC0000000 for 3G/1G split)
- `PHYS_OFFSET`: Start of physical RAM

**Concrete example** (1GB RAM, PAGE_OFFSET=0xC0000000, PHYS_OFFSET=0x00000000):
```
vmalloc_limit = 0xFF800000 - 240MB - 8MB - 0xC0000000 + 0x00000000
             = 0xFF800000 - 0x0F800000 - 0xC0000000
             = 0x40000000   ← 1GB
```
So the lowmem limit is 1GB. With 1GB of RAM and 1GB lowmem limit, ALL RAM is lowmem — no highmem needed (typical for ≤1GB systems).

**Example with smaller vmalloc** (`vmalloc=128M`):
```
vmalloc_limit = 0xFF800000 - 128MB - 8MB - 0xC0000000
             = 0xFF800000 - 0x08800000 - 0xC0000000
             = 0x47000000   ← 1.109 GB
```
More physical memory can be mapped as lowmem.

### Step 2: Align First Memory Block to PMD_SIZE

```c
    for_each_mem_range(i, &block_start, &block_end) {
        if (!IS_ALIGNED(block_start, PMD_SIZE)) {
            phys_addr_t len;
            len = round_up(block_start, PMD_SIZE) - block_start;
            memblock_mark_nomap(block_start, len);
        }
        break;  /* Only check the first block */
    }
```

The first memory block must start at a PMD-aligned address (2MB on ARM32) because the early page table setup uses PMD-granularity mappings. If the first block starts at a non-PMD-aligned address, the non-aligned prefix is marked `MEMBLOCK_NOMAP` — it won't be mapped or used.

### Step 3: Find Highest Lowmem Physical Address

```c
    for_each_mem_range(i, &block_start, &block_end) {
        if (block_start < vmalloc_limit) {
            if (block_end > lowmem_limit)
                lowmem_limit = min_t(u64, vmalloc_limit, block_end);

            if (!memblock_limit) {
                if (!IS_ALIGNED(block_start, PMD_SIZE))
                    memblock_limit = block_start;
                else if (!IS_ALIGNED(block_end, PMD_SIZE))
                    memblock_limit = lowmem_limit;
            }
        }
    }
```

`lowmem_limit` = the highest physical byte that is both:
1. In a memblock memory region
2. Below `vmalloc_limit`

This is the **boundary of the direct kernel mapping**.

### Step 4: Set Global Variables

```c
    arm_lowmem_limit = lowmem_limit;
    high_memory = __va(arm_lowmem_limit - 1) + 1;
```

- `arm_lowmem_limit`: Physical boundary. Pages below this are "lowmem" — directly mapped.
- `high_memory`: Virtual address of the first byte above the direct mapping. Stored as a global pointer used by many kernel functions.

### Step 5: Handle No-HIGHMEM or VIPT Aliasing

```c
    if (!IS_ENABLED(CONFIG_HIGHMEM) || cache_is_vipt_aliasing()) {
        if (memblock_end_of_DRAM() > arm_lowmem_limit) {
            pr_notice("Ignoring RAM at %pa-%pa\n",
                      &memblock_limit, &end);
            memblock_remove(memblock_limit, end - memblock_limit);
        }
    }
```

If highmem is disabled or the cache has VIPT aliasing (which makes highmem unsafe), any RAM above `arm_lowmem_limit` is **permanently removed from memblock**. The kernel effectively ignores it.

### Step 6: Set memblock Current Limit

```c
    memblock_set_current_limit(memblock_limit);
```

This is critical: it tells the memblock allocator the highest physical address from which it may allocate memory. Allocations during boot must stay within the current limit because the page tables (set up by `paging_init()` later) only map up to this limit.

---

## 4. Data Flow Summary

```
Input:
  vmalloc_size (from parse_early_param: vmalloc= or default 240MB)
  memblock regions (from FDT memory nodes + early_mem handler)
  PHYS_OFFSET, PAGE_OFFSET, VMALLOC_END (compile-time constants)

Computation:
  vmalloc_limit = physical address above which vmalloc region begins

Output (globals set):
  arm_lowmem_limit    → used by paging_init, zone_sizes_init
  high_memory         → used by mm/memory.c, /proc/meminfo
  memblock current limit → used by memblock allocator
```

---

## 5. Call Tree (Bottom-Up)

```
memblock_mark_nomap()           ← mm/memblock.c
memblock_remove()               ← mm/memblock.c
memblock_set_current_limit()    ← mm/memblock.c
        ▲
adjust_lowmem_bounds()          ← arch/arm/mm/mmu.c:1196
        ▲ (first call)
setup_arch()                    ← arch/arm/kernel/setup.c:1158
```

---

## 6. Why Two Calls Are Needed (Preview)

```
adjust_lowmem_bounds()          ← FIRST CALL (this document)
        │ Initial boundary based on memblock as populated by FDT
        ▼
arm_memblock_init(mdesc)
        │ mdesc->reserve() may remove memory regions
        │ e.g., board reserves 16MB for camera firmware
        │ → memblock now has fewer available regions
        ▼
adjust_lowmem_bounds()          ← SECOND CALL (see dir 15)
        │ Recalculate with updated memblock
        │ lowmem_limit may now be lower
```

The first call establishes boundaries used by `arm_memblock_init()` for its `dma_contiguous_reserve(arm_dma_limit)` call (which needs to know where memory ends). The second call ensures the final boundary is accurate after all reservations.

---

## 7. Interview Q&A

**Q1: What is arm_lowmem_limit and how is it used by paging_init()?**
> `arm_lowmem_limit` is the highest physical byte address that the kernel directly maps into its virtual address space. `paging_init()` calls `map_lowmem()` which maps all memblock regions from `PHYS_OFFSET` to `arm_lowmem_limit` into kernel virtual space. Physical addresses above `arm_lowmem_limit` are "highmem" — not permanently mapped, accessed only via kmap/kunmap.

**Q2: What is high_memory and where is it used?**
> `high_memory` is the virtual address of the first byte beyond the direct physical mapping — i.e., `__va(arm_lowmem_limit)`. It's used in `/proc/meminfo` to report "LowTotal" vs "HighTotal", in `kmap_prot` to validate highmem page mapping, and in VMA checks to determine if a virtual address is in the direct-mapped range or in highmem.

**Q3: Why is vmalloc_limit computed in physical address space?**
> `adjust_lowmem_bounds()` needs to compare against physical memory blocks returned by `for_each_mem_range()`. Physical addresses must be compared with physical addresses. `vmalloc_limit` represents: "what is the physical address beyond which we cannot use lowmem?" This is derived by working backward from the virtual vmalloc start address.

**Q4: What happens on a system with 512MB RAM and default vmalloc=240M?**
> `vmalloc_limit` = 0xFF800000 - 240MB - 8MB - 0xC0000000 = 0x40000000 (1GB). Since all 512MB of RAM (0x00000000 – 0x1FFFFFFF) is below 1GB, `arm_lowmem_limit = 0x1FFFFFFF`. All 512MB is lowmem. No highmem. The vmalloc window starts at `arm_lowmem_limit + VMALLOC_OFFSET` in virtual space, with 240MB available for vmalloc/ioremap.

**Q5: What is VMALLOC_OFFSET and why does it exist?**
> `VMALLOC_OFFSET` is an 8MB guard gap between the end of the direct-mapped lowmem region and the start of vmalloc. Without it, a kernel bug (off-by-one in address calculations, integer overflow) might compute a vmalloc address that overlaps with the last lowmem page. The 8MB gap makes such overflow bugs obvious — they cause an immediate fault at the guard gap rather than silently corrupting lowmem data.
