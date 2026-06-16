# Position-Independent Code: The `__pi_` Prefix Explained

**Concept:** Why boot code needs to be position-independent  
**Source:** Multiple files in `arch/arm64/mm/pi/` and `arch/arm64/kernel/`

---

## 0. The Fundamental Problem

Consider a function in the kernel that reads a global variable:

```c
// Normal (non-PI) kernel code:
extern unsigned long some_variable;  // Compile-time VA: 0xFFFF_8000_1234_0000

void some_function(void)
{
    unsigned long v = some_variable;  // Access at VA 0xFFFF_8000_1234_0000
}
```

Compiled to:

```asm
// Non-PI assembly:
adrp    x0, some_variable            // Load page address: 0xFFFF_8000_1234_0000
ldr     x0, [x0, :lo12:some_variable]  // Load from that VA
```

**Before the MMU is enabled**, the CPU runs at physical addresses. If this code
runs at physical address `0x5000_0000`, then:
- `adrp x0, some_variable` computes `0xFFFF_8000_1234_0000` (compile-time VA)
- The CPU tries to read from `0xFFFF_8000_1234_0000`
- With MMU off: this is treated as a physical address
- Physical address `0xFFFF_8000_1234_0000` is in a different DRAM bank or
  doesn't exist → data abort / bus error → crash

**Position-independent code (PIC/PIE) solves this** by using PC-relative
addressing instead of absolute addresses.

---

## 1. How Position-Independent Code Works

With PIC compilation (`-fPIC`):

```c
// Same code, compiled as PI:
extern unsigned long some_variable;

void some_function(void)
{
    unsigned long v = some_variable;
}
```

Compiled to:

```asm
// PI assembly (using GOT for global variables):
adrp    x0, :got:some_variable       // Load GOT page (PC-relative → physical!)
ldr     x0, [x0, :got_lo12:some_variable]  // Load GOT entry (PA of variable)
ldr     x0, [x0]                     // Load the actual value
```

Or, for simple address loads:
```asm
// PI: PC-relative address computation
adrp    x0, some_variable            // BUT this computes a PA-relative address!
add     x0, x0, :lo12:some_variable  // PA of some_variable in x0
ldr     x0, [x0]                     // Load from PA — works pre-MMU!
```

**Key insight:** `adrp` is PC-relative. When the PC holds a physical address
(before MMU), `adrp` computes physical addresses. The computed address is the
physical address of the symbol, which is exactly what we want before the MMU.

---

## 2. The `__pi_` Prefix Convention

Functions with the `__pi_` prefix are compiled into a position-independent
section of the kernel:

```c
// arch/arm64/mm/pi/map_kernel.c — compiled with special flags

// The function:
void __pi_early_map_kernel(...) { ... }

// The Makefile adds these flags for files in mm/pi/:
// CFLAGS_map_kernel.o += -fpie -fPIE
// or the entire pi/ directory is compiled with PI flags
```

The `__pi_` prefix in the function name is a **convention** (not a compiler
feature) that tells other developers "this function runs pre-MMU and must not
use absolute VAs."

When the linker sees `__pi_early_map_kernel`, it places it in the kernel binary
with PC-relative addressing throughout. The function can be called from assembly
code that passes the physical address in a register, and it will work correctly.

---

## 3. Which Files Are Built as Position-Independent

```
arch/arm64/mm/pi/
    map_kernel.c        ← __pi_early_map_kernel
    map_range.c         ← __pi_map_range, __pi_create_pgd_table, etc.
    
arch/arm64/mm/
    mmu.c (partial)     ← some __pi_ functions
```

Build system:

```makefile
# arch/arm64/mm/pi/Makefile
CFLAGS_map_kernel.o  := -fpie
CFLAGS_map_range.o   := -fpie
```

Only the specific files that run pre-MMU need `-fpie`. The vast majority of
the kernel is NOT compiled as PI (it uses normal absolute addressing with
the kernel VA assumed to be valid).

---

## 4. GOT (Global Offset Table) in PI Kernel Code

For PI code accessing global variables:

```
Without MMU (pre-MMU execution):
  PC = PA of current instruction
  GOT entry = PA of global variable (since GOT is also in the kernel image at a PA)
  Dereference GOT entry → read global variable → works!

With MMU (post-MMU execution):
  PC = VA of current instruction
  GOT entry = PA of global variable... but we want VA!
```

**Post-MMU problem:** The GOT was built with physical addresses (because the
linker used compile-time addresses, and the code runs pre-MMU). After the MMU
is enabled, the GOT entries are PAs, not VAs.

**Solution:** The PI functions that run pre-MMU (like `__pi_early_map_kernel`)
are called ONLY before the MMU is enabled. After the MMU is on, only normal
(non-PI) kernel code runs. The PI functions are never called post-MMU.

---

## 5. The `__pi_kimage_voffset` and `kimage_voffset` Duality

```c
// Two versions of the same variable:
extern u64 __pi_kimage_voffset;   // PI version — used pre-MMU, accessed by PA
extern u64 kimage_voffset;        // Normal version — same address, accessed by VA post-MMU
```

Both symbols point to the same physical memory location. The difference is in
how they are accessed:

```c
// Pre-MMU (PI context):
*(unsigned long *)__pi_kimage_voffset = computed_value;
// __pi_kimage_voffset is accessed via GOT → PA → writes to the correct memory location

// Post-MMU (normal kernel context):
unsigned long offset = kimage_voffset;
// kimage_voffset is accessed via its VA → translates to same PA → reads correct value
```

The `__pi_` prefix signals which version to use in which context.

---

## 6. Stack Usage in PI Functions

PI functions can use the stack normally (the stack pointer `sp` holds a valid
physical address, set up by `primary_entry`):

```c
void __pi_some_function(void)
{
    unsigned long local_var = 42;   // Stored on stack at PA
    char buffer[256];               // Stack-allocated buffer at PA
    // These work fine — stack SP is a PA, stores/loads go to PA
}
```

What PI functions CANNOT do:
- Call normal kernel functions (those use VAs for their own globals)
- Use `printk` (uses complex data structures at kernel VAs)
- Access kernel global variables by their compile-time VAs

What PI functions CAN do:
- Use their own globals via the GOT (PA-accessible)
- Use stack variables
- Call other `__pi_` functions
- Use physical addresses passed in as arguments

---

## 7. The `RELOC_HIDE` Macro

```c
// include/linux/compiler.h
#define RELOC_HIDE(ptr, off) \
    ({ unsigned long __ptr;                             \
       __asm__("" : "=r"(__ptr) : "0"(ptr));            \
       (typeof(ptr)) (__ptr + (off)); })
```

`RELOC_HIDE` prevents the compiler from assuming a pointer's value is known
at compile time. It forces the compiler to use a runtime value (the actual
pointer). In PI contexts, this ensures the compiler generates PC-relative loads
rather than absolute address loads.

---

## 8. Summary: Pre-MMU / Post-MMU Code Classification

| Code Type | Address Model | Can Use? | When |
|---|---|---|---|
| Normal kernel (`.text`) | Compile-time VA | No (pre-MMU), Yes (post-MMU) | Post-MMU only |
| `__pi_` prefixed | GOT + PC-relative | Yes (pre-MMU), Not called post-MMU | Pre-MMU only |
| `.idmap.text` (`__idmap_text`) | PA=VA identity | Yes (both, via identity map) | Both (transition period) |
| EFI stub | Separate PI binary | Yes (pre-boot) | Pre-kernel-boot |

The three-way classification ensures that:
1. Normal kernel code runs correctly post-MMU
2. PI code runs correctly pre-MMU
3. `.idmap.text` code runs correctly during the MMU transition

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
ARMv8-A uses 64-bit (Aarch64) ELF binaries with RELA relocations. A RELA entry encodes (offset, symbol, addend): the linker writes the relocation record; at load time, the loader computes the final address as symbol_value + addend and writes it to the offset location. For position-independent code (PIC/PIE), all absolute address references are expressed as relocations. The CPU itself is not involved in relocation; it simply executes whatever instructions and data are at the final addresses after the loader has applied all relocations.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 kernel is linked as a PIE (Position Independent Executable) when KASLR is enabled. All absolute symbol references in the kernel become RELA relocation entries in the .rela.dyn ELF section. At boot, before the MMU is enabled, __pi_relocate_kernel (arch/arm64/kernel/pi/relocate.c) iterates over every RELA entry and applies the delta (actual_load_PA - link_PA) to each relocation site. This is why all boot-path code uses the __pi_ prefix: it is position-independent and can run before relocations are applied.

### Memory Perspective (ARMv8 Memory Model)
Applying ELF relocations means modifying kernel data at physical addresses. These writes go through the D-cache (if enabled) or directly to memory (if not). After applying all relocations, the kernel performs a D-cache flush (DC CIVAC range) and an I-cache invalidation (IC IVAU range) for any text sections that were modified. This ensures the I-cache does not serve stale pre-relocation instructions. The ARMv8 memory model requires the dsb + isb sequence after I-cache maintenance to guarantee that the pipeline fetches the updated instructions.