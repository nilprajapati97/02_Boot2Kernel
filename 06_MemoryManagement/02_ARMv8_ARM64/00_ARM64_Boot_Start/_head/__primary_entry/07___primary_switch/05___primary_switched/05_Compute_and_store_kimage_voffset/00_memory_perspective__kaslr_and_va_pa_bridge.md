# kimage_voffset — Memory Perspective: KASLR, Virtual Address Space, and the VA/PA Bridge

**Classification**: ARM64 Memory Architecture — Kernel Address Space
**Scope**: `kimage_voffset` derivation in `__primary_switched`
**Perspective**: Virtual memory layout, KASLR mechanics, address translation
**Style Reference**: Google Project Zero / AMD64 System Programming Guide

---

## 1. The Fundamental Problem: A Kernel That Doesn't Know Where It Lives

At link time, the kernel binary is built to run at a specific virtual address:
```
KIMAGE_VADDR = 0xFFFF800010000000  (typical ARM64, 48-bit VA, 4KB pages)
```

All symbols in `vmlinux` have virtual addresses relative to this base.
For example:
```
_text           = 0xFFFF800010000000  (kernel virtual base)
schedule        = 0xFFFF800010ABCDEF  (somewhere in text)
init_task       = 0xFFFF800012345678  (in data)
```

But KASLR (Kernel Address Space Layout Randomisation) **shifts the entire
kernel image** to a different virtual address at boot time. After KASLR:
```
_text           = 0xFFFF800014200000  (shifted by 0x4200000)
schedule        = 0xFFFF800014EBCDEF  (same offset from new base)
init_task       = 0xFFFF800016545678  (same offset from new base)
```

Meanwhile, the kernel is physically loaded at some physical address that is
also not fixed:
```
_text (physical) = 0x0000000080000000  (loaded by bootloader)
```

The gap between the virtual address and the physical address of `_text` is
`kimage_voffset`. Every routine that needs to convert between virtual and
physical addresses must use this value.

---

## 2. Virtual Address Space Layout (ARM64 48-bit, 4KB pages)

```
64-bit Virtual Address Space:
═══════════════════════════════════════════════════════════════════════════════

 0xFFFF_FFFF_FFFF_FFFF ┐
                       │  EL1 accessible (TTBR1_EL1)
                       │
 0xFFFF_800000000000   │  ← PAGE_OFFSET (start of linear map)
                       │     [linear mapping of all physical memory]
                       │
 0xFFFF_000000000000   │  ← VMALLOC_START
                       │     [vmalloc / ioremap space]
                       │
                       │  ... various kernel virtual regions ...
                       │
 0xFFFF_800010000000   │  ← KIMAGE_VADDR (kernel image base, pre-KASLR)
                       │     [kernel text + data + bss]
                       │
                       │  ... KASLR may shift KIMAGE_VADDR by up to 2GB ...
                       │
                       └
                       ← 64KB hole (canonical address gap)
                       ┐
                       │  EL0 accessible (TTBR0_EL1)
                       │  [user-space mappings per process]
                       │
 0x0000_000000000000   └

═══════════════════════════════════════════════════════════════════════════════
```

### Why The Gap (Canonical Hole)?

ARM64 with 48-bit VA implements "canonical addresses":
- Valid user addresses: `0x0000_0000_0000_0000` to `0x0000_FFFF_FFFF_FFFF`
- Valid kernel addresses: `0xFFFF_0000_0000_0000` to `0xFFFF_FFFF_FFFF_FFFF`
- Addresses with bits [62:48] not all equal are **non-canonical** → fault

The hardware TTBR0/TTBR1 split at bit 63 naturally creates this layout:
- Bit 63 = 0 → TTBR0 (user)
- Bit 63 = 1 → TTBR1 (kernel)

---

## 3. KASLR Offset Derivation — How the Random Shift Is Computed

KASLR is computed in `early_map_kernel` (called from `__primary_switch`):

```c
// arch/arm64/kernel/pi/map_kernel.c
u64 pa_base = (u64)&_text;                  // physical load address
u64 kaslr_offset = pa_base % MIN_KIMG_ALIGN; // low bits from physical placement

if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
    u64 kaslr_seed = kaslr_early_init(fdt, chosen);  // high bits from /chosen/kaslr-seed
    kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);
}

u64 va_base = KIMAGE_VADDR + kaslr_offset;
```

### Why Low Bits Come From Physical Placement

`MIN_KIMG_ALIGN = 2MB`. The kernel image must be mapped with 2MB block
descriptors at the PMD level for efficiency. 2MB block descriptors require
the virtual address to be 2MB-aligned.

If `pa_base % 2MB = 0x80000` (physical address is 512KB into a 2MB region),
then the virtual load address must also end in `0x80000` to use 2MB blocks.
Otherwise, we'd need 4KB page mappings for the first 2MB, wasting TLB entries.

### Why High Bits Come From a Random Seed

The high bits (bits above MIN_KIMG_ALIGN) are supplied by the bootloader
via the FDT `/chosen/kaslr-seed` property. This is a 64-bit value written
by EFI stub or u-boot from a hardware random number generator (TRNG).

```
Final kaslr_offset structure:
  Bits [20:0]  = pa_base % 2MB       (physical alignment preservation)
  Bits [63:21] = kaslr_seed[63:21]   (cryptographically random)

Effective randomisation:
  Address range: KIMAGE_VADDR to KIMAGE_VADDR + 2GB (30 bits of entropy)
  Effective entropy: ~21 bits (after alignment constraints)
  = 2097152 possible positions
```

---

## 4. kimage_voffset Computation — The Exact Instruction Sequence

```asm
// __primary_switched:
//   x0 = __pa(KERNEL_START) passed in by __primary_switch

adrp   x4, _text                 // x4 = virtual address of _text (PC-relative)
sub    x4, x4, x0                // x4 = VA(_text) - PA(KERNEL_START)
str_l  x4, kimage_voffset, x5   // kimage_voffset = x4
```

### Why `adrp x4, _text` Gives the Virtual Address

At this point, `__primary_switched` is executing from its **virtual address**
(we jumped here via `br x8` where x8 held `=__primary_switched`).
PC-relative instructions like `adrp` compute addresses relative to the
**current PC** — which is the virtual address of the executing instruction.

Therefore `adrp x4, _text` generates:
```
x4 = (current PC, 4KB-page aligned) + page_offset_of(_text)
   = VA(_text)    ← virtual address after KASLR
```

### Why x0 = `__pa(KERNEL_START)` Gives the Physical Address

Before `br x8`, `__primary_switch` executed:
```asm
adrp   x0, KERNEL_START    // x0 = physical address (was still in identity-map)
br     x8                  // jump to virtual address
```

`adrp x0, KERNEL_START` executed while the CPU was still at its identity-mapped
physical address (TTBR0 identity map). PC-relative → the computed address is
the **physical** address of `KERNEL_START`. This physical address was passed
to `__primary_switched` in x0.

### The Subtraction

```
kimage_voffset = VA(_text) - PA(KERNEL_START)
               = (KIMAGE_VADDR + kaslr_offset) - physical_load_base
```

Since `KERNEL_START == _text` (the kernel image starts at `_text`):
```
kimage_voffset = (KIMAGE_VADDR + kaslr_offset) - physical_load_base
```

---

## 5. Usage Across the Kernel

```c
// arch/arm64/include/asm/memory.h
extern u64 kimage_voffset;

// Convert kernel virtual address to physical address
#define __kimg_to_phys(addr)   ((addr) - kimage_voffset)

// Convert physical address to kernel virtual address
#define __phys_to_kimg(x)      ((unsigned long)((x) + kimage_voffset))
```

### Call Sites and Their Significance

```
Usage                               File                        Why Critical
─────────────────────────────────────────────────────────────────────────────────
virt_to_phys(kernel_sym)            arch/arm64/include/memory.h  DMA mapping
phys_to_virt(phys_addr)             arch/arm64/include/memory.h  Memory mapping
kexec setup (crash kernel)          arch/arm64/kernel/machine_kexec.c  Reboot
vmcore for kdump                    arch/arm64/kernel/vmcore_info.c   Crash dump
KVM nVHE shadow pages               arch/arm64/kvm/va_layout.c        Virtualisation
module load address computation     kernel/module/main.c              Module loading
```

### The `__ro_after_init` Protection Timeline

```
Boot timeline:

  __primary_switched:  kimage_voffset = computed_value   ← WRITE (PTE: RW)
          |
          v
  start_kernel → rest_init → kernel_init → mark_rodata_ro()
                                               |
                                               v
                                           PTE for kimage_voffset
                                           changed from RW → RO
                                           TLB shootdown (IPI to all CPUs)
                                               |
                                               v
                             kimage_voffset is READ-ONLY forever after

  Any write after mark_rodata_ro() → permission fault (SIGSEGV for kernel = BUG)
```

The `__ro_after_init` attribute places `kimage_voffset` in the
`.data..ro_after_init` section, which `mark_rodata_ro()` write-protects
after boot completes. This ensures that even if an attacker gains arbitrary
kernel write capability after boot, they cannot alter the VA/PA translation
base to redirect DMA or kexec operations.

---

## 6. Security Analysis: What an Attacker Can Do Without KASLR

Without KASLR, all kernel symbol addresses are fixed and known from the
public kernel binary:

```
Attack scenario (no KASLR):
  1. Attacker exploits a kernel bug to get arbitrary read
  2. Reads a function pointer (e.g., from a struct ops)
  3. Immediately knows the kernel base address
     (symbol_addr - known_offset_from_vmlinux)
  4. Constructs ROP chain using exact gadget addresses
  5. Overwrites return address via stack exploit
  6. Executes arbitrary code

With KASLR:
  Step 3 still works IF attacker can leak a kernel pointer.
  But KASLR forces attackers to first find a pointer leak bug,
  which is a separate, harder bug class to exploit.
```

### KASLR Effectiveness vs. Information Leaks

KASLR is defeated if any kernel virtual address is leaked to userspace:
- Uninitialized stack/heap memory returned to user
- `/proc/kallsyms` (requires CAP_SYSLOG)
- Hardware-level side channels (Spectre, Meltdown)
- Kernel message logs with pointer format strings

The kernel actively mitigates pointer leaks:
- `%pK` format specifier: prints `0x0000000000000000` if not root
- `%px`: always prints real address (for debugging only)
- `CONFIG_INIT_ON_ALLOC_DEFAULT_ON`: zero-fill allocations to prevent leaks
