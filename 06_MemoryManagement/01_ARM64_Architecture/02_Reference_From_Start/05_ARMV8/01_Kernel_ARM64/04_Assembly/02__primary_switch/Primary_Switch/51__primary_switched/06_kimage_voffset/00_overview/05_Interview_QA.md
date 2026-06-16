# kimage_voffset Overview — Interview Q&A

---

## Q1: In one sentence, what is `kimage_voffset`?

**A:** `kimage_voffset` is the runtime-computed offset added to any kernel symbol's
virtual address to get its physical address: `PA = VA - kimage_voffset`.

---

## Q2: Why is `kimage_voffset` needed when the kernel already knows `PAGE_OFFSET`?

**A:** Before KASLR, `kimage_voffset == PAGE_OFFSET` was guaranteed, and
`PAGE_OFFSET` (a compile-time constant) sufficed. With KASLR, the kernel is
loaded at a random physical address AND mapped at a random virtual address. Both
randomizations can shift the VA-PA relationship away from the simple `PAGE_OFFSET`
constant. `kimage_voffset` captures the ACTUAL runtime VA-PA difference, which
may differ from `PAGE_OFFSET` by the KASLR slide amount.

---

## Q3: What is `PAGE_OFFSET` and how does it relate to `kimage_voffset`?

**A:**
- `PAGE_OFFSET` = compile-time constant = assumed start of kernel virtual address
  space = `0xffff800000000000` for standard 48-bit VA ARM64.
- `kimage_voffset` = runtime value = actual `VA(_text) - PA(_text)`.
- Without KASLR: `kimage_voffset == PAGE_OFFSET` (assuming kernel loads at PA 0)
- With KASLR VA shift: `kimage_voffset = PAGE_OFFSET + kaslr_va_offset - PA(_text)`
- With KASLR PA shift: `kimage_voffset = PAGE_OFFSET - kaslr_pa_offset`
- With both: `kimage_voffset = PAGE_OFFSET + kaslr_va_offset - kaslr_pa_offset`

`PAGE_OFFSET` is a useful approximation but NOT the authoritative value.

---

## Q4: Can `kimage_voffset` be negative?

**A:** Theoretically yes (if PA > VA), but in practice no for ARM64 Linux.
The kernel maps at VA `0xffff800000000000` or higher (a very large number) and
loads at a PA of at most a few GB (a small number). VA >> PA always, so
`kimage_voffset` is always a very large positive value (close to `PAGE_OFFSET`).
The `s64` type handles the case where, in two's complement representation, the
value would be "negative" if interpreted as signed — but ARM64 `__pa()` uses
unsigned-style arithmetic anyway (it subtracts, which works for both signed
and unsigned in two's complement).

---

## Q5: What could go wrong if `kimage_voffset` is set before the MMU is enabled?

**A:** If `adrp x4, _text` executed before the MMU was on:
- PC would be a physical address (e.g., `0x40200000`)
- `adrp` would compute: `(0x40200000 & ~0xFFF) + imm21 × 4096`
- This would give a PHYSICAL-address-based result, NOT the virtual address
- `x4 - x0` = `PA(_text) - PA(_text)` = 0
- `kimage_voffset = 0`
- `__pa(any_kernel_va)` would return the VA unchanged (wrong by ~0xffff800000000000)
- First `__pa()` call in `start_kernel` → catastrophic memory access

The assembly must execute AFTER MMU enable. In `__primary_switched`, the MMU was
enabled by `__enable_mmu` before this function was entered.

---

## Q6: How does `kimage_voffset` interact with `CONFIG_DEBUG_VIRTUAL`?

**A:** `CONFIG_DEBUG_VIRTUAL` adds bounds checking to `__pa()`:
```c
// arch/arm64/mm/physaddr.c (with CONFIG_DEBUG_VIRTUAL)
phys_addr_t __virt_to_phys(unsigned long x)
{
    WARN_ON(!is_linear_map_va(x));  // assert x is in linear map range
    return x - kimage_voffset;
}
```
If `__pa()` is called with a vmalloc address or userspace address (both outside
the linear map), `CONFIG_DEBUG_VIRTUAL` triggers a `WARN_ON` with a stack trace.
This catches bugs where code incorrectly calls `__pa()` on non-linear-map addresses.
Normally (without the debug config), `__pa()` silently returns garbage for such
inputs.

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