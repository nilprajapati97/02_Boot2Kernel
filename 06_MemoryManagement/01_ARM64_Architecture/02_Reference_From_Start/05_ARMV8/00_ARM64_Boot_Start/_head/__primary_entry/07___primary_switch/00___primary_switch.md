# `__primary_switch` — MMU Enable & Kernel Virtual Address Setup

**File**: `arch/arm64/kernel/head.S`
**Section**: `.idmap.text` (must be identity-mapped — starts with MMU off)
**Called from**: `primary_entry` (after `__cpu_setup`)
**Enters**: physical address space
**Exits**: virtual address space (`br x8` to `__primary_switched`)

---

## Purpose

`__primary_switch` is the **critical MMU transition bridge**. It:
1. Enables the MMU using the identity-map page tables (still at phys == virt)
2. Calls `__pi_early_map_kernel` to build the **final kernel virtual mappings**
   and apply KASLR relocations
3. Jumps to `__primary_switched` at its **virtual address** — the first
   instruction to execute in the fully mapped kernel virtual address space

This is the point of no return from physical addressing. After the `br x8`,
the kernel runs entirely at virtual addresses.

---

## Register State at Entry (from `primary_entry`)

| Register | Value |
|----------|-------|
| `x0`     | `INIT_SCTLR_EL1_MMU_ON` (returned by `__cpu_setup`) |
| `x20`    | CPU boot mode (`BOOT_CPU_MODE_EL1` or `EL2`) |
| `x21`    | FDT physical address |
| `sp`     | `early_init_stack` |

---

## The Three Page Tables in Play

```
reserved_pg_dir    ← empty/zeroed, loaded into TTBR1 temporarily
                     No kernel translations exist yet.
                     Prevents speculative accesses to stale TTBR1 mappings
                     during the MMU-on transition.

__pi_init_idmap_pg_dir  ← identity map (phys == virt) for boot code
                           Loaded into TTBR0.
                           Built by __pi_create_init_idmap earlier.
                           Covers _stext → _end at their physical addresses.

swapper_pg_dir    ← final kernel page tables (TTBR1)
                    Built by __pi_early_map_kernel (this function).
                    Maps kernel at KIMAGE_VADDR + kaslr_offset.
                    Takes over from reserved_pg_dir after map_kernel().
```

---

## Call Flow

```
primary_entry
  b __primary_switch                    [MMU is OFF]
│
└── __primary_switch
        │
        ├── [1] Load page tables into __enable_mmu arguments
        │     adrp x1, reserved_pg_dir       ← TTBR1: empty, no kern mappings
        │     adrp x2, __pi_init_idmap_pg_dir ← TTBR0: identity map
        │     (x0 already = INIT_SCTLR_EL1_MMU_ON from __cpu_setup)
        │
        ├── [2] bl __enable_mmu              arch/arm64/kernel/head.S
        │         │
        │         ├── Read ID_AA64MMFR0_EL1.TGran field
        │         │   Validate page granule size is supported
        │         │   If not → __no_granule_support (wfe/wfi loop, hang)
        │         │
        │         ├── msr TTBR0_EL1 = __pi_init_idmap_pg_dir (phys)
        │         │     Identity map active on TTBR0
        │         │     User VA space (0x0000...) → physical identity
        │         │
        │         ├── load_ttbr1 (msr TTBR1_EL1 = reserved_pg_dir)
        │         │     Kernel VA space (0xffff...) → empty, no translations
        │         │     Prevents speculative kernel VA fetches during transition
        │         │
        │         ├── set_sctlr_el1 x0    ← SCTLR_EL1.M = 1
        │         │     ═══════════════════════════════════════
        │         │          MMU IS NOW ON
        │         │     ═══════════════════════════════════════
        │         │     Next instruction fetch goes through MMU.
        │         │     PC is still a physical addr → identity map
        │         │     maps it as phys == virt → execution continues.
        │         │
        │         └── ret → back to __primary_switch  [MMU ON, phys==virt]
        │
        ├── [3] Reset early stack (post-MMU)
        │     adrp x1, early_init_stack
        │     mov  sp, x1           ← stack pointer refreshed
        │     mov  x29, xzr         ← clear frame pointer
        │     Reason: __enable_mmu modifies SCTLR which affects stack
        │     behavior; reset to known state before C call
        │
        ├── [4] Set up arguments for __pi_early_map_kernel
        │     mov x0, x20           ← boot_status (EL1/EL2 + flags)
        │     mov x1, x21           ← fdt (FDT physical address)
        │
        ├── [5] bl __pi_early_map_kernel   arch/arm64/kernel/pi/map_kernel.c
        │         (= early_map_kernel C function, PI-renamed)
        │         │
        │         ├── Clear BSS and init_pg_dir region (memset to 0)
        │         │
        │         ├── map_fdt(fdt)
        │         │     Add FDT to identity map so it's accessible
        │         │
        │         ├── Parse /chosen node from FDT
        │         │     init_feature_override() — CPU feature overrides
        │         │     from kernel command line
        │         │
        │         ├── Determine VA_BITS (48 or 52 based on CPU caps)
        │         │     Adjust TCR_EL1.T1SZ if 52-bit VA supported
        │         │
        │         ├── [KASLR] kaslr_early_init()
        │         │     Read KASLR seed from FDT /chosen/kaslr-seed
        │         │     Combine with physical load offset:
        │         │       kaslr_offset = pa_base % MIN_KIMG_ALIGN
        │         │                    | (seed & ~(MIN_KIMG_ALIGN - 1))
        │         │     Decides final virtual load address of kernel
        │         │
        │         ├── [LPA2] remap_idmap_for_lpa2()  [if applicable]
        │         │     Recreate idmap with LPA2-compatible PTEs before
        │         │     enabling TCR.DS for 52-bit PA support
        │         │
        │         ├── va_base = KIMAGE_VADDR + kaslr_offset
        │         │
        │         └── map_kernel(kaslr_offset, va_base - pa_base, levels)
        │                 │
        │                 ├── Maps all kernel segments into init_pg_dir:
        │                 │   Segment              Permissions
        │                 │   [_text, _stext)       RW (non-exec header)
        │                 │   [_stext, _etext)      ROX (kernel text)
        │                 │   [__start_rodata, __inittext_begin)  RO
        │                 │   [__inittext_begin, __inittext_end)  ROX
        │                 │   [__initdata_begin, __initdata_end)  RW
        │                 │   [_data, _end)         RW (data + bss)
        │                 │
        │                 ├── [RELOCATABLE / CONFIG_RANDOMIZE_BASE]
        │                 │     First pass: map text as RW (not ROX)
        │                 │     relocate_kernel(kaslr_offset)
        │                 │       Apply ELF RELA relocations in place
        │                 │       Adjusts all absolute symbol references
        │                 │       for the new virtual load address
        │                 │     Second pass: remap text as ROX
        │                 │
        │                 ├── dsb ishst  (ensure page table writes complete)
        │                 ├── idmap_cpu_replace_ttbr1(init_pg_dir)
        │                 │     Swap TTBR1 from reserved_pg_dir → init_pg_dir
        │                 │     Kernel virtual address space is NOW LIVE
        │                 │
        │                 └── memcpy swapper_pg_dir ← init_pg_dir (root PGD)
        │                     idmap_cpu_replace_ttbr1(swapper_pg_dir)
        │                     Promote to permanent swapper_pg_dir (TTBR1)
        │
        ├── [6] Load virtual address of __primary_switched
        │     ldr x8, =__primary_switched     ← absolute virtual address
        │     adrp x0, KERNEL_START           ← __pa(KERNEL_START) as arg
        │
        └── [7] br x8  → __primary_switched  [VIRTUAL ADDRESS]
                  ═══════════════════════════════════════════════
                  First instruction executed at kernel virtual address.
                  identity map (TTBR0) still active but no longer needed.
                  ═══════════════════════════════════════════════
```

---

## Why `reserved_pg_dir` First, Then Switch to `init_pg_dir`

This is a commonly asked question.

When `__enable_mmu` writes `SCTLR_EL1.M = 1`, the CPU immediately starts
translating VA → PA for every access including speculative prefetches.
If `TTBR1` already pointed to `init_pg_dir` at that moment:

- `init_pg_dir` is not yet populated (it gets built by `__pi_early_map_kernel`
  **after** MMU enable)
- A speculative fetch through TTBR1 would hit an empty PGD entry
- This could cause a translation fault or TLB pollution

Using `reserved_pg_dir` (an empty, zeroed page) as a **placeholder** ensures
that no TTBR1 translations are possible during the gap between MMU-on and
`map_kernel` completing.

```
Timeline:
  __enable_mmu:  TTBR1 = reserved_pg_dir  (empty, safe)   ← MMU ON
       ↓
  __pi_early_map_kernel: builds init_pg_dir
       ↓
  idmap_cpu_replace_ttbr1(init_pg_dir)    ← kernel VA space live
       ↓
  idmap_cpu_replace_ttbr1(swapper_pg_dir) ← permanent tables
```

---

## The `ldr x8` + `br x8` Trampoline Pattern

```asm
ldr  x8, =__primary_switched    // absolute virtual address (literal pool)
adrp x0, KERNEL_START           // physical address as arg
br   x8                         // indirect branch to virtual address
```

`bl __primary_switched` cannot be used here because:
- `bl` generates a **PC-relative** branch (±128MB range)
- At the point of `br x8`, the CPU is still running from the
  **physical address** of `__primary_switch`
- `__primary_switched` is at the **virtual address** of the kernel
  (e.g., `0xFFFF800010xxxxxx`) — potentially terabytes away in address space
- Only an indirect branch through a register (`br x8`) can span this gap

The literal pool entry `=__primary_switched` gives the absolute
virtual address, which `ldr` loads regardless of current PC.

---

## Memory Map Before and After

```
Before __primary_switch:
  TTBR0 → __pi_init_idmap_pg_dir   phys[_stext.._end] → virt == phys
  TTBR1 → (not yet active / garbage from bootloader)
  MMU: OFF

After __enable_mmu:
  TTBR0 → __pi_init_idmap_pg_dir   phys == virt (identity)
  TTBR1 → reserved_pg_dir          empty — no kernel VA translations
  MMU: ON  (running at identity-mapped physical addresses via TTBR0)

After __pi_early_map_kernel + br x8:
  TTBR0 → __pi_init_idmap_pg_dir   still active (cleaned up later)
  TTBR1 → swapper_pg_dir           full kernel virtual mappings
  PC: virtual address (0xFFFF800010...)
  Running in kernel virtual address space
```

---

## Key Design Decisions

| Decision | Reason |
|----------|--------|
| `reserved_pg_dir` as initial TTBR1 | Prevents speculative TTBR1 faults while kernel page tables are being built |
| `__enable_mmu` validates granule size | If granule not supported, CPU is parked — prevents silent corruption |
| Stack reset after `__enable_mmu` | SCTLR change invalidates pipeline; reset to known state before C call |
| `ldr x8` + `br x8` trampoline | PC-relative branch cannot reach virtual kernel address from physical identity-mapped code |
| `__pi_early_map_kernel` called with MMU ON | C function needs a working stack and MMU; called via TTBR0 identity map |
| `swapper_pg_dir` promoted at the end | `init_pg_dir` is in `.init.data` and will be freed later; `swapper_pg_dir` is the permanent kernel page table |
| KASLR relocations applied here | Only after virtual mappings are live can we patch absolute addresses |
| BSS cleared at start of `early_map_kernel` | BSS must be zero before any C code uses globals; done before `init_feature_override` |
