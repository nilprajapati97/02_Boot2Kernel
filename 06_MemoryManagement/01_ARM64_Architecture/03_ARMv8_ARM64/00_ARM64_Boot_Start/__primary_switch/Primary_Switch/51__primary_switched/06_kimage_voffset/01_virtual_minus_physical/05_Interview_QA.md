# VA Minus PA — Interview Q&A

---

## Q1: What is the value of `kimage_voffset` on a typical ARM64 system without KASLR?

**A:** Without KASLR, the kernel is loaded at PA `CONFIG_PHYS_OFFSET` (often `0x40000000`
or `0x80000000` depending on the board) and maps at VA `PAGE_OFFSET + CONFIG_PHYS_OFFSET`.

Example: `CONFIG_PHYS_OFFSET = 0x40000000`, `PAGE_OFFSET = 0xffff800000000000`:
```
_text PA = 0x40000000
_text VA = 0xffff800040000000
kimage_voffset = 0xffff800040000000 - 0x40000000 = 0xffff800000000000 = PAGE_OFFSET
```

Without KASLR, `kimage_voffset = PAGE_OFFSET` exactly. With KASLR, they differ.

---

## Q2: Why is `sub x4, x4, x0` preferred over `mov x4, #(PAGE_OFFSET)`?

**A:** Four reasons:
1. `PAGE_OFFSET` is a compile-time constant, but with KASLR the ACTUAL VA-PA offset
   differs from `PAGE_OFFSET` at runtime. Hardcoding `PAGE_OFFSET` would be wrong.
2. A 64-bit constant like `0xffff800000000000` requires multiple instructions to
   load (`mov + movk` sequence), while `sub` is a single instruction.
3. The `sub` approach works correctly regardless of where firmware places the kernel
   in physical memory — no assumption about PA value needed.
4. The same code works for any `PAGE_OFFSET` or VA layout without modification.

---

## Q3: After KASLR randomizes the kernel load address, can you tell from `kimage_voffset` where the kernel landed?

**A:** Yes. Given `kimage_voffset` and knowing `PAGE_OFFSET`:
```
kaslr_offset = kimage_voffset - PAGE_OFFSET
```
Wait — this only gives the VA randomization. For full KASLR analysis:
```
kimage_voffset = (PAGE_OFFSET + kaslr_va_offset) - (default_PA + kaslr_pa_offset)
```
From `/proc/kallsyms` you can read VA(_text) directly (if allowed). The PA can
be computed as `VA - kimage_voffset`. This is why:
1. `/proc/kallsyms` shows `0x0000000000000000` for all symbols (zeroed) to
   unprivileged users — to prevent KASLR bypass.
2. `dmesg` on recent kernels redacts memory addresses.
3. `kimage_voffset` itself is `__ro_after_init` but its value is security-sensitive.

---

## Q4: What is `virt_to_phys` and when should a driver use it instead of `__pa`?

**A:** `virt_to_phys(addr)` is the driver-facing public API for VA→PA conversion.
It calls `__virt_to_phys()` which uses `kimage_voffset`. Drivers should use
`virt_to_phys()` (not `__pa()`) because:
1. `__pa()` is a low-level kernel-internal macro — its exact implementation may change
2. `virt_to_phys()` has better documentation and is the correct API for drivers
3. With `CONFIG_DEBUG_VIRTUAL`, `virt_to_phys()` adds bounds checking

However, even `virt_to_phys()` only works for linear-map addresses. For DMA from
dynamically allocated memory:
- `kmalloc()` returns linear-map VA → `virt_to_phys()` works
- `vmalloc()` returns vmalloc-area VA → must use `dma_map_single()` or `virt_to_page()+page_to_phys()`

---

## Q5: Does `kimage_voffset` change during the lifetime of a running kernel?

**A:** No. `kimage_voffset` is set once in `__primary_switched` and is protected
by `__ro_after_init` from that point. Any write attempt after `mark_readonly()`
causes a kernel page fault. The value never changes because:
1. The kernel image doesn't move after boot (no kernel image relocation at runtime)
2. The page tables mapping the kernel don't change the kernel's PA-VA relationship
3. Kexec (kernel reboot into new kernel) uses a fresh boot sequence that sets a new value

If `kimage_voffset` could change, all outstanding `__pa()`-based DMA operations
would be invalidated, causing catastrophic memory corruption.

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