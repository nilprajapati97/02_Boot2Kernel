# KASLR and `kimage_voffset` — Interview Q&A

---

## Q1: Does KASLR randomize both the physical and virtual load address of the kernel?

**A:** Yes, on ARM64 Linux with `CONFIG_RANDOMIZE_BASE=y`, KASLR randomizes:
1. **Physical address**: the kernel image is placed at a random 2 MB-aligned PA
   within available DRAM (selected during early boot before MMU enable).
2. **Virtual address**: the page tables map the kernel to a random VA within the
   linear map region (random offset within the kernel VA space).

Both randomizations are independent, which gives higher combined entropy than
VA-only KASLR (used on x86). The `kimage_voffset` value captures the net effect
of both randomizations as a single number.

---

## Q2: Can disabling KASLR simplify `__primary_switched`?

**A:** Not meaningfully. Without KASLR, `kimage_voffset = PAGE_OFFSET` always,
so you could theoretically replace `str_l x4, kimage_voffset` with a constant.
But:
1. The code uses the same `sub x4, x4, x0` computation regardless — it happens
   to produce `PAGE_OFFSET` when KASLR is off.
2. Removing the computation would require a compile-time conditional `#ifdef` and
   a different `__pa()` implementation — significant code complexity for marginal gain.
3. The one-time boot computation costs <5 cycles — completely negligible.

Linux correctly uses the runtime-computed value even without KASLR.

---

## Q3: If an attacker learns `kimage_voffset`, what can they compute?

**A:** Knowing `kimage_voffset`:
- `kaslr_offset = kimage_voffset - PAGE_OFFSET` (if VA-only KASLR)
- `PA(_text) = VA(_text) - kimage_voffset` (if they also know VA(_text))
- Physical addresses of all kernel symbols: `PA(sym) = VA(sym) - kimage_voffset`
- This completely defeats KASLR for that boot

This is why:
- `kimage_voffset` is `__ro_after_init` (prevents runtime modification)
- `kptr_restrict` hides kernel addresses from non-root
- `%p` format hash prevents address leaks in kernel messages
- `/proc/iomem` is root-only for the kernel code region

---

## Q4: How does KASLR interact with secondary CPU startup?

**A:** Secondary CPUs (brought up after primary boot) use the SAME `kimage_voffset`
that the primary CPU computed. The secondary CPU startup path:

1. Primary CPU writes `kimage_voffset` in `__primary_switched` ✓
2. Primary CPU calls `start_kernel` → `smp_init()` → `cpu_up(cpu)` for each secondary
3. Secondary CPU enters `secondary_startup` (assembly)
4. Secondary CPU reads `kimage_voffset` (already set) via `ldr x4, [x7]`
5. Secondary CPU can now use `__pa()` correctly

Secondary CPUs never recompute `kimage_voffset` — they rely on the primary's value.
This is safe because all CPUs use the same page tables (same VA→PA mapping).

---

## Q5: What is the "module area" and how does KASLR affect it?

**A:** The module area is a vmalloc sub-region specifically for kernel module text.
It's located within ±128 MB of `_etext` so that relative branch instructions
(`bl`, `b`) can reach kernel functions directly.

With KASLR, `_etext` is at a random VA → the module area base is also random
(same random offset). When a module is loaded, it's allocated within the module
area at a random position. Both the module and the kernel text are at KASLR-shifted
addresses, so relative branches between them still work (they're both shifted by
the same kernel VA offset).

`kimage_voffset` does NOT apply to module addresses (they're in vmalloc area).

---

## Q6: If KASLR is disabled via `kaslr=off` kernel parameter, when is that parameter parsed?

**A:** The `kaslr=off` parameter is parsed BEFORE `kimage_voffset` is computed.
The sequence is:
1. `kaslr_early_init()` (in head.S / kaslr.c) checks for `kaslr=off` in the FDT
   `/chosen/bootargs` and returns 0 (no KASLR offset)
2. No randomization applied → kernel stays at original PA and VA
3. `__primary_switched` computes `kimage_voffset = PAGE_OFFSET` (normal value)

The kernel cannot parse command-line parameters normally at this point (that
requires C runtime and string parsing). Instead, `kaslr_early_init` does a
raw scan of the bootargs string from the FDT to find `nokaslr` or `kaslr=off`.

---

## Q7: Does `kimage_voffset` affect how Spectre/Meltdown mitigations work?

**A:** Indirectly yes. Meltdown mitigation (KPTI = Kernel Page Table Isolation)
maintains two sets of page tables:
- User page table (trampoline): minimal kernel mappings
- Kernel page table: full kernel mappings

The trampoline code that switches page tables on exception entry uses PA-level
addressing for `TTBR1_EL1` (which must be the PA of the page table). To compute
this PA, it uses something equivalent to `kimage_voffset`: the VA→PA offset for
the page directory.

KPTI trampoline code in `arch/arm64/kernel/entry.S`:
```asm
// Switch to kernel page table:
adrp    x25, reserved_pg_dir     // VA of kernel PGD
phys_to_ttbr x25, x25            // convert VA→PA for TTBR
msr     ttbr1_el1, x25           // set kernel page table
isb
```

`phys_to_ttbr` uses `kimage_voffset` (or a cached copy in a per-CPU variable)
to convert the VA of the PGD to its PA. Without correct `kimage_voffset`, the
TTBR would be set to a wrong PA, causing immediate MMU fault on kernel entry.

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