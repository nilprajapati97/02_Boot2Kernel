# ELF Relocation and Symbol Fixup During ARM64 Boot

**Context:** Why KASLR requires symbol relocation and how it's applied  
**Source:** `arch/arm64/kernel/pi/relocate.c`, linker scripts, `ELF64 ABI spec`

---

## 0. The Relocation Problem with KASLR

An ELF executable (the kernel binary) is linked with a **fixed base address**
(e.g., `0xFFFF_8000_0000_0000`). The linker resolves all symbol references to
this base address. For example:

```asm
// Compiled at VA 0xFFFF_8000_0010_0000:
ldr     x0, =sched_init       // Load address of sched_init
// Linker encodes this as:
// literal pool entry: 0xFFFF_8000_0020_5000 (VA of sched_init)
```

With KASLR, the kernel is loaded at a different address than its link base:
```
Link base VA:  0xFFFF_8000_0000_0000 (compile-time)
Actual load:   0xFFFF_8000_DE20_0000 (KASLR randomized — different!)
Offset:        +0xDE20_0000
```

The literal pool entry `0xFFFF_8000_0020_5000` is WRONG. The correct VA is
`0xFFFF_8000_0020_5000 + 0xDE20_0000 = 0xFFFF_8000_DE40_5000`.

Without fixing this, any code that uses absolute addresses (literal pool loads,
data pointers in `.rodata`, function pointer tables, etc.) would jump to or
access the WRONG VA after KASLR randomization.

---

## 1. What Needs Relocation

### Does NOT Need Relocation (Safe under KASLR)

1. **PC-relative instructions** (`adrp`, `adr`, `b`, `bl`, `cbz`, `ldr` literal):
   These are encoded as offsets from PC, not absolute addresses. They work
   correctly regardless of where the code is loaded.

2. **Position-independent code** (`__pi_` functions): Already discussed.

### DOES Need Relocation

1. **Absolute address references in code:**
   ```asm
   ldr x0, =some_symbol_absolute_va  // Literal pool with absolute VA
   ```

2. **Data sections with pointers:**
   ```c
   struct ops table[] = {
       { .fn = &function_a },   // VA of function_a in rodata
       { .fn = &function_b },
   };
   ```
   These VA values in `.rodata` are wrong after KASLR randomization.

3. **Exception vector table entries** (absolute addresses).

4. **`__bug_table`**, `__alt_instructions`, `__start_notes` etc.:
   These are tables of absolute addresses used by kernel features.

---

## 2. ELF64 Relocation Types Used by ARM64

The kernel binary includes an `.rela.dyn` section with relocation entries:

```c
// ELF64 relocation entry:
typedef struct {
    Elf64_Addr r_offset;    // VA of the location to patch
    Elf64_Xword r_info;     // Relocation type + symbol index
    Elf64_Sxword r_addend;  // Addend for the relocation
} Elf64_Rela;
```

ARM64 relocation types used in the kernel:

| Type | Encoding | Operation |
|---|---|---|
| `R_AARCH64_RELATIVE` | 1027 | `*offset = base + addend` — absolute address fixup |
| `R_AARCH64_JUMP_SLOT` | 1026 | PLT fixup (for dynamic linking — not used in kernel) |

**`R_AARCH64_RELATIVE` is the primary type for KASLR fixups:**

```
For each relocation entry where type = R_AARCH64_RELATIVE:
    location_va = r_offset + KASLR_offset    // VA of 8-byte pointer to fix
    new_value = base_va + r_addend           // base_va = kernel VA start
    *location_va = new_value                 // Patch the pointer
```

---

## 3. The Relocation Process in ARM64 Boot

### When Does Relocation Happen?

Relocation happens in `__primary_switch` **before** `__pi_early_map_kernel`,
by calling `relocate_kernel`:

```asm
// arch/arm64/kernel/head.S — __primary_switch:
...
bl      relocate_kernel          // Apply ELF relocations for KASLR
bl      __pi_early_map_kernel    // Build page tables (using correct VAs)
bl      __enable_mmu             // Enable MMU
...
```

Wait — actually the order depends on the kernel version. In some versions,
relocation is applied before MMU enable. In others, the PI code handles it.

### `relocate_kernel` Implementation

```c
// arch/arm64/kernel/pi/relocate.c
void __init relocate_kernel(unsigned long kaslr_offset)
{
    Elf64_Rela *rela = (Elf64_Rela *)__rela_start; // .rela.dyn section start
    Elf64_Rela *rela_end = (Elf64_Rela *)__rela_end;
    
    unsigned long base = (unsigned long)_text;      // PA of _text (runtime)
    
    for (; rela < rela_end; rela++) {
        unsigned long *target;
        
        if (ELF64_R_TYPE(rela->r_info) != R_AARCH64_RELATIVE)
            continue;  // Only handle RELATIVE type
        
        // Compute the physical address of the pointer to patch:
        target = (unsigned long *)(base + rela->r_offset - kimage_voffset);
        // base = runtime PA of _text
        // r_offset = compile-time VA of pointer
        // target = PA of the pointer
        
        // Patch the pointer:
        *target = base + rela->r_addend - kimage_voffset;
        // new value = runtime VA of target symbol
    }
}
```

This iterates the `.rela.dyn` table and patches every absolute pointer in the
kernel data to reflect the KASLR-adjusted VA.

---

## 4. The `.rela.dyn` Section

The linker generates `.rela.dyn` entries for every absolute address reference
in the kernel:

```bash
# Inspect kernel relocations:
$ aarch64-linux-gnu-readelf -r vmlinux | head -20

Relocation section '.rela.dyn' at offset 0x... contains N entries:
    Offset             Info          Type                  Sym. Value
ffff800080001000  0000000000000403  R_AARCH64_RELATIVE     ffff800080020000
ffff800080001008  0000000000000403  R_AARCH64_RELATIVE     ffff800080021000
...
```

Each entry tells the kernel: "At VA 0xFFFF_8000_8000_1000, there is an 8-byte
pointer. Patch it to 0xFFFF_8000_8002_0000 + KASLR_offset."

Without KASLR, all offsets are zero and the pointers are correct as-is.

---

## 5. BSS: No Relocation Needed

The `.bss` section contains zero-initialized variables. Since these are zeroed
at boot (not containing pre-computed pointers), no relocation is needed for BSS.
The BSS is zeroed in `__primary_switched` after the MMU is enabled.

---

## 6. Relocation and Cache Coherency

After `relocate_kernel` patches memory, the patched values are in physical
memory (since D-cache is off with C=0). When the MMU is enabled and the kernel
accesses these patched pointers via VAs, the PTW and CPU will read the correct
values from physical memory.

No explicit cache flush is needed because:
- D-cache is off (C=0) during patching → writes go directly to memory
- The MMU+D-cache enable happens after patching
- First reads of the patched values go to physical memory (cache miss)

---

## 7. Alternative: No Relocation with 100% PC-Relative Code

Theoretically, the kernel could be compiled entirely with PC-relative code
(no absolute addresses). Then no `.rela.dyn` section would be needed and no
relocation step would be required.

In practice, this is not fully achievable because:
1. C global arrays with function pointers (`struct ops table[] = {&fn1, &fn2}`)
   generate absolute addresses in the object file
2. Exception table entries use absolute addresses
3. Jump tables (compiler optimization) use absolute addresses
4. The kernel is not compiled with `-fno-jump-tables` and similar restrictions

So relocation is currently necessary. The kernel is compiled with enough
PC-relative code to minimize the relocation table, but not to eliminate it.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A uses 64-bit (Aarch64) ELF binaries with RELA relocations. A RELA entry encodes (offset, symbol, addend): the linker writes the relocation record; at load time, the loader computes the final address as symbol_value + addend and writes it to the offset location. For position-independent code (PIC/PIE), all absolute address references are expressed as relocations. The CPU itself is not involved in relocation; it simply executes whatever instructions and data are at the final addresses after the loader has applied all relocations.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 kernel is linked as a PIE (Position Independent Executable) when KASLR is enabled. All absolute symbol references in the kernel become RELA relocation entries in the .rela.dyn ELF section. At boot, before the MMU is enabled, __pi_relocate_kernel (arch/arm64/kernel/pi/relocate.c) iterates over every RELA entry and applies the delta (actual_load_PA - link_PA) to each relocation site. This is why all boot-path code uses the __pi_ prefix: it is position-independent and can run before relocations are applied.

### Memory Perspective (ARMv8 Memory Model)
Applying ELF relocations means modifying kernel data at physical addresses. These writes go through the D-cache (if enabled) or directly to memory (if not). After applying all relocations, the kernel performs a D-cache flush (DC CIVAC range) and an I-cache invalidation (IC IVAU range) for any text sections that were modified. This ensures the I-cache does not serve stale pre-relocation instructions. The ARMv8 memory model requires the dsb + isb sequence after I-cache maintenance to guarantee that the pipeline fetches the updated instructions.