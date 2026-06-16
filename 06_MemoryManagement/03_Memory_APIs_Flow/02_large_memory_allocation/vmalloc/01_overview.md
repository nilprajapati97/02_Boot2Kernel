# vmalloc — Overview

> Linux **6.6 LTS** · ARM64 · vmalloc subsystem
> Header: `<linux/vmalloc.h>` · Source: [`mm/vmalloc.c`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c)

---

## 1. One-line definition

`vmalloc(size)` returns a kernel virtual address whose pages are **virtually contiguous** but **physically scattered** (one struct page at a time), mapped on demand into the **vmalloc area** of the kernel address space.

---

## 2. Prototype family

```c
/* include/linux/vmalloc.h */
void *vmalloc(unsigned long size);
void *vzalloc(unsigned long size);                  /* + __GFP_ZERO */
void *vmalloc_node(unsigned long size, int node);
void *vzalloc_node(unsigned long size, int node);
void *vmalloc_user(unsigned long size);             /* zeroed, mmap-ready */
void *vmalloc_32(unsigned long size);               /* phys pages <= 4 GB */
void *vmalloc_huge(unsigned long size, gfp_t gfp);  /* try PMD-block mappings */
void *__vmalloc(unsigned long size, gfp_t gfp_mask);
void *__vmalloc_node_range(unsigned long size, unsigned long align,
                           unsigned long start, unsigned long end,
                           gfp_t gfp_mask, pgprot_t prot,
                           unsigned long vm_flags, int node, const void *caller);
void  vfree(const void *addr);
```

All of the convenience wrappers funnel into `__vmalloc_node_range()` ([`mm/vmalloc.c:3322`](https://elixir.bootlin.com/linux/v6.6/source/mm/vmalloc.c#L3322)).

---

## 3. What you get / what you don't

| Property                                | `vmalloc`                | `kmalloc`        |
|-----------------------------------------|--------------------------|------------------|
| Virtually contiguous                    | yes                      | yes              |
| Physically contiguous                   | **no**                   | yes              |
| Pointer valid for `virt_to_phys`/`page` | **no** (use `vmalloc_to_page`) | yes        |
| Demand-mapped pages                     | **yes** — PTEs populated at alloc time, but pages can be huge so MMU walks may differ | no — linear map pre-existing |
| Max useful size                         | hundreds of GB (vmalloc area)| ~4 MB        |
| Sleeps?                                 | yes (uses `GFP_KERNEL` by default) | depends on flags |
| Atomic context?                         | **no** — always sleepable| sometimes        |
| TLB cost on alloc                       | yes — mappings created   | none (linear map) |

---

## 4. When to choose `vmalloc`

| Need                                                       | Choose                |
|------------------------------------------------------------|-----------------------|
| Multi-MB to multi-GB buffer, kernel-only                   | **`vmalloc`**         |
| Anything DMA-able                                          | `dma_alloc_coherent` (or `dma_map_sg` over `vmalloc` pages with care) |
| Small (≤ 4 MB), needs phys-contig                          | `kmalloc`             |
| Large + may fall back to vmalloc if kmalloc fails          | `kvmalloc(size, gfp)` |
| Module code/data (auto-allocated by module loader)         | (module loader uses `module_alloc` → vmalloc area) |
| Exec area (e.g., BPF JIT)                                  | dedicated BPF allocator — not `vmalloc` directly |

---

## 5. Default GFP

`vmalloc()` uses `GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO` internally? — actually:

```c
static inline void *vmalloc(unsigned long size)
{
    return __vmalloc_node(size, 1, GFP_KERNEL, NUMA_NO_NODE,
                          __builtin_return_address(0));
}
```

So the default is `GFP_KERNEL` (sleepable, reclaim allowed). `vzalloc` adds `__GFP_ZERO`. `vmalloc_user` adds `__GFP_ZERO` and sets up the area for user-space `mmap()`.

---

## 6. Memory region

On ARM64 (48-bit VA, 4 KB pages, defconfig):

```
VMALLOC_START = PAGE_OFFSET + PUD_SIZE       // just above linear map
VMALLOC_END   = VMEMMAP_START - SZ_256M      // just below vmemmap
```

Total size on a typical arm64 server: hundreds of TB of virtual address space — effectively unbounded. The actual cap comes from physical RAM available to back the pages.

See [04_memory_map.md](04_memory_map.md) for an ASCII layout.

---

## 7. Page attributes

By default `vmalloc` maps pages with `PAGE_KERNEL` ([`arch/arm64/include/asm/pgtable-prot.h`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/include/asm/pgtable-prot.h)) — Normal cacheable, EL1 RW, non-executable. Variants:

| Function                       | `pgprot_t`               | Use case                                       |
|--------------------------------|--------------------------|------------------------------------------------|
| `vmalloc`                      | `PAGE_KERNEL`            | RW data                                        |
| `__vmalloc_node_range(..., prot=PAGE_KERNEL_EXEC)` | exec      | module text (legacy / non-BPF)                 |
| `__vmalloc_node_range(..., prot=PAGE_KERNEL_RO)`   | read-only | secure data                                    |
| `vmap()` (sibling)             | caller-specified         | map a set of existing pages into vmalloc area  |
| `ioremap()` (sibling)          | `PROT_DEVICE_nGnRE`      | MMIO regions, **not** RAM                       |

---

## 8. Context constraints

Always **process context, sleepable**. `vmalloc` calls `alloc_pages` repeatedly (one per page), allocates `vm_area_struct` / `vmap_area` from slab, walks/populates page tables, and may invoke reclaim. Never call from IRQ, softirq, or with a spinlock held — lockdep will splat.

---

## 9. Failure modes

| Cause                                  | Symptom                                          |
|----------------------------------------|--------------------------------------------------|
| Vmalloc area exhausted (rare on 64-bit)| `vmap allocation failed: use vmalloc=<size>`     |
| Out of physical pages                  | `vmalloc: allocation failure, allocated … of …`  |
| Called from atomic context             | `BUG: sleeping function called from invalid context` |
| `size == 0` or huge alignment          | `WARN_ON` + NULL                                 |

`/proc/vmallocinfo` shows every live vmalloc range — size, caller, flags.

---

## 10. Minimal usage

```c
#include <linux/vmalloc.h>

void *buf = vmalloc(64 * 1024 * 1024);   /* 64 MB */
if (!buf)
    return -ENOMEM;
/* use buf — contiguous in kernel VA but pages are scattered */
vfree(buf);
```

---

## 11. Cross-references

- Internals → [02_internals.md](02_internals.md)
- ARM64 call flow → [03_arm64_callflow.md](03_arm64_callflow.md)
- Memory map → [04_memory_map.md](04_memory_map.md)
- Interview Q&A → [05_interview_qna.md](05_interview_qna.md)
- Sibling → [`../vfree/`](../vfree/)
