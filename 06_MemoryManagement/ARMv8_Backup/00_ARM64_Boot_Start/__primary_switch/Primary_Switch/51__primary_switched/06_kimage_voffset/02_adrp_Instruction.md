# `adrp` Instruction — How the Kernel Gets Its Own Virtual Address

## The `adrp` Instruction

`ADRP` = **Address of Page** (PC-relative). It computes:
```
result = (PC & ~0xFFF) + (imm21 << 12)
```
where imm21 is a 21-bit signed immediate encoded in the instruction.

In other words:
1. Round down `PC` to the nearest 4 KB page boundary
2. Add the signed page offset (imm21 × 4096)
3. Store the result in the destination register

**Range:** ±4 GB from the current PC (21-bit signed × 4096 = ±4 GB)

---

## `adrp x4, _text` — Decoding the Semantics

```asm
adrp    x4, _text
```

At link time, the assembler/linker fills in the 21-bit immediate so that:
```
x4 = (PC & ~0xFFF) + imm21 × 4096 = virtual_address_of(_text_page)
```

Because `_text` is the first byte of the kernel image:
- `_text` VA = start of the kernel .text section in virtual memory
- `_text` VA page = `_text` VA rounded down to 4 KB
- `x4` = VA of the page containing `_text`

Since `_text` is typically page-aligned (aligned to 2 MB by the linker script),
`x4 = _text VA exactly`.

---

## Why `adrp` Gives VA, Not PA

At the time `adrp x4, _text` executes:
- The MMU is **ON** (enabled earlier in `__primary_switch`)
- PC is executing at a **virtual address** (the kernel virtual address)
- `PC` in the `adrp` calculation is the current **virtual** PC
- Therefore `x4` = `(virtual PC & ~0xFFF) + offset` = **virtual address**

Before MMU enable, if `adrp x4, _text` were executed:
- PC would be a physical address
- `x4` would give a physical-address-based result (PA)

This is a critical distinction. The same instruction gives different results
depending on whether the MMU is active.

---

## `sub x4, x4, x0` — Computing the VA-PA Difference

```asm
sub     x4, x4, x0
```

At this point:
- `x4` = `_text` virtual address (from `adrp`)
- `x0` = `_text` physical address (passed in from boot code)
- `x4 - x0` = VA − PA = `kimage_voffset`

This is a 64-bit subtraction. On a system with:
- PA = `0x40200000`
- VA = `0xffff800040200000`

Result: `0xffff800040200000 - 0x40200000 = 0xffff800000000000`

On a KASLR system where the kernel was loaded at PA `0x70000000` but maps to
VA `0xffff800000000000 + kaslr_va_offset`:
```
kimage_voffset = (0xffff800000000000 + kaslr_va_offset) - 0x70000000
```
This is a large number but still fits in s64 (note: it's stored as signed).

---

## `adrp` vs `ldr` (PC-relative Load)

Two ways to get an address in ARM64:

**`adrp` + `:lo12:` (for data access):**
```asm
adrp    x0, my_var          // x0 = page of my_var
ldr     x1, [x0, :lo12:my_var]  // x1 = value at my_var
```

**`adrp` alone (for address computation):**
```asm
adrp    x4, _text           // x4 = page-aligned VA of _text
// If _text is page-aligned, x4 = _text VA exactly
```

**`ldr x0, =symbol` (pseudo-instruction, immediate load):**
```asm
ldr     x0, =_text          // loads address from literal pool
```
Not used here because literal pools may not be in range during early boot.

---

## The Complete Three-Instruction Sequence — Register Flow

```asm
adrp    x4, _text               // x4 = _text VA (current VA after MMU on)
                                // Uses: PC (virtual), imm21 (linker-computed)
                                // Produces: x4 = _text VA

sub     x4, x4, x0              // x4 = _text VA - _text PA
                                // Uses: x4 = _text VA, x0 = _text PA (boot input)
                                // Produces: x4 = kimage_voffset

str_l   x4, kimage_voffset, x5  // [kimage_voffset] = x4
                                // Uses: x4 = kimage_voffset, x5 = scratch
                                // Produces: kimage_voffset variable written
```

`x5` is used by `str_l` as a temporary for the `adrp + str` sequence:
```asm
// str_l x4, kimage_voffset, x5 expands to:
adrp    x5, kimage_voffset          // x5 = page of kimage_voffset
str     x4, [x5, :lo12:kimage_voffset]  // store x4 there
```

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