# ARM64 Kernel Virtual Memory Map

**Reference:** `arch/arm64/include/asm/memory.h`, `Documentation/arm64/memory.rst`  
**Context:** Understanding where `__primary_switched` lands in the VA space

---

## 0. Why the VA Map Matters Here

When `br x8` executes with `x8 = __primary_switched` (a VA), the CPU jumps to
a **virtual address** for the first time. Understanding the kernel VA map
tells us exactly what range this VA is in, how it's mapped, and why it's safe.

---

## 1. Complete ARM64 Kernel VA Map (48-bit VA, 4KB granule)

With `VA_BITS = 48` (standard for server/desktop ARM64):

```
Virtual Address Space (48-bit, split at VA[47]):

0x0000_0000_0000_0000  ┬──────────────────────────────────────────────┐
                        │ EL0 (User Space) — 128TB                    │
                        │ TTBR0_EL1 range (VA[47] = 0)                │
                        │                                              │
                        │ User stack, heap, code, mmap                │
0x0000_7FFF_FFFF_FFFF  ┴──────────────────────────────────────────────┘
0x0000_8000_0000_0000  ┬─── VA HOLE (UNPREDICTABLE — never use) ──────┐
                        │  Translation fault if accessed               │
                        │  48-bit VA can't encode these addresses      │
0xFFFF_0000_0000_0000  ┴──────────────────────────────────────────────┘
                        ┬──────────────────────────────────────────────┐
0xFFFF_0000_0000_0000  │ EL1 (Kernel Space) — TTBR1_EL1 range        │
                        │ VA[47] = 1 → TTBR1                          │
                        ├──────────────────────────────────────────────┤
0xFFFF_0000_0000_0000  │ LINEAR MAP (physmap): 128TB                  │
                        │ = Physical memory mapped 1:1 at PAGE_OFFSET  │
                        │ PA 0x0 → VA PAGE_OFFSET (0xFFFF_0000_...)   │
                        │ PA 0x1000 → VA PAGE_OFFSET + 0x1000         │
                        │ Used by: phys_to_virt, page_to_virt         │
                        │                                              │
0xFFFF_7FFF_FFFF_FFFF  ├──────────────────────────────────────────────┤
0xFFFF_8000_0000_0000  │ VMALLOC / VMAP area: 128TB                   │
                        │ Dynamic kernel VA allocations                │
                        │ vmalloc(), vmap(), ioremap()                 │
                        │                                              │
0xFFFF_BFFF_FFFF_FFFF  ├──────────────────────────────────────────────┤
0xFFFF_C000_0000_0000  │ VMEMMAP: 4TB                                  │
                        │ Virtual array of struct page descriptors     │
                        │ vmemmap[pfn] = &mem_map[pfn]                │
                        │                                              │
0xFFFF_DFFF_FFFF_FFFF  ├──────────────────────────────────────────────┤
0xFFFF_E000_0000_0000  │ MODULES area: 128MB                          │
                        │ Loadable kernel modules (.ko files)          │
                        │ Close to kernel for BL reach (+/-128MB)     │
                        │                                              │
0xFFFF_E000_07FF_FFFF  ├──────────────────────────────────────────────┤
0xFFFF_FFFF_8000_0000  │ KERNEL IMAGE: variable size                  │
                        │ .text, .rodata, .data, .bss                 │
                        │ (this address is KASLR-randomized)          │
                        │                                              │
0xFFFF_FFFF_FFFF_FFFF  └──────────────────────────────────────────────┘
```

**Note:** These base addresses are approximate and depend on KASLR randomization.
The layout is defined in `Documentation/arm64/memory.rst` for the current kernel.

---

## 2. Key Defines in `memory.h`

```c
// arch/arm64/include/asm/memory.h

// VA space parameters:
#define VA_BITS     CONFIG_ARM64_VA_BITS     // 48 for most configs
#define PAGE_SHIFT  12                        // 4KB pages → 12-bit offset

// Kernel start of VA:
#define PAGE_OFFSET  (-(1UL << VA_BITS))
// For 48-bit: PAGE_OFFSET = -(2^48) = 0xFFFF_0000_0000_0000

// Linear map base:
#define PAGE_OFFSET  UL(0xFFFF000000000000)  // approximately

// Kernel image VA (compile-time, adjusted by KASLR):
#define KERNEL_START  _text
// _text is defined in the linker script at the virtual address
// of the first instruction in the kernel image
```

---

## 3. Where `__primary_switched` Lives

`__primary_switched` is in the kernel's `.text` section:

```
Kernel image in memory (post-KASLR example):
  PA: 0x5200_0000  →  VA: 0xFFFF_8000_0000_0000 (example with KASLR offset applied)

__primary_switched VA:
  0xFFFF_8000_0000_xxxx  (within .text section)
```

When `br x8` executes with `x8 = 0xFFFF_8000_0000_xxxx`:
- VA[47] = 1 → TTBR1_EL1 is used
- TTBR1_EL1 = `swapper_pg_dir` (just built by `__pi_early_map_kernel`)
- `swapper_pg_dir` has a PMD entry for this VA range → 2MB block entry → valid PA

---

## 4. The Linear Map — `PAGE_OFFSET`

The linear map is a direct mapping of all physical memory:

```
PA: 0x0000_0000 → VA: 0xFFFF_0000_0000_0000  (PAGE_OFFSET)
PA: 0x4000_0000 → VA: 0xFFFF_0000_4000_0000
PA: 0x8000_0000 → VA: 0xFFFF_0000_8000_0000
```

Conversion macros:
```c
#define __phys_to_virt(phys)  ((phys) + PAGE_OFFSET)
#define __virt_to_phys(virt)  ((virt) - PAGE_OFFSET)
```

The linear map is built later, in `map_mem()` called from `paging_init()` in
`start_kernel`. During early boot (up to `__primary_switched` and into early
`start_kernel`), the linear map is NOT yet built.

Code that uses `phys_to_virt()` before `paging_init()` would generate a VA in
the linear map range that is NOT yet mapped → Translation Fault.

This is why early boot code either:
1. Works with physical addresses directly
2. Uses `__pa_symbol` / PI code
3. Accesses only the kernel image range (mapped by `swapper_pg_dir`)

---

## 5. The Modules Area and BL Reach

Kernel modules (`insmod`-loaded `.ko` files) are placed at:

```
0xFFFF_E000_0000_0000 to 0xFFFF_E000_07FF_FFFF  (128MB)
```

**Why 128MB?** The ARM64 `BL` instruction (branch and link, function call) has
a ±128MB reach:

```
BL offset: 26-bit signed immediate × 4 = ±128MB
```

Modules call kernel functions using `BL`. If a module is >128MB away from the
kernel text, the `BL` offset doesn't fit, and the linker must generate a veneer
(trampoline) — adding overhead.

The modules area is placed immediately below the kernel image (within ±128MB)
to avoid veneers for all standard kernel function calls.

---

## 6. The `swapper_pg_dir` and What It Maps

After `__pi_early_map_kernel`, `swapper_pg_dir` maps:

| VA Range | PA Range | Notes |
|---|---|---|
| Kernel `.text` VA | PA of `.text` | RX permissions |
| Kernel `.rodata` VA | PA of `.rodata` | R-- permissions |
| Kernel `.data` VA | PA of `.data` | RW- permissions |
| Kernel `.bss` VA | PA of `.bss` | RW- (zero-initialized later) |
| FDT VA | PA of FDT | R-- (read-only, bootloader data) |

NOT yet mapped in `swapper_pg_dir` at this point:
- Linear map (not yet built)
- vmalloc area (not yet in use)
- vmemmap (not yet built)
- Module area (no modules loaded yet)

---

## 7. VA Map for 52-bit VA (LPA/LPA2)

On systems with `VA_BITS = 52` (ARMv8.2 FEAT_LPA2, found in newer ARM CPUs):

```
PAGE_OFFSET = -(2^52) = 0xFFF0_0000_0000_0000
Linear map:  0xFFF0_0000_0000_0000 → +4096TB of physical memory
```

The kernel VA map expands proportionally, but the fundamental structure is the
same. The `__primary_switched` VA is still in the upper half, still uses TTBR1,
and the same analysis applies.

---

## 8. VA Map in a crash Dump

When analyzing a crash with `crash vmlinux vmcore`:

```
(crash) sys

    KERNEL: vmlinux
    DUMPFILE: vmcore
    CPUS: 4
    DATE: ...
    UPTIME: 00:00:01
    LOAD AVERAGE: ...
    TASKS: 1
    NODENAME: kernel-boot
    RELEASE: 6.x.y
    VERSION: #1 SMP PREEMPT
    MACHINE: aarch64
    MEMORY: 4 GB
    PANIC: early boot crash at VA 0xffff800080012abc
```

The VA `0xffff800080012abc` tells you:
- Upper half (VA[47]=1) → kernel space
- Near `0xFFFF_8000_0000_0000` → this is the kernel image range
- `0x12abc` within the first 2MB block → early boot code (`.text` start)

This is exactly the range `__primary_switched` and `start_kernel` live in.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A uses 64-bit (Aarch64) ELF binaries with RELA relocations. A RELA entry encodes (offset, symbol, addend): the linker writes the relocation record; at load time, the loader computes the final address as symbol_value + addend and writes it to the offset location. For position-independent code (PIC/PIE), all absolute address references are expressed as relocations. The CPU itself is not involved in relocation; it simply executes whatever instructions and data are at the final addresses after the loader has applied all relocations.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 kernel is linked as a PIE (Position Independent Executable) when KASLR is enabled. All absolute symbol references in the kernel become RELA relocation entries in the .rela.dyn ELF section. At boot, before the MMU is enabled, __pi_relocate_kernel (arch/arm64/kernel/pi/relocate.c) iterates over every RELA entry and applies the delta (actual_load_PA - link_PA) to each relocation site. This is why all boot-path code uses the __pi_ prefix: it is position-independent and can run before relocations are applied.

### Memory Perspective (ARMv8 Memory Model)
Applying ELF relocations means modifying kernel data at physical addresses. These writes go through the D-cache (if enabled) or directly to memory (if not). After applying all relocations, the kernel performs a D-cache flush (DC CIVAC range) and an I-cache invalidation (IC IVAU range) for any text sections that were modified. This ensures the I-cache does not serve stale pre-relocation instructions. The ARMv8 memory model requires the dsb + isb sequence after I-cache maintenance to guarantee that the pipeline fetches the updated instructions.