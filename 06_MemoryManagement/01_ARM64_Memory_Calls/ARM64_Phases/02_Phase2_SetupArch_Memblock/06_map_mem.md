# `map_mem()` — Linear Mapping of All Physical RAM

**Source:** `arch/arm64/mm/mmu.c` lines 1170–1230
**Phase:** Memblock Era
**Memory Allocator:** Memblock (allocates page table pages)
**Called by:** `paging_init()`
**Calls:** `__map_memblock()`, `memblock_mark_nomap()`, `memblock_clear_nomap()`

---

## What This Function Does

Creates the **linear map** (also called "direct map") — a region of kernel virtual address space where **every byte of physical RAM** has a corresponding virtual address. This is the most important mapping in the kernel.

---

## The Linear Map Formula

```
Virtual Address = Physical Address - memstart_addr + PAGE_OFFSET
Physical Address = Virtual Address - PAGE_OFFSET + memstart_addr
```

**In kernel code:**

```c
// phys_to_virt (physical → virtual via linear map)
#define __va(x) ((void *)((phys_addr_t)(x) - memstart_addr + PAGE_OFFSET))

// virt_to_phys (virtual → physical via linear map)
#define __pa(x) ((phys_addr_t)(x) - PAGE_OFFSET + memstart_addr)
```

**Example:**

```
memstart_addr = 0x4000_0000  (1 GB)
PAGE_OFFSET   = 0xFFFF_0000_0000_0000

Physical 0x4000_0000 → Virtual 0xFFFF_0000_0000_0000
Physical 0x4000_1000 → Virtual 0xFFFF_0000_0000_1000
Physical 0x8000_0000 → Virtual 0xFFFF_0000_4000_0000
```

---

## How It Works With Memory

### Memory It Creates

Page table entries in `swapper_pg_dir` for the entire linear map region.

### Memory It Allocates

| Allocated From | What | Purpose |
|---------------|------|---------|
| memblock | PUD table pages (4 KB each) | Level 1 entries |
| memblock | PMD table pages (4 KB each) | Level 2 entries |
| memblock | PTE table pages (4 KB each) | Level 3 entries (when block mappings can't be used) |

---

## Step-by-Step Execution

### Step 1: Mark Kernel Text as NOMAP

```c
static void __init map_mem(pgd_t *pgdp)
{
    phys_addr_t kernel_start = __pa_symbol(_text);
    phys_addr_t kernel_end = __pa_symbol(__init_begin);

    // Temporarily mark kernel text as NOMAP
    memblock_mark_nomap(kernel_start, kernel_end - kernel_start);
```

**Why mark kernel text as NOMAP?**

This prevents the kernel text from being mapped **twice** with different permissions:

1. The linear map would map it as `PAGE_KERNEL` (RW + executable)
2. The kernel image is already mapped as `PAGE_KERNEL_ROX` (RO + executable)

Having two mappings with different permissions is dangerous:
- An attacker could write to the RW alias to modify code visible via the RO alias
- ARM64 cache coherency requires consistent attributes for the same physical page

By marking it NOMAP, the main loop skips the kernel text region. It's re-mapped separately with proper permissions.

---

### Step 2: Map All Memory Regions

```c
    for_each_mem_range(i, &start, &end) {
        if (start >= end)
            break;

        // Adjust to direct map boundaries
        if (end > direct_map_end)
            end = direct_map_end;
        if (start < __pa(PAGE_OFFSET))
            start = __pa(PAGE_OFFSET);

        __map_memblock(pgdp, start, end,
                      pgprot_tagged(PAGE_KERNEL), flags);
    }
```

**`for_each_mem_range()`** iterates over all non-NOMAP regions in `memblock.memory`:

```
Iteration:
  Region 0: [0x4000_0000, 0x4080_0000)  ← Below kernel text (if separate bank)
  Region 1: [0x4280_0000, 0xC000_0000)  ← Above kernel text
  Region 2: [0x1_0000_0000, 0x1_8000_0000)  ← Second memory bank

  (kernel text [0x4080_0000, 0x4280_0000) is skipped — marked NOMAP)
```

---

### Step 3: `__map_memblock()` — Create Mappings

```c
static void __init __map_memblock(pgd_t *pgdp, phys_addr_t start,
                                   phys_addr_t end, pgprot_t prot,
                                   int flags)
{
    __create_pgd_mapping(pgdp,              // PGD table (swapper_pg_dir)
                         start,              // Physical start
                         __phys_to_virt(start), // Virtual start (linear map)
                         end - start,        // Size
                         prot,               // PAGE_KERNEL
                         early_pgtable_alloc, // Allocator for page table pages
                         flags);             // Block mapping flags
}
```

**`early_pgtable_alloc()`** — allocates a page table page from memblock:

```c
static phys_addr_t __init early_pgtable_alloc(int shift)
{
    phys_addr_t phys = memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
    void *ptr = __va(phys);  // Convert to virtual via linear map
    memset(ptr, 0, PAGE_SIZE);  // Zero the page table
    return phys;
}
```

**Chicken-and-egg?** You might wonder: how can `early_pgtable_alloc()` use `__va()` if the linear map doesn't exist yet?

The answer: page table pages are allocated one-by-one as needed during the mapping walk. By the time a new PTE page is needed at a given address, all **earlier** addresses are already mapped. The memblock allocator returns from high addresses (top-down), which may already be mapped from a previous iteration.

Additionally, `init_pg_dir` (from boot) may still have some mappings that cover the newly allocated page table pages.

---

### Step 4: Re-map Kernel Text (No-Execute in Linear Map)

```c
    // Clear NOMAP and remap kernel text with restricted permissions
    memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
    __map_memblock(pgdp, kernel_start, kernel_end,
                  PAGE_KERNEL, NO_CONT_MAPPINGS);
}
```

The kernel text is now mapped in the linear map as:

| Permission | Linear Map Alias | Kernel Image Mapping |
|------------|-----------------|---------------------|
| Read | Yes | Yes |
| Write | Yes | No (RO) |
| Execute | **No** (NX) | Yes (X) |

The linear map alias is **writable but non-executable**. The kernel image mapping is **read-only but executable**. This maintains W^X: no single mapping is both writable AND executable.

`NO_CONT_MAPPINGS` flag prevents using contiguous PTE entries for the kernel text region, ensuring fine-grained permission control.

---

## Block Mapping Strategy

The `flags` parameter controls whether block mappings (large pages) are used:

### With Block Mappings (Default: `NO_BLOCK_MAPPINGS` NOT set)

```
For a 4GB RAM system with 4KB page granule:

PGD → PUD (1GB blocks):
  PUD[0]: 1GB block → PA 0x0000_0000 — 0x3FFF_FFFF
  PUD[1]: 1GB block → PA 0x4000_0000 — 0x7FFF_FFFF
  PUD[2]: 1GB block → PA 0x8000_0000 — 0xBFFF_FFFF
  PUD[3]: 1GB block → PA 0xC000_0000 — 0xFFFF_FFFF

Only 1 PGD page + 1 PUD page needed = 8 KB total for 4GB!
```

### When Block Mappings Are Disabled

Reasons to disable:
- `rodata_full` — maps entire RAM with fine-grained RO/RW permissions
- DEBUG_PAGEALLOC — needs per-page permission control
- KFENCE — kernel fence for use-after-free detection

Without blocks, each 4KB page needs a PTE entry:

```
4GB / 4KB = 1,048,576 PTEs
1,048,576 PTEs / 512 per table = 2,048 PTE tables × 4KB = 8 MB of page tables
+ PMD tables + PUD tables + PGD = ~8.5 MB total
```

### Contiguous PTEs (TLB Optimization)

ARM64 supports **contiguous PTE entries** — 16 consecutive PTEs (64KB) that share a single TLB entry:

```
Without contiguous:  64KB needs 16 TLB entries
With contiguous:     64KB needs 1 TLB entry (flagged as contiguous)
```

This is used when mapping large aligned regions (normal RAM) but NOT for kernel text (which needs fine-grained permissions).

---

## Page Protection Attributes Used

### `PAGE_KERNEL` (for linear map of normal RAM)

```
AttrIndx = MT_NORMAL     (index 4: Normal, Write-Back, Cacheable)
AP       = AP_KERNEL_RW  (Read-Write, kernel only)
SH       = INNER_SHAREABLE
AF       = 1             (Access Flag)
PXN      = 1             (Privileged Execute Never — can't execute)
UXN      = 1             (User Execute Never)
```

### `pgprot_tagged(PAGE_KERNEL)` (for MTE-enabled systems)

On systems with Memory Tagging Extension (MTE):
```
AttrIndx = MT_NORMAL_TAGGED  (index 6: Normal + MTE tags)
Same permissions as PAGE_KERNEL otherwise
```

MTE adds 4-bit tags to every 16 bytes of memory, enabling hardware detection of use-after-free and buffer overflow bugs.

---

## Memory Consumed by Page Tables

| RAM Size | Block Mapping | PMD Mapping | PTE Mapping |
|----------|--------------|------------|------------|
| 1 GB | 8 KB (PGD+PUD) | 12 KB (+PMD) | ~0.5 MB (+PTEs) |
| 4 GB | 8 KB | 24 KB | ~2 MB |
| 8 GB | 8 KB | 40 KB | ~4 MB |
| 64 GB | 12 KB | 264 KB | ~32 MB |
| 1 TB | 16 KB | 4 MB | ~512 MB |

Block mappings are dramatically more memory-efficient. The kernel always prefers them when possible.

---

## Key Takeaways

1. **Linear map = simple arithmetic** — `__va()` and `__pa()` are just add/subtract, not page table walks
2. **Kernel text is NOMAP'd then remapped** — prevents writable+executable aliases (W^X violation)
3. **Block mappings save memory** — 4GB mapped with 8KB of page tables vs 2MB with PTEs
4. **Page table pages come from memblock** — `early_pgtable_alloc()` calls `memblock_phys_alloc()`
5. **Contiguous PTEs improve TLB** — 16 PTEs share one TLB entry (64KB effective page)
6. **This is the kernel's most-used mapping** — almost all kernel memory access goes through the linear map
