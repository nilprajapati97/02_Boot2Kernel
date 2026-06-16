# Security and Performance Properties of `kimage_voffset`

## Security: `__ro_after_init`

```c
// arch/arm64/mm/init.c
s64 kimage_voffset __ro_after_init;
```

The `__ro_after_init` attribute places `kimage_voffset` in a section that is:
1. **Writable at boot**: `__primary_switched` can store to it
2. **Made read-only after init**: `start_kernel` → `mark_readonly()` → `set_memory_ro()`
3. **Protected from runtime writes**: any write after boot causes a page fault

### Why This Matters

`kimage_voffset` is used by `__pa()` to convert virtual addresses to physical
addresses. If an attacker could modify `kimage_voffset`:

**Attack scenario:**
```c
// Attacker modifies kimage_voffset to a crafted value
kimage_voffset = attacker_controlled_value;

// Now any __pa() call maps to wrong PA:
dma_map_single(dev, kernel_buffer_va, size, DMA_FROM_DEVICE);
    → calls __pa(kernel_buffer_va) = kernel_buffer_va - kimage_voffset
    → returns wrong PA
    → DMA writes to attacker-controlled physical address
    → Kernel memory corruption
```

`__ro_after_init` prevents this class of attacks. After `mark_readonly()`, the
kernel page tables mark the `.data..ro_after_init` section as read-only.
Modifying `kimage_voffset` would require exploiting a page table vulnerability first.

---

## Performance: One Subtraction vs Table Lookup

ARM64 translates VA→PA in two ways:
1. **Hardware page table walk**: 3–4 memory accesses (PGD→PUD→PMD→PTE)
2. **`__pa()` software macro**: one 64-bit subtraction (`x - kimage_voffset`)

The software macro is ~100× faster for kernel addresses in the linear map.

**Benchmark context:**
- Page table walk: ~10–20 ns (if all levels cached in TLB)
- TLB miss + walk: ~100–200 ns
- `__pa(x)`: 1 cycle (single `SUB` instruction)

Performance-critical paths like DMA, network, and storage use `__pa()` millions
of times per second. The subtraction approach is critical for throughput.

---

## KASLR — `kimage_voffset` as the Runtime Slide

With KASLR (`CONFIG_RANDOMIZE_BASE=y`):
```
// arch/arm64/kernel/kaslr.c
static u64 __init kaslr_get_seed(void)
{
    // read entropy from hardware RNG (RNDRRS), or from FDT /chosen/kaslr-seed
    ...
}

// The KASLR code (in head.S / kaslr.c) computes:
// 1. Random physical offset: kaslr_phys_offset (random 2 MB-aligned PA)
// 2. Random virtual offset: kaslr_virt_offset
// 3. Kernel is loaded at: PA = _text_default_PA + kaslr_phys_offset
// 4. Kernel maps at: VA = _text_default_VA + kaslr_virt_offset

// kimage_voffset = VA - PA (unique per boot)
```

`kimage_voffset` encodes BOTH the compile-time VA-PA gap AND the runtime KASLR
randomization in a single value. No separate "KASLR offset" storage is needed
for `__pa()` — the subtraction handles everything.

**Leak resistance**: `kimage_voffset` itself is `__ro_after_init`, not exported
to userspace. Tools like `kaslr_offset()` (used for crash dumps) have restricted
access. An attacker learning `kimage_voffset` would learn the kernel base address,
defeating KASLR — hence the access restrictions.

---

## `kimage_voffset` vs KPTI Considerations

With KPTI (`CONFIG_UNMAP_KERNEL_AT_EL0`):
- User processes cannot access kernel memory
- The KPTI "trampoline" page at exception entry needs to know the kernel base
- The trampoline uses a separate `phys_to_virt_offset` stored in `ttbr1_el1` switch code

`kimage_voffset` is NOT directly accessible from user space memory maps.
After KPTI enables, the kernel linear map is unmapped from user page tables.

---

## Comparison: `kimage_voffset` on ARM64 vs `kaslr_offset` on x86_64

| Property | ARM64 `kimage_voffset` | x86_64 `kaslr_offset` |
|---|---|---|
| Type | `s64 __ro_after_init` | `u64` in header |
| Covers | Full VA-PA gap (PA offset + KASLR VA) | Just the KASLR virtual slide |
| Set by | Assembly in `__primary_switched` | C code in `extract_kernel()` |
| Scope | Entire kernel linear map | Kernel text/data only |
| Module support | Modules use different mechanism | Modules use different mechanism |
| Crash dump | Used by makedumpfile | Used by crash tool |

ARM64's single `kimage_voffset` is more elegant than x86's multiple offset variables
because ARM64's unified linear map means one offset covers all kernel addresses.

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