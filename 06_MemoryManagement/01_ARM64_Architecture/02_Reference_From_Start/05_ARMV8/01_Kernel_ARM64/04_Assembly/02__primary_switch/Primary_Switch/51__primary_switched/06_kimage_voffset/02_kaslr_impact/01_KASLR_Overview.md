# KASLR — Kernel Address Space Layout Randomization

## What KASLR Does

KASLR randomizes the virtual and/or physical address at which the kernel image
is loaded. Goal: an attacker who doesn't know the kernel load address cannot
predict the locations of kernel functions, data structures, or ROP gadgets.

Without KASLR:
```
Every ARM64 Linux boot: _text always at VA 0xffff800010000000
Attacker: hardcode target address in exploit → works on all targets
```

With KASLR:
```
Boot 1: _text at VA 0xffff800040000000 (random)
Boot 2: _text at VA 0xffff800068000000 (different random)
Boot 3: _text at VA 0xffff800012000000 (different again)
Attacker: must find kernel base first (info leak needed)
```

---

## Two Dimensions of ARM64 KASLR

### 1. Physical Address Randomization

The kernel image is loaded at a random PA (multiple of 2 MB, within DRAM):
```c
// arch/arm64/kernel/kaslr.c
static u64 __init kaslr_choose_offset(u64 mem_start, u64 mem_end, u64 seed)
{
    // Choose random PA within [mem_start, mem_end] aligned to 2 MB
    u64 range = mem_end - mem_start - kernel_size;
    return mem_start + (seed % range) & ~(SZ_2M - 1);
}
```

### 2. Virtual Address Randomization

The virtual address is also randomized within the kernel's VA space:
```c
// kaslr.c - virtual address randomization
static u64 __init kaslr_virtual_offset(u64 seed)
{
    // Random offset within the linear map region
    // Multiple of 2 MB (PMD-aligned)
    return (seed & KASLR_VA_MASK) << KASLR_GRANULE_SHIFT;
}
```

Both randomizations are applied before the kernel page tables are set up.

---

## Impact on `kimage_voffset`

Without KASLR:
```
kimage_voffset = PAGE_OFFSET + 0  (no shift)
               = 0xffff800000000000
```

With PA-only KASLR (PA randomized, VA = PAGE_OFFSET + PA):
```
_text PA = 0x78000000 (random)
_text VA = PAGE_OFFSET + 0x78000000 = 0xffff800078000000
kimage_voffset = 0xffff800078000000 - 0x78000000 = 0xffff800000000000 = PAGE_OFFSET
// (PA and VA shift cancel out — kimage_voffset unchanged!)
```

With VA-only KASLR (only VA randomized, PA fixed at default):
```
_text PA = 0x40000000 (fixed)
_text VA = PAGE_OFFSET + kaslr_va_shift = 0xffff800080000000 (example)
kimage_voffset = 0xffff800080000000 - 0x40000000 = 0xffff800040000000
// (NOT == PAGE_OFFSET — kimage_voffset changes!)
```

With FULL KASLR (both PA and VA randomized independently):
```
_text PA = 0x78000000 (random)
_text VA = PAGE_OFFSET + 0xc0000000 (random VA offset, different from PA)
kimage_voffset = (PAGE_OFFSET + 0xc0000000) - 0x78000000
               = 0xffff800048000000  (unique to this boot)
```

`kimage_voffset` captures the COMBINED effect of both randomizations.

---

## Entropy Budget

How many bits of entropy does KASLR provide?

**PA randomization**: Within DRAM range (say 4 GB, max ~1 TB).
- Granule: 2 MB (minimum kernel alignment)
- 4 GB DRAM / 2 MB granule = 2048 positions → 11 bits of entropy

**VA randomization**: Within the kernel VA region (say 512 GB linear map).
- Granule: 2 MB
- 512 GB / 2 MB = 262144 positions → 18 bits of entropy

**Combined entropy**: both dimensions together → up to ~29 bits (not fully
independent, but significant).

With 29 bits of entropy, brute-force guessing requires ~500 million attempts
on average. With kernel pointer authentication (PAC) and hardware RNG seeding,
the effective entropy increases further.

---

## KASLR Seed Sources

ARM64 KASLR uses entropy from:
1. **RNDRRS instruction** (ARMv8.5-A hardware RNG): pure hardware entropy
2. **FDT `/chosen/kaslr-seed`**: firmware-provided random value
3. **`/bootargs kaslr_seed=...`**: command-line override (for testing)
4. **UEFI RNG protocol**: firmware random number service

```c
// arch/arm64/kernel/kaslr.c
static u64 __init kaslr_get_seed(void)
{
    u64 seed;
    
    // Try hardware RNG first (best entropy):
    if (arch_get_random_seed_long(&seed))
        return seed;
    
    // Fall back to FDT-provided seed:
    of_get_property(of_chosen, "kaslr-seed", ...);
    
    // Last resort: timer-based (weak entropy):
    seed = read_sysreg(cntvct_el0);
    return seed;
}
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
KASLR (Kernel Address Space Layout Randomization) on ARMv8-A is implemented by choosing a random physical load address (phys_offset) and a random virtual mapping offset (kimage_voffset) at boot time. The CPU does not know or care about randomization: it simply uses whatever address is in TTBR1_EL1 as the root of the kernel page table. The EL1 exception vector base (VBAR_EL1) is also randomized as a consequence of the kernel image being loaded at a random VA. The hardware has no KASLR-awareness.

### Kernel Perspective (Linux ARM64)
KASLR is implemented in __pi_kaslr_early_init (arch/arm64/kernel/pi/kaslr_early.c). It uses the EFI random number generator (or RNDR instruction on ARMv8.5+) to pick a random offset that is aligned to the minimum KASLR granularity (2 MB for 4 KB pages). The offset is applied by:
  1. Choosing a random VA base for the kernel image within the TTBR1_EL1 region.
  2. Updating kimage_voffset = VA - PA.
  3. Updating all ELF RELA relocation entries to reflect the new VA.
  4. Flushing the D-cache for all modified sections.

### Memory Perspective (ARMv8 Memory Model)
KASLR changes the kernel's VA layout but not its PA layout (for a given boot). The TTBR1_EL1 page tables point to the randomized kernel VA. kimage_voffset is a compile-time-unknown runtime constant used to convert between kernel PA and kernel VA: VA = PA + kimage_voffset. Because the kernel uses position-independent code (__pi_ prefix) during the relocation phase, all accesses are PA-relative until the relocations are applied. After __primary_switch, all kernel code runs at the final randomized VA.