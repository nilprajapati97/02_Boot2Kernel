Let me trace the memory initialization path through `head.S` and its key callees.  


---

## Memory Initialization Walkthrough in head.S

### Stage 1 — Before any page tables (MMU off)

**`primary_entry`** (head.S line 83)

1. **`record_mmu_state`** — Reads `SCTLR_EL1/EL2` to record whether the bootloader left the MMU on. If endianness is wrong, it fixes it by toggling `SCTLR_ELx.EE` and disables the MMU. Result stored in `x19`.

2. **`preserve_boot_args`** — Saves bootloader registers `x0–x3` (FDT pointer, etc.) into `boot_args[]`. If MMU was off, runs `dcache_inval_poc` to invalidate cache lines for that region so they aren't stale once the MMU comes up.

---

### Stage 2 — Building the initial identity map (`init_idmap_pg_dir`)

```asm
adrp  x0, __pi_init_idmap_pg_dir
mov   x1, xzr
bl    __pi_create_init_idmap         // head.S line 94
```

**`create_init_idmap`** (map_range.c) runs with MMU off and builds a **physical == virtual** (identity) map covering:

| Region | Protection |
|---|---|
| `_stext` → `__initdata_begin` | `PAGE_KERNEL_ROX` (read + exec) |
| `__initdata_begin` → `_end` | `PAGE_KERNEL` (read + write) |

The page table root is `init_idmap_pg_dir`; sub-tables follow it in memory. The return value is the next free physical address after the used page table pages.

After building, if the MMU was off (`x19 == 0`), the identity-map page tables are **cache-invalidated** (`dcache_inval_poc`) since they were written with the cache off. If the MMU was already on, those tables are **cleaned to PoC** (`dcache_clean_poc`) so they are coherent.

---

### Stage 3 — CPU/MMU register setup

```asm
bl  __cpu_setup                       // arch/arm64/mm/proc.S line 483
```

This initialises all the registers that the MMU depends on (still **not** turning the MMU on yet):

| Register | What is set |
|---|---|
| `MAIR_EL1` | Memory attribute indirection (device, normal WB, normal NC…) |
| `TCR_EL1` | Translation granule, VA size (T0SZ/T1SZ), ASID, cacheability, shareability, LPA2/HA flags |
| `TCR2_EL1` | Supplementary TCR fields (PIE, HAFT) if the CPU supports them |
| `PIRE0_EL1` / `PIR_EL1` | Permission Indirection registers if S1PIE is supported |
| TLBs | `tlbi vmalle1` + `dsb nsh` at entry to flush stale entries |

Returns `INIT_SCTLR_EL1_MMU_ON` in `x0` — the value that will switch the MMU on.

---

### Stage 4 — Enable MMU with identity map (`__primary_switch`)

```asm
b  __primary_switch                   // head.S line 508
```

```asm
adrp  x1, reserved_pg_dir            // TTBR1 (kernel VA, upper half) — placeholder
adrp  x2, __pi_init_idmap_pg_dir     // TTBR0 (identity map, lower half)
bl    __enable_mmu
```

**`__enable_mmu`** (head.S line ~443):
- Reads `ID_AA64MMFR0_EL1` to verify the configured page **granule** is actually supported; if not, parks the CPU forever (`__no_granule_support`).
- Loads `TTBR0_EL1` ← identity map root (phys == virt, allows the CPU to keep fetching the next instruction after the MMU flips on).
- Loads `TTBR1_EL1` ← `reserved_pg_dir` (dummy; kernel VA not fully mapped yet).
- Calls `set_sctlr_el1 x0` — **this is the moment the MMU turns on**.

---

### Stage 5 — Build the real kernel page tables (`early_map_kernel`)

Still in `__primary_switch`, now running with identity map active:

```asm
bl  __pi_early_map_kernel             // head.S line 518
```

**`early_map_kernel`** (map_kernel.c):

1. **`map_fdt`** — Identity-maps the FDT into `init_idmap_pg_dir` so it can be read.
2. **Clears BSS** and the initial page table buffers (`memset`).
3. Parses the command line for feature overrides (`rodata=off`, KASLR seed, etc.).
4. Handles **LPA2** (52-bit PA) if needed: calls `remap_idmap_for_lpa2` which rebuilds the identity map with LPA2-compatible descriptors before switching the LPA2 bit in TCR.
5. Calls **`map_kernel`**, which populates `init_pg_dir` with the full kernel virtual address mapping:

   | Segment | VA range | Permissions |
   |---|---|---|
   | `[_text, _stext)` | non-exec header | `PAGE_KERNEL` |
   | `[_stext, _etext)` | kernel text | `PAGE_KERNEL_ROX` |
   | `[__start_rodata, __inittext_begin)` | read-only data | `PAGE_KERNEL` |
   | `__inittext_*` | init text | `PAGE_KERNEL_ROX` |
   | `__initdata_*` | init data | `PAGE_KERNEL` |
   | `[_data, _end)` | kernel data/BSS | `PAGE_KERNEL` |

6. Switches **TTBR1** to `init_pg_dir` (via `idmap_cpu_replace_ttbr1`).
7. If `CONFIG_RELOCATABLE`: applies KASLR relocations.
8. After relocations, remaps text segments with final read-only permissions.
9. **Copies** `init_pg_dir` → `swapper_pg_dir` and switches TTBR1 to `swapper_pg_dir` — this is the **permanent kernel page table**.

---

### Stage 6 — Jump to virtual kernel (`__primary_switched`)

```asm
ldr  x8, =__primary_switched
br   x8                               // first branch to a virtual address
```

**`__primary_switched`** (head.S line ~230):
- Sets up the initial kernel stack via `init_cpu_task`.
- Sets `VBAR_EL1` to the kernel exception vector table.
- Saves FDT pointer (`__fdt_pointer`) and computes `kimage_voffset` (physical ↔ virtual offset).
- Calls `start_kernel` — all further memory setup (`paging_init`, `memblock`, slab, etc.) happens from there.

---

### Summary diagram

```
primary_entry
  ├── record_mmu_state          (check/fix endianness, note MMU state)
  ├── preserve_boot_args        (save x0-x3, invalidate cache)
  ├── create_init_idmap         (build phys==virt map → init_idmap_pg_dir)
  │     ├── cache flush/clean
  ├── init_kernel_el            (EL1/EL2 setup, SPSR/ELR for ERET)
  ├── __cpu_setup               (MAIR, TCR, TLB flush → prepare MMU regs)
  └── __primary_switch
        ├── __enable_mmu        (TTBR0=idmap, TTBR1=reserved → MMU ON)
        ├── early_map_kernel    (map_fdt, clear BSS, map_kernel)
        │     ├── create init_pg_dir segments with correct permissions
        │     ├── switch TTBR1 → init_pg_dir
        │     ├── KASLR relocations (if RELOCATABLE)
        │     └── copy to swapper_pg_dir → switch TTBR1 permanently
        └── __primary_switched  → start_kernel()
```
