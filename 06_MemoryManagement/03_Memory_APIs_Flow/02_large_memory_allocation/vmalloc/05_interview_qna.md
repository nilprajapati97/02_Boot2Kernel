# vmalloc — Interview Q&A (Nvidia / Google / Qualcomm)

---

### Q1. Why is `vmalloc` virtually contiguous but physically discontiguous?  `[Nvidia]` `[Google]`

It's literally the design goal. `vmalloc` reserves a contiguous virtual range from the **vmalloc area**, then allocates physical pages **one at a time** via the bulk page allocator and stitches them into that VA range using kernel page tables. Each PTE points to an independent PFN, so the buffer is contiguous from the CPU's MMU perspective but may be scattered across DRAM. This sidesteps fragmentation: even when no high-order contiguous physical block is available, `vmalloc` can succeed as long as enough order-0 pages exist.

---

### Q2. Three things `vmalloc` does that `kmalloc` does not.  `[Qualcomm]`

1. **Reserves a VA range** from the vmalloc rbtree (`alloc_vmap_area`).
2. **Installs kernel PTEs** by walking `init_mm` page tables and possibly allocating intermediate PUD/PMD/PTE tables.
3. **Issues a `dsb ishst`** to publish the PTE writes (no TLB invalidate on add, but a publication barrier is required).

`kmalloc` does none of these — it just hands out an offset inside an already-mapped linear-map slab page.

---

### Q3. Why can't you `virt_to_phys()` a vmalloc pointer?  `[Nvidia]`

`virt_to_phys()` on ARM64 is implemented as `va - PAGE_OFFSET + PHYS_OFFSET` — a simple subtraction valid only inside the linear map. A vmalloc address is in `VMALLOC_START..VMALLOC_END`, far above `PAGE_OFFSET`; the math returns garbage. Use `vmalloc_to_page(va)` to walk `init_mm` and recover the backing `struct page *`, then `page_to_phys()`.

---

### Q4. ARM64-specific: why no TLB invalidate when `vmalloc` installs a new PTE?  `[Qualcomm]`

ARMv8 guarantees TLB entries can only cache **valid** translations. Before `vmap_pages_range_noflush`, the VA was either unmapped or guard-paged — the TLB cannot hold a translation for it. After writing the PTE and a `dsb ishst` publication barrier, any subsequent walk fills the TLB from the new PTE. No TLBI needed. (The free side is different — `vfree` must invalidate when transitioning valid → invalid.)

---

### Q5. What's `kvmalloc()` and when should you prefer it?  `[Google]`

`kvmalloc(size, gfp)` ([`mm/util.c:kvmalloc_node`](https://elixir.bootlin.com/linux/v6.6/source/mm/util.c#L612)) first tries `kmalloc(size, gfp | __GFP_NOWARN | __GFP_NORETRY)` and falls back to `vmalloc(size)` if that fails or `size > PAGE_SIZE * 2`. Use it for buffers whose size you can't bound tightly but want to be fast when small. Pair with `kvfree()` on cleanup, which auto-dispatches.

---

### Q6. Why is `vmalloc` not allowed in atomic context?  `[Qualcomm]`

It sleeps in many places: `alloc_pages_bulk_array_node` may invoke direct reclaim with `GFP_KERNEL`; `kvmalloc` for the page array may sleep; `__pmd_alloc` for intermediate page tables may sleep. There is no `GFP_ATOMIC` variant — the design assumes process context. Lockdep splats `BUG: sleeping function called from invalid context` if you try.

---

### Q7. Walk through `vmalloc_huge`'s benefit on a 64-core ARM64 server.  `[Nvidia]`

If the buffer fits as a 2 MB-aligned multiple, `vmalloc_huge` installs **PMD block** entries instead of leaf PTEs. Effects:

- One TLB entry covers 2 MB — dramatic reduction in TLB pressure for big buffers (think: large eBPF maps, ZRAM pools).
- Page-table writes drop by 512× — faster allocation.
- Cache locality of MMU walks improves.

Cost: requires order-9 (2 MB) physically contiguous chunks, which may not be available under fragmentation; `vmalloc_huge` then transparently falls back to 4 KB PTEs.

---

### Q8. Compare the page-fault behavior of vmalloc on ARM64 vs (historical) x86.  `[Google]`

On ARM64, the kernel pgd (`init_pg_dir`) is the **single global authority** for TTBR1 translations. Every CPU and every user process inherits the kernel mappings directly — no per-process copies. So after `vmap_pages_range` + `dsb ishst`, all CPUs see the new mappings; no page fault is ever taken on vmalloc addresses.

Historically x86 32-bit kept per-process page-directory copies for the kernel; the first access to a newly-vmalloc'd region would page-fault, and `do_page_fault` would call `vmalloc_fault()` to copy the missing PMD into the current process's pgd. ARM64 (and x86-64 since the unified kernel pgd) avoid this entirely.

---

### Q9. What's the lazy purge mechanism and why?  `[Nvidia]` `[Qualcomm]`

`vfree` does not synchronously tear down PTEs and flush the TLB. Instead it appends the area to a **lazy purge list**. When the total pending size crosses `lazy_max_pages()` (proportional to RAM and CPU count), `__purge_vmap_area_lazy` runs: clears all queued PTEs, issues one `flush_tlb_kernel_range()` (which on arm64 emits broadcast `TLBI VAE1IS` for the union of ranges), and returns pages to buddy. This amortizes the TLB-broadcast IPI cost across many frees — hugely beneficial on big multi-core arm64 systems where a per-free TLB broadcast would be a scalability disaster.

---

### Q10. KASAN-VMALLOC — what's the cost and what does it catch?  `[Google]`

`CONFIG_KASAN_VMALLOC=y` allocates shadow PTEs at `KASAN_SHADOW_OFFSET + (vmalloc_va >> 3)` on every `vmap_pages_range`. Cost: ~12.5% extra memory for the vmalloc area, plus modest CPU on alloc/free. Catches: OOB and UAF accesses **inside vmalloc'd buffers** — which plain KASAN-GENERIC (linear-map-only) misses, because the shadow for vmalloc addresses doesn't exist without it. Essential for shipping arm64 server kernels that hold large eBPF maps, IO buffers, etc.

---

### Q11. A driver does `vmalloc(16 MB)` and then `dma_map_single(dev, p, 16 MB, DMA_TO_DEVICE)`. What's wrong?  `[Nvidia]` `[Qualcomm]`

`dma_map_single` assumes a **physically contiguous** kernel-linear-map buffer. The vmalloc pointer is virtually contiguous but **physically scattered** — the call will either WARN immediately (`CONFIG_DMA_API_DEBUG`) or silently program garbage into the device.

Correct patterns:

- Use `dma_alloc_coherent` (returns a contiguous DMA-safe buffer).
- Or build an `sg_table` by walking each vmalloc page with `vmalloc_to_page` and call `dma_map_sg`.
- Or use `dma-buf` heap APIs for large buffers in modern stacks.

---

### Q12. Why do vmalloc areas have guard pages?  `[Qualcomm]`

To turn buffer-overflow bugs into immediate translation faults instead of silent corruption of an adjacent vmalloc area. Each area is `size + 2 * PAGE_SIZE` of VA reservation, but only `size` worth of PTEs are populated; the leading and trailing pages remain unmapped. An access one byte past the end faults via `do_translation_fault` and produces a clear backtrace. Opt out with `VM_NO_GUARD` if absolutely needed (rare).

---

### Q13. Module loader and vmalloc — what's the connection?  `[Google]`

Module code/data is allocated via `module_alloc()` ([`arch/arm64/kernel/module.c`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/kernel/module.c)) which is a thin wrapper over `__vmalloc_node_range` constrained to the **MODULES** region (`MODULES_VADDR..MODULES_END`, 128 MB). This keeps modules within a relative-branch range from the kernel image text (necessary for arm64's `BL`/`B` 26-bit signed offset of ±128 MB). So when you see a `[<ffff800000123456>] my_module+0x10/0x20` in dmesg, the address is in the modules sub-region of vmalloc space.

---

### Q14. Bonus: how do KASLR and `VMALLOC_START` interact on arm64?  `[Qualcomm]`

With `CONFIG_RANDOMIZE_BASE=y`, the kernel image base is randomized within a `MODULES_VSIZE` window. `VMALLOC_START` is calculated relative to `PAGE_OFFSET + PUD_SIZE`, but the **layout shift** affects all derived addresses. You can observe the result via `dmesg | grep 'vmalloc'` at boot or via `/proc/vmallocinfo`. KASLR doesn't change vmalloc's *algorithm* — just where the area starts.

---

### Q15. Staff-level: design a 1 GB ring buffer in kernel for tracing on a memory-constrained arm64 SoC.  `[Nvidia]` `[Google]`

Key constraints: contiguous physical = nearly impossible at 1 GB on a 4 GB device; virt-contig fine for trace consumers; want to mmap into userspace too.

Approach:

1. `vmalloc_huge(SZ_1G, GFP_KERNEL)` for PMD-block mappings → best TLB profile.
2. Pass `VM_USERMAP` flag (via `__vmalloc_node_range`) so userspace can `mmap` it; expose a char device whose `mmap` calls `remap_vmalloc_range`.
3. Build per-CPU sub-rings inside the buffer; producers use only their CPU's slice — no cross-CPU cache-line bouncing in the hot path.
4. Use `CONFIG_KASAN_VMALLOC=y` only on dev kernels (cost: 128 MB of shadow).
5. Free via `vfree`; lazy purge handles the TLB flush off the hot path.

---

## Common pitfalls

| Pitfall                                                | Fix |
|--------------------------------------------------------|-----|
| `kfree` on vmalloc pointer                             | Use `vfree` / `kvfree`. |
| `virt_to_phys` / `dma_map_single` on vmalloc pointer   | Use `vmalloc_to_page` + sg list, or `dma_alloc_coherent`. |
| vmalloc in atomic context                              | Defer to a workqueue. |
| Allocating huge vmalloc on 32-bit (legacy)             | 32-bit vmalloc area is tiny (~120 MB); design around it. |
| Treating vmalloc memory as DMA-coherent                | It isn't — use the DMA API. |
