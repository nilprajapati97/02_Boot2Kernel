
---

## What the Kernel Does Next After `__pi_init_idmap_pg_dir` (Line 510)

The selection is in `__primary_switch`. At this point, the identity map page tables are **already built** (done earlier in `primary_entry`). Now the kernel **uses** them. Here's the step-by-step flow:

---

### Step 1: `__enable_mmu` — Turn ON the MMU

```asm
adrp    x1, reserved_pg_dir              // x1 = PA of empty page (placeholder for TTBR1)
adrp    x2, __pi_init_idmap_pg_dir       // x2 = PA of identity map tables  ← YOU ARE HERE
bl      __enable_mmu
```

Inside `__enable_mmu` (head.S):

1. **Check granule support** — reads `ID_AA64MMFR0_EL1` to verify the CPU supports the configured page size (4K/16K/64K). If not, CPU is parked forever.
2. **`msr ttbr0_el1, x2`** — loads the identity map base address into TTBR0 (lower VA range `0x0000_...`)
3. **`load_ttbr1 x1`** — loads `reserved_pg_dir` into TTBR1 (upper VA range `0xFFFF_...`). This is an empty placeholder for now.
4. **`set_sctlr_el1 x0`** — sets `SCTLR_EL1.M = 1` → **MMU turns ON**

From this moment, every instruction fetch goes through the identity map: VA `0x4000_xxxx` → PA `0x4000_xxxx`.

---

### Step 2: Re-establish the Stack (MMU is now ON)

```asm
adrp    x1, early_init_stack
mov     sp, x1
mov     x29, xzr                // frame pointer = NULL (end of call chain)
```

The stack is reset because `ADRP` now produces an **identity-mapped VA** (which equals the PA since identity map is active). The stack was used before MMU-on too, so this re-establishes it cleanly.

---

### Step 3: `__pi_early_map_kernel` — Build the Real Kernel Page Tables

```asm
mov     x0, x20                 // x0 = boot_status (EL1 or EL2 + flags)
mov     x1, x21                 // x1 = FDT physical address
bl      __pi_early_map_kernel   // → early_map_kernel() in map_kernel.c
```

This is the **big function** — it builds the proper kernel virtual address mapping. Here's what it does inside (map_kernel.c):

#### 3a. Map the FDT into identity map
```c
void *fdt_mapped = map_fdt(fdt);
```
Adds the FDT (device tree) to the identity map page tables so it can be read.

#### 3b. Clear BSS and init page tables
```c
memset(__bss_start, 0, (char *)init_pg_end - (char *)__bss_start);
```
Zeros out BSS (uninitialized globals) and the `init_pg_dir` page table region — preparing clean memory for the new page tables.

#### 3c. Parse CPU feature overrides from FDT
```c
chosen = fdt_path_offset(fdt_mapped, "/chosen");
init_feature_override(boot_status, fdt_mapped, chosen);
```
Reads command-line overrides for CPU features (e.g., `nokaslr`, `rodata=off`).

#### 3d. Determine VA bits and root level
```c
int va_bits = VA_BITS;          // e.g., 48
int root_level = 4 - CONFIG_PGTABLE_LEVELS;  // e.g., 0 for 4-level

// May reduce if hardware doesn't support LVA or LPA2
if (IS_ENABLED(CONFIG_ARM64_LPA2) && !cpu_has_lpa2()) {
    va_bits = VA_BITS_MIN;
    root_level++;
}
```

#### 3e. KASLR (Kernel Address Space Layout Randomization)
```c
if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
    u64 kaslr_seed = kaslr_early_init(fdt_mapped, chosen);
    kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);
}
```
Gets a random seed from the FDT (provided by bootloader) and computes a randomized virtual base address. The low bits come from the physical alignment; the high bits from the random seed.

#### 3f. LPA2 remapping (if needed)
If 52-bit physical addresses (LPA2) are enabled, the identity map must be rebuilt with LPA2-compatible descriptors (bits [9:8] change meaning).

#### 3g. `map_kernel()` — Build the REAL kernel page tables
```c
va_base = KIMAGE_VADDR + kaslr_offset;  // e.g., 0xFFFF_8000_1000_0000 + random
map_kernel(kaslr_offset, va_base - pa_base, root_level);
```

This is where `init_pg_dir` gets populated with **proper virtual address mappings** (NOT identity map). It maps:

| Segment | Range | Permissions | VA Space |
|---|---|---|---|
| `_text → _stext` | Header (non-executable) | RW | `0xFFFF_8000_1xxx_xxxx` |
| `_stext → _etext` | Kernel code | ROX | `0xFFFF_8000_1xxx_xxxx` |
| `__start_rodata → __inittext_begin` | Read-only data | RW (no exec) | `0xFFFF_8000_1xxx_xxxx` |
| `__inittext_begin → __inittext_end` | Init code | ROX | `0xFFFF_8000_1xxx_xxxx` |
| `__initdata_begin → __initdata_end` | Init data | RW | `0xFFFF_8000_1xxx_xxxx` |
| `_data → _end` | Kernel data | RW | `0xFFFF_8000_1xxx_xxxx` |

Then:
1. **`idmap_cpu_replace_ttbr1(init_pg_dir)`** — loads `init_pg_dir` into **TTBR1** (upper half VA). Now the kernel has two active translations: identity map in TTBR0 (lower), kernel virtual in TTBR1 (upper).
2. If relocatable: **`relocate_kernel()`** — applies ELF relocations to adjust for KASLR offset.
3. If SCS enabled: **`scs_patch()`** — patches `PACIASP` into shadow call stack instructions.
4. **Two-pass remapping** — first pass maps text as RW (for relocations/patching), second pass remaps text as ROX.
5. **Copy root to `swapper_pg_dir`** — the final permanent page table:
   ```c
   memcpy(swapper_pg_dir + va_offset, init_pg_dir, PAGE_SIZE);
   idmap_cpu_replace_ttbr1(swapper_pg_dir);
   ```

---

### Step 4: Jump to `__primary_switched` — Enter the Virtual World

```asm
ldr     x8, =__primary_switched         // x8 = VIRTUAL address of __primary_switched
adrp    x0, KERNEL_START                // x0 = PA of kernel start
br      x8                              // JUMP to virtual address!
```

**This is the critical transition**: `ldr x8, =__primary_switched` loads the **link-time virtual address** (e.g., `0xFFFF_8000_1xxx_xxxx`). The `br x8` jumps to it. The MMU translates this through TTBR1 → `swapper_pg_dir` → the physical address of the code. From this point on, the kernel runs at **virtual addresses**.

Inside `__primary_switched` (head.S):
1. **Initialize `init_task`** — sets SP to the proper kernel stack, sets `sp_el0` to the task struct
2. **Set VBAR_EL1** — loads the exception vector table address
3. **Store FDT pointer** — saves `x21` to `__fdt_pointer`
4. **Compute `kimage_voffset`** — `virtual address - physical address` offset, stored globally
5. **`set_cpu_boot_mode_flag`** — records whether booted at EL1 or EL2
6. **`finalise_el2`** — configures VHE if available
7. **`bl start_kernel`** — **enters C `start_kernel()`** — the kernel is fully running!

---

### Summary: The Complete Chain After Line 510

```
__pi_init_idmap_pg_dir (line 510)
    │
    ▼
__enable_mmu
    ├── Check granule support
    ├── TTBR0 ← identity map page tables
    ├── TTBR1 ← reserved_pg_dir (empty)
    └── SCTLR.M = 1 → MMU ON (identity map active)
    │
    ▼
__pi_early_map_kernel (C code, running with identity map)
    ├── Map FDT into identity map
    ├── Clear BSS + init_pg_dir
    ├── Parse CPU features from FDT
    ├── Compute KASLR offset
    ├── Handle LPA2 if needed
    ├── map_kernel() → build init_pg_dir with VIRTUAL addresses
    │     ├── Map 6 kernel segments into init_pg_dir
    │     ├── TTBR1 ← init_pg_dir (kernel VA now active!)
    │     ├── relocate_kernel() (apply KASLR relocations)
    │     ├── Remap text as ROX (two-pass)
    │     ├── Copy to swapper_pg_dir
    │     └── TTBR1 ← swapper_pg_dir (final page tables!)
    │
    ▼
br __primary_switched (JUMP to virtual address!)
    ├── Setup init_task, stack, per-CPU offset
    ├── Set VBAR_EL1 (exception vectors)
    ├── Store FDT pointer, kimage_voffset
    ├── finalise_el2 (VHE setup)
    └── bl start_kernel ← KERNEL IS RUNNING!
```

The identity map (`__pi_init_idmap_pg_dir`) served as a **temporary bridge** — it kept the CPU alive during the transition from physical to virtual addressing. Once `swapper_pg_dir` is loaded in TTBR1 and the kernel jumps to a virtual address, the identity map in TTBR0 is no longer needed (and is eventually cleared).