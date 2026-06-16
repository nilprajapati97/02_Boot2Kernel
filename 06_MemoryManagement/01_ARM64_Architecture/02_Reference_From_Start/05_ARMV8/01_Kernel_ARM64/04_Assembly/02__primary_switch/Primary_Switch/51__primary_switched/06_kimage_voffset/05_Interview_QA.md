# `kimage_voffset` — Interview Q&A

---

## Q1: What does `kimage_voffset` store and how is it computed?

**A:** `kimage_voffset` stores `VA(_text) - PA(_text)` — the offset between the
kernel's virtual and physical load addresses. It's computed in `__primary_switched`
with three instructions:
```asm
adrp    x4, _text       // x4 = VA of _text (from PC-relative addressing, MMU on)
sub     x4, x4, x0      // x4 = VA - PA (x0 = PA of _text from boot)
str_l   x4, kimage_voffset, x5  // store result
```
The result is a runtime-computed value that depends on where the bootloader placed
the kernel (for KASLR) and what virtual address the page tables map it to.

---

## Q2: What is the `adrp` instruction and why does it give a virtual address here?

**A:** `adrp` (Address of Page) computes: `(PC & ~0xFFF) + (imm21 × 4096)`. It uses
the **current PC** value. When `adrp x4, _text` executes in `__primary_switched`:
- The MMU is active (enabled earlier in `__primary_switch`)
- PC is a virtual address (the kernel's VA space)
- Therefore `adrp` computes a virtual address

Before MMU enable, the same instruction would compute a physical-address-based
result. The difference is entirely in what value PC holds (VA vs PA).

---

## Q3: Why is `kimage_voffset` signed (`s64`) instead of unsigned (`u64`)?

**A:** In principle, `VA - PA` should always be a large positive number (since VA
is in the high half of the 64-bit address space, e.g., `0xffff800000000000`, while
PA is a small positive number, e.g., `0x40000000`). Using unsigned arithmetic would
work fine numerically (two's complement wrap-around gives the right bit pattern).

The `s64` type is chosen because:
1. It allows signed arithmetic in C without undefined behavior warnings
2. Some architectures have PA > VA (not ARM64, but generic code must handle it)
3. Tools that read crash dumps use signed interpretation for proper display

In practice on ARM64, `kimage_voffset` is always a large positive value
(between `PAGE_OFFSET` and `PAGE_OFFSET + max_DRAM_size`).

---

## Q4: Why does `kimage_voffset` need to be saved in a global variable? Can't you recompute it?

**A:** Recomputation would require knowing the physical address of `_text` at the
time of recomputation. During early boot, `x0` holds that value, but:
1. `x0` is a call-argument register — it's overwritten by the first function call
2. After `start_kernel` is called, `x0` is gone
3. The only persistent record is `kimage_voffset` itself

You COULD get the PA from the page tables (walk TTBR1_EL1 → page table walk for
`_text` VA → get PA), but that's expensive (multiple memory accesses). The global
variable approach costs one load per `__pa()` call (typically L1 cache hit) and
avoids repeated page table walks.

---

## Q5: What is `__ro_after_init` and why is `kimage_voffset` marked with it?

**A:** `__ro_after_init` is a section marker that puts the variable in
`.data..ro_after_init`. After `start_kernel` calls `mark_readonly()`, the kernel
page tables change the permissions of this section to read-only. Subsequent writes
cause a page fault (Write Protect exception).

`kimage_voffset` is marked `__ro_after_init` because:
- It's written ONCE during boot (by `__primary_switched`)
- It must NOT change after boot (would corrupt all `__pa()` results)
- An attacker modifying it could redirect DMA to arbitrary physical addresses
  (a critical memory corruption primitive for exploits)

This is a defense-in-depth security measure.

---

## Q6: How does a kernel module know its own physical address? Does it use `kimage_voffset`?

**A:** No. Kernel modules are mapped into the vmalloc region (`0xffff000000000000`
on typical ARM64), NOT the linear map. `kimage_voffset` only works for addresses
in the linear map (`PAGE_OFFSET` to `PAGE_OFFSET + max_phys`).

For module addresses, the physical address is obtained from the page tables:
```c
phys_addr_t module_text_pa = page_to_phys(vmalloc_to_page(module_va));
```
`vmalloc_to_page` walks the kernel page tables (not using `kimage_voffset`) to
find the `struct page`, then `page_to_phys` gets the PA from the page frame number.

---

## Q7: What would happen if `kimage_voffset` was set to 0?

**A:** All `__pa(x)` calls would return `x` unchanged (PA = VA). This would be
catastrophically wrong:
- `__pa(kernel_stack_va)` → returns VA as if it were PA → DMA to wrong address
- Page table entries would contain wrong PAs → memory access to wrong pages
- All memory operations would be corrupted or crash immediately

Practically, the first call to `__pa()` after `start_kernel` (probably in
`setup_arch → memblock operations`) would produce a wrong physical address, and
the resulting memory access would trigger a Data Abort exception. The system would
crash before printing any message.

---

## Q8: How does `makedumpfile`/`crash` use `kimage_voffset` for post-mortem analysis?

**A:** `makedumpfile` (for generating crash dumps) and `crash` (for analyzing them)
need to convert between VA and PA in the dump file:
1. The live kernel exports `kimage_voffset` via `/proc/vmcore` or `VMCOREINFO`
2. `makedumpfile` reads it and uses it to translate VA→PA when accessing kernel structures
3. In the dump, `crash` reads `KERNELOFFSET` (computed from `kimage_voffset`) to
   establish the KASLR slide: `kaslr_offset = kimage_voffset - PAGE_OFFSET`
4. `crash` applies this offset to all symbol addresses when reading the dump

Without correct `kimage_voffset`, crash analysis would show wrong symbol names,
wrong memory contents, and be useless for debugging.

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