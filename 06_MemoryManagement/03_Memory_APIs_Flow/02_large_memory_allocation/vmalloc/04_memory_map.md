# vmalloc — Memory Map (ARM64)

> Linux 6.6 · ARM64 · 48-bit VA · 4 KB pages.
> `vmalloc` returns a VA in the **VMALLOC area**, **not** the linear map.
> Backing physical pages are individual non-contiguous pages from
> `ZONE_NORMAL` (or wherever the GFP mask permits).

---

## 1. Headline

> The pointer returned by `vmalloc()` lives in
> `VMALLOC_START..VMALLOC_END`. Physical pages backing it are scattered
> across RAM (one struct page per 4 KB slot). `virt_to_phys()` on that
> pointer is **invalid** — use `vmalloc_to_page(ptr)` or `vmalloc_to_pfn`.

---

## 2. Where in the ARM64 kernel VA space

```
Kernel TTBR1 half
+--------------------------------------------------+ 0xffff_ffff_ffff_ffff
| FIXMAP, PCI I/O                                  |
+--------------------------------------------------+
| VMEMMAP    (struct page array)                   |
+--------------------------------------------------+ VMEMMAP_START
| KASAN SHADOW (1/8 of kernel half if enabled)     |
+--------------------------------------------------+
| MODULES    (128 MB, MODULES_VADDR..MODULES_END)  |
+--------------------------------------------------+
| BPF JIT    (128 MB)                              |
+--------------------------------------------------+
| =======   VMALLOC AREA   =======                 |
|                                                  |
|       VMALLOC_START .. VMALLOC_END               |
|                                                  |
|       hundreds of TB on 48-bit VA                |
|                                                  |
|   <-- vmalloc(), vzalloc(), vmap(), ioremap()    |
|       module data lives here too                 |
|                                                  |
+--------------------------------------------------+ VMALLOC_START = PAGE_OFFSET + PUD_SIZE
| LINEAR (DIRECT) MAP                              |
|   PAGE_OFFSET = 0xffff_8000_0000_0000            |
|   (kmalloc, kmem_cache_alloc, __get_free_pages)  |
+--------------------------------------------------+ PAGE_OFFSET
```

### Constants (48-bit / 4K defconfig)

| Symbol           | Value (approx.)                |
|------------------|--------------------------------|
| `PAGE_OFFSET`    | `0xffff_8000_0000_0000`        |
| `VMALLOC_START`  | `PAGE_OFFSET + PUD_SIZE` (1 GB) = `0xffff_8000_4000_0000` |
| `VMALLOC_END`    | `VMEMMAP_START - SZ_256M`      |
| `VMEMMAP_START`  | `-VMEMMAP_SIZE - SZ_2M`        |
| `MODULES_VADDR`  | `BPF_JIT_REGION_END`           |
| `MODULES_END`    | `MODULES_VADDR + SZ_128M`      |

`/sys/kernel/debug/kernel_page_tables` (with `CONFIG_PTDUMP_DEBUGFS`) shows the exact boundaries on your running system.

---

## 3. Zoom in: what's actually mapped

```
              VMALLOC area (huge, sparse)
    VMALLOC_START                                        VMALLOC_END
        |                                                    |
  +-----+----+         +----+----+----+        +----+--------+
  | area#1   |.........| area#2  |...| ........| ar#N|       |
  | [G] data [G]       | [G] data[G] |          [G] data [G] |
  +----------+         +-------------+         +--------------+
     ^                    ^                       ^
     |                    |                       |
  vmalloc(64KB)        vmalloc(2MB)            ioremap(MMIO)

  [G] = guard page (unmapped, PROT_NONE)
```

Each `vmalloc()` reservation is **bracketed by two guard pages** (`VM_NO_GUARD` can opt out) so that overflows trap immediately instead of corrupting an adjacent vmalloc area.

The pages **inside** each area are PTEs pointing to scattered physical pages — visualized:

```
  area = vmalloc(16 KB)  -> 4 pages = 4 PTEs

   VA:   |  pg0  |  pg1  |  pg2  |  pg3  |
          PTE0    PTE1    PTE2    PTE3
           |       |       |       |
           v       v       v       v
   PA:   pfn_47  pfn_91  pfn_3   pfn_220   <-- random pfns from buddy
```

---

## 4. Per-page lookup

To get the physical page for a vmalloc VA:

```c
struct page *p = vmalloc_to_page(addr);   /* mm/vmalloc.c */
phys_addr_t pa = page_to_phys(p);
```

`vmalloc_to_page` walks `init_mm`'s page tables. Cost: ~4 memory loads (PGD→PUD→PMD→PTE). Use sparingly.

For scatter-gather DMA over a vmalloc buffer, the canonical pattern is to build an `sg_table` by iterating with `vmalloc_to_page` per page — see [`drivers/dma-buf/heaps/system_heap.c`](https://elixir.bootlin.com/linux/v6.6/source/drivers/dma-buf/heaps/system_heap.c) for a reference.

---

## 5. Page attributes (PTE bits)

Same as in [03_arm64_callflow.md](03_arm64_callflow.md) §3:

| Bit          | Value         | Effect                       |
|--------------|---------------|------------------------------|
| `ATTRINDX`   | `MT_NORMAL`   | cacheable, WB-WA             |
| `AP[2:1]`    | EL1 RW, EL0 no | kernel-only                  |
| `UXN`/`PXN`  | both 1        | non-executable               |
| `nG`         | 0             | global — TLB cached across ASID |

Variants:

- `vmalloc_user` adds `PAGE_READONLY` access for EL0 if subsequently `remap_vmalloc_range`'d into userspace.
- `__vmalloc_node_range(..., prot=PAGE_KERNEL_EXEC)` clears `UXN`/`PXN` so the area can hold executable code (used by older module loader; BPF uses its own).

---

## 6. Backing physical zone

```
  high PA
  +---------------------------+
  |  ZONE_NORMAL              | <-- vmalloc(GFP_KERNEL) lands here
  +---------------------------+ 4 GB
  |  ZONE_DMA32               | <-- vmalloc_32() / vmalloc(GFP_DMA32)
  +---------------------------+
  |  ZONE_DMA  (rare on arm64)| <-- vmalloc(GFP_DMA)
  +---------------------------+
```

Because `vmalloc` allocates pages one at a time via the bulk page allocator, it never *needs* a high-order block — even under fragmentation it can succeed as long as order-0 pages exist. This is the central advantage over `kmalloc` for large buffers.

---

## 7. Lifecycle on the address space

```
   vmalloc(N) ----> VA range reserved + N pages mapped + PTEs visible
        ^
        |
        | (use buffer)
        v
   vfree(p)  ----> rbtree entry moved to lazy-purge list
                   PTEs still present (deferred TLB invalidation)
        ^
        |
        | (purge threshold hit)
        v
   __purge_vmap_area_lazy()  ----> PTEs cleared, dsb ishst,
                                    flush_tlb_kernel_range() broadcast TLBI,
                                    pages returned to buddy.
```

This means after `vfree(p)`, the VA `p` may still appear in TLBs for some time. Dereferencing it is UB but may not fault immediately.

---

## 8. Verify on a live system

```text
# All current vmalloc areas with callers and sizes
$ cat /proc/vmallocinfo | head
0xffff800040000000-0xffff800040041000   266240 vmalloc+0x84/0xa8 pages=64 vmalloc N0=64

# Pgtable dump (requires CONFIG_PTDUMP_DEBUGFS):
$ cat /sys/kernel/debug/kernel_page_tables | grep -A2 'vmalloc'

# Vmalloc limit:
$ grep VmallocChunk /proc/meminfo
VmallocChunk:   34359738368 kB    # ~34 TB free contiguous
```

---

## 9. Cross-references

- Internals → [02_internals.md](02_internals.md)
- ARM64 call flow → [03_arm64_callflow.md](03_arm64_callflow.md)
- Compare to kmalloc memory map → [`../../01_basic_allocation/kmalloc/04_memory_map.md`](../../01_basic_allocation/kmalloc/04_memory_map.md)
- Free side → [`../vfree/04_memory_map.md`](../vfree/04_memory_map.md)
