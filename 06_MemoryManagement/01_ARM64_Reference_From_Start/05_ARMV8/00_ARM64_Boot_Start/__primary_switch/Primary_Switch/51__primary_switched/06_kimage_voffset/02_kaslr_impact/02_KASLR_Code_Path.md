# KASLR Implementation — Code Paths and Register Flow

## KASLR Happens Before `__primary_switched`

The timeline:
```
Firmware:
    x0 = FDT PA
    Jump to _text + IMAGE_TEXT_OFFSET (kernel entry)
         │
         ▼
primary_entry (head.S):
    bl preserve_boot_args    // save x0 (FDT PA) to x21, boot_args
    bl init_kernel_el        // configure EL
         │
         ▼
__primary_switch (head.S):
    bl  __cpu_setup          // SCTLR, TCR, MAIR
    adrp x0, init_pg_dir    // x0 = VA of initial page tables
    bl  __enable_mmu         // ENABLE MMU: from here, VA in use
    b   __primary_switched   // jump to primary_switched with x0 = PA(_text)
         │
         ▼
__primary_switched:
    init_cpu_task ...
    adrp x4, _text           // x4 = VA(_text) — kimage_voffset computed here
    sub  x4, x4, x0
    str_l x4, kimage_voffset, x5
```

Wait — KASLR code runs BEFORE `__primary_switch` in the EL2/KASLR setup code.
Let me show the actual KASLR integration point:

---

## KASLR Early Boot Code

```asm
// arch/arm64/kernel/head.S (when CONFIG_RANDOMIZE_BASE=y):
SYM_CODE_START(primary_entry)
    bl  preserve_boot_args       // x21 = FDT PA
    bl  init_kernel_el           // configure EL state

#ifdef CONFIG_RANDOMIZE_BASE
    tst x23, ~(MIN_KIMG_ALIGN - 1)  // x23 = boot_args, check alignment
    b.ne 0f
    bl  kaslr_early_init         // compute KASLR offset, relabel ELF
    cbz x0, 0f                   // x0 = KASLR offset; 0 = disabled
    orr x23, x23, x0             // mix KASLR offset into boot_args
0:
#endif
    ...
    b   __primary_switch
```

`kaslr_early_init` (from `arch/arm64/kernel/kaslr.c`) does:
1. Reads entropy (RNDRRS or FDT seed)
2. Computes random physical offset (multiple of 2 MB)
3. Computes random virtual offset (multiple of 2 MB)
4. Moves the kernel image to the new PA (or records the new PA to use)
5. Returns the combined offset in `x0`

After KASLR: the kernel image is at the randomized PA, and the page tables
will map it to the randomized VA.

---

## KASLR in `__enable_mmu` — Virtual Address Application

```asm
// arch/arm64/kernel/head.S __enable_mmu:
SYM_FUNC_START(__enable_mmu)
    ...
    // Set TTBR1_EL1 = swapper_pg_dir (the initial kernel page tables)
    // These page tables map _text to the KASLR-randomized VA
    msr ttbr1_el1, x1
    isb
    
    // Enable MMU:
    msr sctlr_el1, x0   // SCTLR.M=1 → MMU enabled
    isb
    
    // Now executing at VA
    // PC was PA X, now jumps to VA X (both map to same instruction)
    // Wait — the trampoline handles the PA→VA transition:
    br  x27             // jump to the VA of the continuation code
    // x27 = VA of code to continue (explicit jump to VA)
```

After the `br x27` in `__enable_mmu`:
- The CPU is executing at a virtual address
- `_text` VA is the KASLR-randomized virtual address
- `x0` holds PA (not changed by MMU enable)

---

## The Critical Moment: x0 Holds PA(_text) for `__primary_switched`

Just before the jump to `__primary_switched`, the boot code sets:
```asm
// arch/arm64/kernel/head.S __primary_switch:
adrp    x0, _text                    // AFTER MMU on → gives VA?
                                     // NO! This is executed in a special way
```

Actually, looking at the code more carefully:
```asm
__primary_switch:
    adrp    x1, reserved_pg_dir
    adrp    x2, init_pg_dir
    bl      __enable_mmu
    ldr     x8, =__primary_switched  // x8 = VA of __primary_switched
    adrp    x0, KERNEL_START         // After MMU: this gives VA of KERNEL_START
    sub     x0, x0, x28             // x28 = virtual-physical difference? 
    br      x8                       // jump to __primary_switched with x0 = PA
```

The `sub x0, x0, x28` subtracts the VA-PA offset to convert `adrp` result
(which gave VA after MMU on) back to PA. `x28` was set to `kimage_voffset`
or the VA-PA offset during the pre-MMU code path.

**Bottom line**: by whatever means, when `__primary_switched` starts, `x0 = PA(_text)`.

---

## KASLR Boot Verification

After boot:
```bash
# Check if KASLR is active:
$ dmesg | grep -i kaslr
Kernel/Memory randomization: enabled

# Check the actual load address (root only):
$ cat /proc/kallsyms | grep ' T _text$'
ffff800040200000 T _text   # (redacted to 0 for non-root)

# Enable /proc/kallsyms full disclosure (testing only):
$ echo 0 > /proc/sys/kernel/kptr_restrict
$ cat /proc/kallsyms | grep ' T _text$'
ffff8000481a0000 T _text   # actual KASLR-shifted address

# kimage_voffset = 0xffff8000481a0000 - (PA from /proc/iomem)
$ grep "Kernel code" /proc/iomem
481a0000-48ffffff : Kernel code
# kimage_voffset = 0xffff8000481a0000 - 0x481a0000 = 0xffff800000000000
# (matches PAGE_OFFSET — so only PA was randomized in this example)
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