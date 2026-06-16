# adjust_lowmem_bounds() First Call — System Design Approach and Q&A

## 1. Why This Function Exists: The VA/PA Impedance Mismatch Problem

On a 32-bit system, there is a fundamental tension:
- **Hardware**: Physical RAM can be up to 4GB (or even more with LPAE)
- **Software**: Kernel needs only 1GB of virtual address space (with 3G/1G split)
- **Constraint**: The kernel must maintain enough virtual space for vmalloc (ioremap, kernel modules, dynamic mappings) — typically 240MB
- **Result**: At most ~768MB of RAM can be "lowmem" with default settings. The rest is "highmem".

`adjust_lowmem_bounds()` is the function that **quantifies** this tradeoff and establishes the lowmem/highmem boundary.

---

## 2. Design Principle: Establish Bounds Before Allocating

The kernel's early boot has a strict ordering requirement:

```
WRONG ORDER (would cause bugs):
  arm_memblock_init()     ← allocates memory (page tables, initrd)
  adjust_lowmem_bounds()  ← sets arm_lowmem_limit
  Problem: arm_memblock_init() called dma_contiguous_reserve(arm_dma_limit)
           which needs arm_lowmem_limit to be valid!

CORRECT ORDER:
  adjust_lowmem_bounds()  ← sets arm_lowmem_limit, memblock_limit
  arm_memblock_init()     ← can now safely allocate within known bounds
```

The comment in `setup_arch()` explicitly says:
> "Make sure the calculation for lowmem/highmem is set appropriately before reserving/allocating any memory"

This is a **precondition pattern** — establish global state before any code depends on it.

---

## 3. Why Two Calls? The Reserve-Recalculate Pattern

```
Call 1: adjust_lowmem_bounds()
        │
        │ Based on full FDT memory → conservative (larger lowmem limit)
        │
        ▼
arm_memblock_init(mdesc)
        │
        ├── mdesc->reserve()      ← board-specific reservations
        │       e.g., 128MB reserved for GPU framebuffer
        │       → that 128MB is now REMOVED from memblock
        │
        ├── dma_contiguous_reserve()  ← carves out CMA region
        │
        └── arm_memblock_steal_permitted = false
        
Call 2: adjust_lowmem_bounds()
        │
        │ Based on updated memblock (minus all reservations)
        │ May produce lower arm_lowmem_limit if reservations
        │ fragmented the top of memory
```

**Why not just make one call after arm_memblock_init()?** Because `arm_memblock_init()` itself uses `arm_lowmem_limit` (indirectly via `dma_contiguous_reserve(arm_dma_limit)`). The first call is needed to bootstrap the system. The second call corrects any inaccuracy introduced by reservations.

---

## 4. Dependency Graph

```
[FDT memory nodes]
[early_mem handler]          → memblock populated
         │
         ▼
[efi_init/arm_efi_init]      → EFI regions added to memblock
         │
         ▼
[adjust_lowmem_bounds()] ←── vmalloc_size (from parse_early_param)
         │                    VMALLOC_END, PAGE_OFFSET, PHYS_OFFSET (constants)
         │
         ├──→ arm_lowmem_limit     → used by dma_contiguous_reserve()
         │                           used by map_lowmem() in paging_init()
         │                           used by zone_sizes_init()
         ├──→ high_memory           → used by /proc/meminfo
         │                           used by mm/memory.c
         └──→ memblock current limit → used by memblock_alloc() (must stay below)
```

---

## 5. The vmalloc= Parameter: Tuning the Tradeoff

The `vmalloc=` kernel command line parameter directly affects `adjust_lowmem_bounds()`:

```
vmalloc=240M  → vmalloc_size = 240MB (default)
               vmalloc_limit = 0x40000000 (1GB lowmem with default PAGE_OFFSET)

vmalloc=128M  → vmalloc_size = 128MB
               vmalloc_limit = 0x47000000 (~1.1GB lowmem)
               Benefit: more lowmem for large-RAM systems
               Cost: less vmalloc space → fewer ioremap mappings

vmalloc=512M  → vmalloc_size = 512MB
               vmalloc_limit = 0x28000000 (~640MB lowmem)
               Benefit: large vmalloc for systems with big MMIO bars
               Cost: less lowmem → more reliance on highmem/kmap
```

**Real-world tuning case**: A GPU with 256MB PCI BAR requires 256MB of vmalloc contiguous virtual address space for ioremap. If vmalloc is only 240MB, the ioremap might fail. Increasing `vmalloc=512M` or more resolves this, at the cost of lowmem.

---

## 6. Security Consideration: ASLR and Memory Layout

On ARM32, `adjust_lowmem_bounds()` sets fixed boundaries that affect kernel ASLR:
- `arm_lowmem_limit` is a predictable address (attacker can compute it from vmalloc_size)
- This is a known limitation of ARM32 ASLR

On ARM64, the direct linear map is randomized (`CONFIG_RANDOMIZE_BASE`):
- `memstart_addr` is randomized, shifting the entire direct map
- No `adjust_lowmem_bounds()` needed; the direct map doesn't conflict with vmalloc

---

## 7. System Design Q&A

**Q: Why can't ARM32 just use a 2G/2G split to get more kernel VA?**
> Linux on ARM32 supports 2G/2G split (`CONFIG_VMSPLIT_2G`), which sets `PAGE_OFFSET = 0x80000000`. This gives the kernel 2GB of VA instead of 1GB. `adjust_lowmem_bounds()` recalculates: `vmalloc_limit = 0xFF800000 - 240MB - 8MB - 0x80000000 = 0x7F000000` = ~2GB lowmem limit. More lowmem is available. The tradeoff is that user processes get only 2GB of VA instead of 3GB, which breaks applications that need large VAs (e.g., 32-bit databases, JVM with large heap). Most distributions use 3G/1G split.

**Q: What happens when arm_lowmem_limit is set incorrectly too high?**
> If `arm_lowmem_limit` is set higher than the vmalloc window allows, `map_lowmem()` in `paging_init()` would try to map virtual addresses that overlap with the vmalloc region. This creates a virtual address collision: when a driver calls `ioremap()`, it gets a VA that might already have a PTE pointing to RAM. Accessing that VA reads RAM instead of the device register. This produces silent memory corruption — extremely hard to debug. The careful math in `adjust_lowmem_bounds()` prevents this.

**Q: What is MEMBLOCK_NOMAP and why is it used for non-aligned first block?**
> `MEMBLOCK_NOMAP` is a flag on a memblock region that says "this physical memory exists but should not be mapped by the kernel." The non-aligned prefix of the first memory block is marked NOMAP because ARM32's `prepare_page_table()` clears page table entries in PMD (2MB) granularity. If the first block starts at 0x01000000 (non-PMD-aligned), `prepare_page_table()` would clear the PMD covering 0x00000000–0x001FFFFF even though part of that range is RAM. Marking it NOMAP tells later code: this range exists but the page table code won't create PTEs for it.

**Q: How does memblock_set_current_limit() protect against early boot allocation bugs?**
> `memblock_set_current_limit(memblock_limit)` sets a soft upper bound on the physical address from which memblock can allocate. If kernel code tries to allocate memory above this limit (e.g., in `arm_memblock_init()`), memblock will look for memory below the limit first. This prevents allocating physical memory above `arm_lowmem_limit` for early boot data structures — because `paging_init()` won't map above `arm_lowmem_limit`, any pointer to memory above it would be a virtual address that's not mapped (a guaranteed fault when dereferenced).

**Q: If CONFIG_HIGHMEM is disabled, what happens to RAM above arm_lowmem_limit?**
> If `CONFIG_HIGHMEM=n` (common for systems with ≤1GB RAM), RAM above `arm_lowmem_limit` cannot be used as highmem (no ZONE_HIGHMEM). `adjust_lowmem_bounds()` calls `memblock_remove()` to permanently delete those physical regions from memblock. The kernel never sees this RAM again. It's wasted. This is why embedded systems with tight memory carefully tune `PAGE_OFFSET` and `vmalloc_size` to maximize lowmem while keeping CONFIG_HIGHMEM disabled (simpler, faster, no kmap overhead).
