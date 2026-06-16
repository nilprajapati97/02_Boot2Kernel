# VA-PA Conversion — Performance Analysis

## The Cost of `__pa()` and `__va()`

`__pa(va)` expands to a single `SUB` instruction:
```asm
// C: phys = __pa(virt)
// ASM:
sub  x1, x0, x7     // x7 = kimage_voffset (loaded earlier)
// Total: 1 cycle (plus 1 load for kimage_voffset if not cached)
```

`__va(pa)` expands to a single `ADD` instruction:
```asm
// C: virt = __va(phys)
// ASM:
add  x1, x0, x7     // x7 = kimage_voffset
// Total: 1 cycle (plus 1 load for kimage_voffset if not cached)
```

---

## Hardware Page Table Walk as Alternative

If instead of `kimage_voffset`, the kernel used hardware AT instructions:
```asm
// Translate VA → PA using hardware:
at      s1e1r, x0    // Address Translate, stage 1, EL1, read
mrs     x1, par_el1  // Physical Address Register = PA result
// Total: ~5-10 cycles (pipeline serialization + system register read)
```

The `at` instruction plus `mrs par_el1` takes 5-10× longer than a single
`SUB`. For millions of `__pa()` calls per second, this matters.

---

## Cache Behavior of `kimage_voffset`

`kimage_voffset` is accessed by virtually every `__pa()` call. It's a single
`s64` (8 bytes) global variable. On ARM64 Cortex-A78:

- L1 data cache line: 64 bytes
- After the first access, `kimage_voffset` is in L1 cache
- Subsequent accesses: L1 hit latency = 4 cycles
- L1 hit rate: essentially 100% (hot variable, frequent access)

Effective cost per `__pa()` call:
- 1 load (4 cycles L1) + 1 sub (1 cycle) = ~5 cycles total
- On a 2 GHz CPU: ~2.5 ns per `__pa()` call

Alternative (page table walk): ~20-50 ns per translation.

`kimage_voffset` approach is ~10× faster.

---

## Hot Path Examples Where `__pa()` Is Called

### 1. Network Stack — sk_buff DMA mapping
```c
// net/core/skbuff.c:
skb->data = kmalloc(len, GFP_KERNEL);      // VA
dma_addr = dma_map_single(dev, skb->data, len, DMA_TO_DEVICE);
    // dma_map_single → __pa(skb->data)
    // On 1 Gbps network: ~100,000 packets/sec = 100,000 __pa() calls/sec
```

### 2. Disk I/O — bio page mapping
```c
// block/bio.c:
page_pa = page_to_phys(page);              // phys_addr of buffer page
// Followed by IOMMU mapping which uses PA
// On NVMe SSD: ~1,000,000 IOPS × 4 pages/IO = 4,000,000 __pa() calls/sec
```

### 3. Page Reclaim — memory scanning
```c
// mm/page_alloc.c zone initialization:
for_each_page_in_zone(zone, page) {
    pa = page_to_phys(page);  // uses pfn_to_phys — NOT kimage_voffset
    va = __va(pa);            // USES kimage_voffset
    // ...
}
// During page reclaim: millions of pages scanned
```

---

## `kimage_voffset` vs `virt_to_phys` — User-Facing API

```c
// include/linux/mm.h
static inline phys_addr_t virt_to_phys(const volatile void *x)
{
    return __virt_to_phys((unsigned long)(x));  // wrapper for __pa()
}

static inline void *phys_to_virt(phys_addr_t x)
{
    return (void *)__phys_to_virt(x);  // wrapper for __va()
}
```

`virt_to_phys` and `phys_to_virt` are the public API. Drivers use these rather
than `__pa()` / `__va()` directly:
```c
// drivers/dma/arm-coherent-dma.c:
phys_addr_t phys = virt_to_phys(virt_buffer);  // ultimately uses kimage_voffset
```

The entire driver ecosystem depends on `kimage_voffset` being correctly set
before ANY driver code runs. Since `__primary_switched` sets it before
`start_kernel`, and all drivers initialize during `start_kernel`, the ordering
is correct.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The ARM64 kernel virtual memory map uses the upper half of the 64-bit VA space (addresses with the top T1SZ bits = 1, i.e., 0xFFFF_xxxx_xxxx_xxxx for 48-bit). TTBR1_EL1 translates these addresses. The layout from high to low is:
- 0xFFFF_FFFF_FFFF_FFFF: vmalloc region top
- kernel text/data/bss: mapped by kimage_voffset + PA
- linear map: VA = PAGE_OFFSET + PA (direct-map of all physical RAM)
- vmalloc / vmap area
- PCI I/O space / fixmap
The hardware only cares about TTBR1_EL1 root and TCR_EL1 T1SZ. All the regions above are software conventions; the CPU treats them uniformly via the page tables.

### Kernel Perspective (Linux ARM64)
kimage_voffset = (kernel_VA_start - kernel_PA_start). After KASLR, kimage_voffset is set at boot and used by:
- __phys_to_virt(pa): va = pa - PHYS_OFFSET + PAGE_OFFSET
- __virt_to_phys(va): pa = va - kimage_voffset
The kernel linear map is set up in map_kernel and map_mem (arch/arm64/mm/mmu.c). The kernel text is mapped read-only/execute, the data is read-write/no-execute. After start_kernel, paging_init() rebuilds the definitive page tables.

### Memory Perspective (ARMv8 Memory Model)
The virtual memory map is purely a software abstraction enforced by the page tables. Physically, the linear map means every byte of physical RAM has a corresponding kernel VA: VA = PAGE_OFFSET + PA. This allows the kernel to access any physical address by simple arithmetic. The kimage offset separates the kernel text/data from the linear map to allow different permissions: kernel text is mapped Execute (PXN=0) but not writable; linear map is mapped Read-Write (AP=0b01) but not executable (PXN=1, UXN=1). Both regions use Normal Inner-Shareable Write-Back Cacheable attributes (MT_NORMAL).