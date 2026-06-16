# kmalloc — Memory Map (ARM64, 48‑bit VA, 4 KB pages)

> Linux 6.6 LTS, `CONFIG_ARM64_VA_BITS_48=y`, `CONFIG_ARM64_4K_PAGES=y`,
> `CONFIG_KASAN_GENERIC=y` (typical defconfig).
> All constants verified against
> [`arch/arm64/include/asm/memory.h`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/include/asm/memory.h)
> and [`Documentation/arch/arm64/memory.rst`](https://elixir.bootlin.com/linux/v6.6/source/Documentation/arch/arm64/memory.rst).

---

## 1. Headline answer

> **A pointer returned by `kmalloc()` lives in the kernel _linear (direct) map_, backed by a physical page from `ZONE_NORMAL` (or `ZONE_DMA32` / `ZONE_DMA` if a DMA flag was used).**
> Its virtual address satisfies `phys_to_virt(p) == p` translation, i.e.
> `VA = PAGE_OFFSET + (PA - PHYS_OFFSET)`.

---

## 2. ARM64 virtual address space (48‑bit VA, 4 KB pages)

```
+--------------------------------------------------------------------------+ 0xffff_ffff_ffff_ffff
|  FIXMAP           (a few MB, early ioremap, kmap_atomic)                 |
+--------------------------------------------------------------------------+ FIXADDR_TOP
|  PCI I/O          (16 MB, PCI_IO_START..PCI_IO_END)                      |
+--------------------------------------------------------------------------+
|  VMEMMAP          (struct page array, sparsemem-vmemmap)                 |
|                   VMEMMAP_START .. VMEMMAP_START + VMEMMAP_SIZE          |
+--------------------------------------------------------------------------+ VMEMMAP_START
|                                                                          |
|  KASAN SHADOW     (1/8 of linear map; only if CONFIG_KASAN)              |
|                   KASAN_SHADOW_START .. KASAN_SHADOW_END                 |
|                   KASAN_SHADOW_OFFSET = 0xdfff_8000_0000_0000            |
|                                                                          |
+--------------------------------------------------------------------------+
|  MODULES          (128 MB, MODULES_VADDR .. MODULES_END)                 |
+--------------------------------------------------------------------------+ MODULES_END
|  BPF JIT          (128 MB, BPF_JIT_REGION_START/END)                     |
+--------------------------------------------------------------------------+
|                                                                          |
|  VMALLOC          (huge: VMALLOC_START .. VMALLOC_END,                   |
|                    ~ several hundred TB)                                 |
|                    <-- vmalloc(), ioremap(), module data live here       |
|                                                                          |
+--------------------------------------------------------------------------+ VMALLOC_START
|                                                                          |
|                                                                          |
|  ====  LINEAR (DIRECT) MAP  ====                                         |
|  PAGE_OFFSET = 0xffff_8000_0000_0000  (CONFIG_ARM64_VA_BITS=48)          |
|                                                                          |
|        1:1 (modulo PHYS_OFFSET) mapping of ALL RAM,                      |
|        cacheable, global, never unmapped.                                |
|                                                                          |
|        <-- kmalloc(), kzalloc(), kmem_cache_alloc(),                     |
|            __get_free_pages(), alloc_pages() (via page_address())        |
|                                                                          |
+==========================================================================+ PAGE_OFFSET = 0xffff_8000_0000_0000
                                ▲
                                │     (kernel half / TTBR1_EL1)
================================│============================================ 0xffff_0000_0000_0000
                                │     (gap — non-canonical addresses)
================================│============================================ 0x0001_0000_0000_0000
                                │     (user half / TTBR0_EL1)
+--------------------------------------------------------------------------+ TASK_SIZE = 0x0001_0000_0000_0000 (256 TB)
|                                                                          |
|  USER SPACE       (per-process)                                          |
|                    mmap area, heap, stack, text, data                    |
|                                                                          |
+--------------------------------------------------------------------------+ 0x0000_0000_0000_0000
```

### Numeric constants on a 48‑bit / 4 KB / defconfig kernel

| Symbol               | Value                          | Source                                              |
|----------------------|--------------------------------|-----------------------------------------------------|
| `VA_BITS`            | 48                             | `arch/arm64/Kconfig` (`ARM64_VA_BITS_48`)           |
| `PAGE_SHIFT`         | 12                             | `arch/arm64/include/asm/page-def.h`                 |
| `PAGE_OFFSET`        | `0xffff_8000_0000_0000`        | `asm/memory.h` — `_PAGE_OFFSET(VA_BITS) = (-(UL(1) << (VA_BITS - 1)))` |
| `PHYS_OFFSET`        | platform RAM base (e.g. `0x4000_0000` on QEMU virt) | `memstart_addr` runtime |
| `VMALLOC_START`      | `PAGE_OFFSET + PUD_SIZE`       | `asm/pgtable.h` (post-randomization base)           |
| `VMALLOC_END`        | `(VMEMMAP_START - SZ_256M)`    | `asm/pgtable.h`                                     |
| `MODULES_VADDR`      | `BPF_JIT_REGION_END`           | `asm/memory.h`                                      |
| `MODULES_END`        | `MODULES_VADDR + SZ_128M`      | `asm/memory.h`                                      |
| `VMEMMAP_START`      | `(-VMEMMAP_SIZE - SZ_2M)`      | `asm/memory.h`                                      |
| `KASAN_SHADOW_OFFSET`| `0xdfff_8000_0000_0000`        | `arch/arm64/Kconfig` (`KASAN_SHADOW_OFFSET`)        |
| `TASK_SIZE` (EL0)    | `0x0001_0000_0000_0000` = 256 TB | `asm/processor.h`                                 |

---

## 3. Where the `kmalloc` pointer lives — annotated zoom-in

```
                    Kernel virtual address space (TTBR1)
   high addrs
   ─────────────────────────────────────────────────────────
                          ┌─────────────────────┐
       VMALLOC area  ───▶ │  vmalloc / vfree    │   (NOT used by kmalloc)
                          │  ioremap            │
                          └─────────────────────┘
                          ╔═════════════════════╗  ◀── KMALLOC RETURNS HERE
                          ║                     ║
                          ║   LINEAR MAP        ║   PAGE_OFFSET = 0xffff_8000_…
                          ║                     ║
                          ║   ┌─────────────┐   ║
                          ║   │ slab page   │   ║   one struct slab = one page (or 2^order pages)
                          ║   │ [obj0][obj1]│   ║
                          ║   │ [obj2][obj3]│◀──╫─── kmalloc returned &obj2
                          ║   │  …          │   ║
                          ║   └─────────────┘   ║
                          ║                     ║
                          ╚═════════════════════╝
   low addrs
   ─────────────────────────────────────────────────────────
                          PAGE_OFFSET (start of linear map)
```

Key consequences:

- `virt_to_phys(p)`  works in O(1):  `(p - PAGE_OFFSET) + PHYS_OFFSET`
  → drivers can build scatter-gather lists, SMMU mappings, etc.
- `virt_to_page(p)`  → `pfn_to_page((virt_to_phys(p)) >> PAGE_SHIFT)`
  via the vmemmap.
- `kmalloc()` memory **is always present** in the page tables — no fault
  on first touch (unlike `vmalloc`).
- It is **cacheable, write-back, normal memory** (per the linear map's
  attrs in `mair_el1` slot 0xff / `MT_NORMAL`).

---

## 4. Physical side — which zone?

```
                         Physical RAM
   high PA ──────────────────────────────────────────────
                 ┌──────────────────────────────────┐
                 │       ZONE_MOVABLE   (opt)       │
                 ├──────────────────────────────────┤
                 │       ZONE_NORMAL                │  ◀── default kmalloc(GFP_KERNEL)
                 │       (4 GB  ..  end-of-RAM)     │
                 ├──────────────────────────────────┤ 4 GB boundary (0x1_0000_0000)
                 │       ZONE_DMA32                 │  ◀── kmalloc(GFP_DMA32)
                 │       (1 MB  ..  4 GB)           │
                 ├──────────────────────────────────┤ 1 MB boundary (platform dependent)
                 │       ZONE_DMA                   │  ◀── kmalloc(GFP_DMA)   (rare on arm64)
                 │       (0     ..  1 MB or so)     │
   low PA ───────└──────────────────────────────────┘
```

> **arm64 specifics:** `ZONE_DMA` only exists when an in-kernel device has
> `dma-ranges` constrained below 4 GB (Raspberry Pi 4 historically — see
> [`arch/arm64/mm/init.c:zone_sizes_init`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/mm/init.c#L196)).
> On most server/mobile arm64 SoCs there is no `ZONE_DMA`, and `ZONE_DMA32`
> may also be empty if all RAM is above 4 GB.

The slab page returned by buddy is then **already linearly mapped** at
`PAGE_OFFSET + (PA - PHYS_OFFSET)`. No additional `ioremap` / `vmap` work.

---

## 5. Size bucket → slab page → returned address

Example: `p = kmalloc(56, GFP_KERNEL);`

```
1. kmalloc_index(56)  →  bucket 6  →  "kmalloc-64"           (next ≥ size)
2. s = kmalloc_caches[KMALLOC_NORMAL][6]
3. s->oo = (order=0, objects=64)                              ⇒ 1 page, 64 × 64 B objects
4. SLUB pulls a free 64 B slot from c->freelist:
                ┌────────── one 4 KB page in linear map ──────────┐
                │ 64B │ 64B │ 64B │ p->│ 64B │ ... │ 64B │ ... │ 64B │
                └─────┴─────┴─────┴────┴─────┴─────┴─────┴─────┴─────┘
                                ▲
                                └── p (your pointer; cache-line aligned)
5. virt_to_phys(p) = (p - PAGE_OFFSET) + PHYS_OFFSET
6. The page's struct page lives in vmemmap at
   VMEMMAP_START + (pfn * sizeof(struct page))
```

---

## 6. Page-attribute summary (what the MMU sees)

| Attribute (from `pgprot_t` of linear map)  | Value                                    |
|--------------------------------------------|------------------------------------------|
| `MAIR` index                               | `MT_NORMAL` (Normal, Inner+Outer WB-WA)  |
| Shareability                               | Inner shareable                          |
| Access permissions                         | `AP[2:1] = 00` → EL1 RW, EL0 no-access   |
| `UXN` (unprivileged execute never)         | 1                                        |
| `PXN` (privileged execute never)           | 1 (data; `rodata_full` keeps this)       |
| `nG` (not global)                          | 0 (global — TLB entries kept across ASID)|
| Contiguous hint                            | may be set when 16 consecutive entries point to consecutive PFNs |

See [`arch/arm64/mm/mmu.c:__create_pgd_mapping`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/mm/mmu.c#L351) and [`asm/pgtable-prot.h`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/include/asm/pgtable-prot.h).

---

## 7. Side-by-side: kmalloc vs vmalloc vs ioremap on the same map

```
   TTBR1 kernel virtual map  (top)
   ┌─────────────────────────────────────────┐ 0xffff_ffff_…
   │ FIXMAP / PCI I/O / vmemmap              │
   ├─────────────────────────────────────────┤
   │ MODULES / BPF JIT                       │
   ├─────────────────────────────────────────┤
   │ VMALLOC area                            │
   │   ├── vmalloc()    ── virt-contig, phys-NON-contig, demand mapped │
   │   ├── ioremap()    ── device MMIO, MT_DEVICE_nGnRE                │
   │   └── module text  ── code, RX                                    │
   ├─────────────────────────────────────────┤
   │ LINEAR MAP                              │
   │   ├── kmalloc()        ── phys-contig, cacheable, always mapped   │
   │   ├── kzalloc()        ── same as kmalloc, zeroed                 │
   │   ├── kmem_cache_alloc ── same region                             │
   │   └── __get_free_pages ── same region                             │
   └─────────────────────────────────────────┘ PAGE_OFFSET
```

---

## 8. Verify on a live system

```text
$ cat /proc/iomem            # physical RAM ranges
$ cat /proc/vmallocinfo      # vmalloc users  (kmalloc NOT listed here)
$ cat /proc/slabinfo         # SLUB caches    (kmalloc-N visible)
$ cat /sys/kernel/debug/kernel_page_tables   # CONFIG_PTDUMP_DEBUGFS — full pgtable dump
```

The `kernel_page_tables` dump shows a single huge `Linear mapping` entry
(usually carved into 2 MB blocks or 1 GB blocks where alignment allows)
covering all RAM. That is the region your kmalloc pointer falls in.

---

## 9. Cross-references

- Internals (SLUB walk) → [02_internals.md](02_internals.md)
- ARM64 call flow → [03_arm64_callflow.md](03_arm64_callflow.md)
- Compare to vmalloc's memory map → [`../../02_large_memory_allocation/vmalloc/04_memory_map.md`](../../02_large_memory_allocation/vmalloc/04_memory_map.md)
- Compare to alloc_pages → [`../../04_page_level_allocation/alloc_pages/04_memory_map.md`](../../04_page_level_allocation/alloc_pages/04_memory_map.md)
