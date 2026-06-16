# Day 08 — Physical Frame Allocator (Bootmem → Bitmap)

> **Goal**: Walk the DTB memory ranges, reserve the kernel image / DTB / initrd, and expose `alloc_page()` / `free_page()` backed by a single bitmap.
>
> **Why today**: Every subsequent memory consumer (buddy on Day 9, page tables for user procs on Day 15, slab on Day 10) needs a way to obtain raw 4 KiB physical frames. Bitmap is the simplest correct allocator; buddy will sit on top.

---

## 1. Background

### 1.1 What "phys frame allocator" provides
- `phys_addr_t alloc_page(void)` — returns 4 KiB-aligned PA, or 0 on OOM.
- `void free_page(phys_addr_t pa)` — returns frame to pool.
- Internally tracks a contiguous PFN (page frame number) range with a bitmap: 1 bit per page.

### 1.2 Memory map we work with
From Day 3 FDT walk:
- RAM base = `0x4000_0000`, size = `0x2000_0000` (512 MiB) → PFN 0..131071 (relative).
- Reserved regions:
  - Kernel image: `__kernel_start_pa..__kernel_end_pa` (linker symbols).
  - DTB: from `x0` at boot + DTB totalsize.
  - initrd: from `/chosen/linux,initrd-{start,end}` if present.

### 1.3 Bitmap sizing
- 512 MiB / 4 KiB = 131072 frames → 16384 bytes = 16 KiB bitmap.
- Place in BSS (kernel image grows; that's fine).

---

## 2. Design

### 2.1 Files
```
mm/bootmem.c
mm/page_alloc.c            (the bitmap; renamed Day 9 to sit below buddy)
include/kernel/mm.h
```

### 2.2 API
```c
typedef u64 phys_addr_t;

void  mm_init(u64 ram_base, u64 ram_size);
void  mm_reserve(phys_addr_t start, u64 size);
phys_addr_t alloc_page(void);
phys_addr_t alloc_pages_contig(unsigned nr);          /* simple linear scan */
void  free_page(phys_addr_t pa);

u64   mm_total_pages(void);
u64   mm_free_pages(void);
```

---

## 3. Implementation

### 3.1 `mm/page_alloc.c`
```c
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/types.h>
#include <asm-arm64/pgtable.h>

#define MAX_FRAMES (512UL * 1024UL * 1024UL / PAGE_SIZE)   /* up to 512 MiB */

static u64 bitmap[MAX_FRAMES / 64];
static u64 ram_base_pa;
static u64 nr_frames;
static u64 free_count;

#define PFN(pa)    (((pa) - ram_base_pa) >> PAGE_SHIFT)
#define PA(pfn)    (ram_base_pa + ((u64)(pfn) << PAGE_SHIFT))

static int bit_test(u64 i)  { return (bitmap[i >> 6] >> (i & 63)) & 1; }
static void bit_set(u64 i)  { bitmap[i >> 6] |=  (1UL << (i & 63)); }
static void bit_clear(u64 i){ bitmap[i >> 6] &= ~(1UL << (i & 63)); }

void mm_init(u64 base, u64 size)
{
    ram_base_pa = base;
    nr_frames   = size >> PAGE_SHIFT;
    if (nr_frames > MAX_FRAMES) nr_frames = MAX_FRAMES;
    /* All free initially */
    for (u64 i = 0; i < nr_frames; i++) bit_clear(i);
    free_count = nr_frames;
    printk(KERN_INFO "mm: %lu frames, %lu MiB\n",
           nr_frames, (nr_frames << PAGE_SHIFT) >> 20);
}

void mm_reserve(phys_addr_t start, u64 size)
{
    u64 s = start & PAGE_MASK;
    u64 e = (start + size + PAGE_SIZE - 1) & PAGE_MASK;
    for (u64 pa = s; pa < e; pa += PAGE_SIZE) {
        u64 pfn = PFN(pa);
        if (pfn >= nr_frames) continue;
        if (!bit_test(pfn)) { bit_set(pfn); free_count--; }
    }
    printk(KERN_INFO "mm: reserved [%p..%p) (%lu KiB)\n",
           (void*)s, (void*)e, (e - s) >> 10);
}

phys_addr_t alloc_page(void)
{
    for (u64 i = 0; i < nr_frames; i++) {
        if (!bit_test(i)) {
            bit_set(i); free_count--;
            phys_addr_t pa = PA(i);
            /* Zero the frame via its linear-map VA */
            u64 *va = (u64 *)(pa + 0xffff000000000000UL);
            for (int k = 0; k < 512; k++) va[k] = 0;
            return pa;
        }
    }
    return 0;
}

phys_addr_t alloc_pages_contig(unsigned nr)
{
    for (u64 i = 0; i + nr <= nr_frames; i++) {
        unsigned ok = 1;
        for (unsigned k = 0; k < nr; k++) if (bit_test(i + k)) { ok = 0; i += k; break; }
        if (!ok) continue;
        for (unsigned k = 0; k < nr; k++) bit_set(i + k);
        free_count -= nr;
        return PA(i);
    }
    return 0;
}

void free_page(phys_addr_t pa)
{
    u64 pfn = PFN(pa);
    if (pfn >= nr_frames || !bit_test(pfn)) {
        printk(KERN_WARN "free_page: bad pa %p\n", (void *)pa);
        return;
    }
    bit_clear(pfn); free_count++;
}

u64 mm_total_pages(void){ return nr_frames; }
u64 mm_free_pages(void) { return free_count; }
```

### 3.2 Wiring into boot
```c
void kmain_post_mmu(void)
{
    u64 base, size;
    fdt_get_memory(&base, &size);
    mm_init(base, size);

    /* Reserve kernel image */
    extern char __kernel_start[], __kernel_end[];
    u64 k_start_pa = (u64)__kernel_start - 0xffff000000000000UL;
    u64 k_end_pa   = (u64)__kernel_end   - 0xffff000000000000UL;
    mm_reserve(k_start_pa, k_end_pa - k_start_pa);

    /* Reserve DTB */
    u64 dtb_pa = saved_dtb_phys;
    u32 dtb_sz = fdt_totalsize();
    mm_reserve(dtb_pa, dtb_sz);

    /* Reserve initrd if present */
    u64 ir_s, ir_e;
    if (fdt_get_initrd(&ir_s, &ir_e) == 0)
        mm_reserve(ir_s, ir_e - ir_s);

    printk(KERN_INFO "mm: %lu free / %lu total pages\n",
           mm_free_pages(), mm_total_pages());

    /* Smoke test */
    phys_addr_t p1 = alloc_page();
    phys_addr_t p2 = alloc_page();
    printk(KERN_INFO "alloc_page test: %p %p\n", (void*)p1, (void*)p2);
    free_page(p1); free_page(p2);

    gic_init(); timer_init();
    asm volatile("msr daifclr, #2");
    for (;;) asm volatile("wfi");
}
```

---

## 4. Pitfalls

1. **Off-by-one in reserve**: include the last byte's page. Use the `(start + size + PAGE_SIZE - 1) & PAGE_MASK` ceiling.
2. **Reserving kernel image**: include `.bss` and the boot stack. Use the `__kernel_end` symbol from linker.
3. **Bitmap in BSS**: BSS is zeroed by `_start`. Good — means "all free" initially is the natural state, but you still need explicit `bit_clear` for the case where MMU mapping causes accidental writes.
4. **Linear-map zeroing**: zeroing via VA requires the direct-map being set up. If you haven't mapped all RAM yet (Day 6 only mapped 1 GiB at PA `0x4000_0000`), restrict allocator to that range or extend the map.
5. **Fragmentation in `alloc_pages_contig`**: O(N·k) — fine for boot, replaced by buddy Day 9.

---

## 5. Verification

```
[INFO] mm: 131072 frames, 512 MiB
[INFO] mm: reserved [0x40080000..0x40098000) (96 KiB)
[INFO] mm: reserved [0x40000000..0x40010000) (64 KiB)      # DTB
[INFO] mm: 131032 free / 131072 total pages
[INFO] alloc_page test: 0x40000000 0x40001000             # first free PFN
```

Unit test (in-kernel): allocate 1000 pages, free in reverse, assert `mm_free_pages()` returns to original.

---

## 6. Stretch

- Maintain a `next_hint` pointer to skip already-allocated low PFNs (O(1) common case).
- Separate "boot pool" (for early ones) vs "general pool" (post-buddy).
- Print a memory ASCII map: `[KKKKK....RRR...........]`.

---

## 7. References

- Linux `mm/memblock.c` (concept), `mm/page_alloc.c` (long-term).
- *Understanding the Linux Virtual Memory Manager*, Mel Gorman — Ch. 5.
