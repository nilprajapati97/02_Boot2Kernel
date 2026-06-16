# Virtual Minus Physical — The Arithmetic Behind `kimage_voffset`

## The Fundamental Identity

For any symbol `sym` in the kernel linear map:
```
VA(sym) = PA(sym) + kimage_voffset

Rearranging:
kimage_voffset = VA(sym) - PA(sym)
```

This is computed for `sym = _text` (kernel image start), but the same offset
applies to ALL kernel linear-map addresses:
```
VA(_text) - PA(_text) = VA(_data) - PA(_data) = VA(init_stack) - PA(init_stack)
= kimage_voffset   (constant for all symbols in the kernel image)
```

This works because the kernel page tables create a LINEAR mapping (constant
offset) from PA to VA over the entire kernel image.

---

## The Computation in Detail

```asm
// At this point: MMU is ON, PC is a VA, x0 = PA(_text)
adrp    x4, _text   // Step 1: Get VA(_text) from PC-relative addressing
sub     x4, x4, x0  // Step 2: kimage_voffset = VA(_text) - PA(_text)
```

**Step 1 Analysis:**

```
Assumptions:
    _text is 2 MB aligned (linker script enforces this)
    Current PC = somewhere in _text.head section (within first 2 MB of image)
    adrp computes: (PC & ~0xFFF) + (imm21 << 12)
    The linker sets imm21 so the result = page containing _text = _text itself
    
    Result: x4 = _text virtual address
```

**Step 2 Analysis:**

```
x4 = VA(_text) = 0xffff800040200000 (example)
x0 = PA(_text) = 0x40200000 (example)

SUB x4, x4, x0:
    x4 = 0xffff800040200000 - 0x40200000
       = 0xffff800040200000
       - 0x000000040200000
       ─────────────────────
       = 0xffff800000000000  (= PAGE_OFFSET in this no-KASLR example)

Two's complement:
    0xffff800040200000 = 1111111111111111 1000000000000000 0100000000100000 0000000000000000
   -0x0000000040200000 = 0000000000000000 0000000001000000 0010000000000000 0000000000000000
   ──────────────────────────────────────────────────────────────────────────────────────────
    0xffff800000000000 = 1111111111111111 1000000000000000 0000000000000000 0000000000000000
```

---

## Why the Mapping is Linear — Page Table Structure

The kernel is mapped with LARGE pages (2 MB or 1 GB) in the linear map for
TLB efficiency. The mapping covers the entire kernel image:

```
Level 1 entry (1 GB block): covers 0x00000000 - 0x3FFFFFFF (devices, not RAM)
Level 1 entry (1 GB block): covers 0x40000000 - 0x7FFFFFFF (RAM, includes kernel)
    ↓ maps to:
    VA 0xffff800040000000 - 0xffff8000BFFFFFFF
    PA 0x40000000         - 0xBFFFFFFF

All entries in this range have: PA = VA - PAGE_OFFSET
```

Because it's a block mapping with a constant PA-VA relationship, the offset is
the same for ALL addresses within the block:
- `VA(addr) - PA(addr) = PAGE_OFFSET` (no KASLR)
- `VA(addr) - PA(addr) = kimage_voffset` (with KASLR)

---

## Two's Complement Makes It Work for All Address Ranges

Consider the subtraction on a 64-bit CPU:

```
VA = 0xffff800010000000  (kernel in upper address space)
PA = 0x10000000          (small physical address)

VA - PA in 64-bit two's complement:
    0xffff800010000000
  - 0x0000000010000000
  ─────────────────────
    0xffff800000000000

As a signed 64-bit number: -140737488355328 (negative!)
But stored in s64 and used as subtraction operand, it works:

VA - kimage_voffset = PA:
    0xffff800010000000 - 0xffff800000000000
  = 0x0000000010000000 = 0x10000000 = PA ✓

The two's complement arithmetic is self-consistent.
```

---

## The Sub Instruction — 64-bit Subtraction Properties

`sub x4, x4, x0` on ARM64:
- 64-bit unsigned subtraction (no sign extension)
- If VA > PA (always true for kernel), result = correct positive kimage_voffset
- If result were "negative" in signed terms, it would still be correct for
  subsequent subtractions (two's complement consistency)
- No flags are set (plain `sub`, not `subs`)
- No overflow check needed — the result is a valid 64-bit pattern

The key insight: arithmetic modulo 2^64 works correctly for address calculations
as long as you're consistent about interpretation. Mixing signed and unsigned is
safe here because the actual bit patterns are what matter.

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